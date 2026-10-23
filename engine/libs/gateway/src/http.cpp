#include <charconv>
#include <ctime>

#include <boost/json.hpp>

#include <mda/gateway/http.hpp>

namespace mda::gateway {
namespace {

Response json_response(http::status status, const boost::json::value& body) {
    Response response{status, 11};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.body() = boost::json::serialize(body);
    return response;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

Response success(const boost::json::value& data, http::status status) {
    return json_response(status, boost::json::object{{"status", "success"}, {"data", data}});
}

Response failure(http::status status, std::string_view error_type, std::string_view message) {
    return json_response(
        status,
        boost::json::object{{"status", "error"}, {"error_type", error_type}, {"message", message}});
}

std::optional<boost::json::object> json_body(const Request& request) {
    const std::string_view type = request[http::field::content_type];
    if (!type.starts_with("application/json")) {
        return std::nullopt;
    }
    boost::system::error_code error;
    boost::json::value value = boost::json::parse(request.body(), error);
    if (error || !value.is_object()) {
        return std::nullopt;
    }
    return std::move(value.as_object());
}

std::optional<std::string> string_field(const boost::json::object& object, std::string_view name) {
    const auto* value = object.if_contains(name);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return std::string(value->as_string());
}

std::optional<std::string> cookie(const Request& request, std::string_view name) {
    std::string_view header = request[http::field::cookie];
    while (!header.empty()) {
        const std::size_t semicolon = header.find(';');
        const std::string_view pair = trim(header.substr(0, semicolon));
        header =
            semicolon == std::string_view::npos ? std::string_view{} : header.substr(semicolon + 1);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
            return std::string(pair.substr(eq + 1));
        }
    }
    return std::nullopt;
}

std::string client_ip(const Request& request) {
    return std::string(request[kClientIpHeader]);
}

bool via_https(const Request& request) {
    return request[kViaHttpsHeader] == "1";
}

std::string public_host(const Request& request) {
    const std::string_view stamped = request[kPublicHostHeader];
    const std::string_view host = request[http::field::host];  // beast's string_view type
    return stamped.empty() ? std::string(host) : std::string(stamped);
}

std::string iso_utc(std::int64_t unix_seconds) {
    const auto seconds = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    char text[32];
    const std::size_t size = std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return {text, size};
}

namespace {

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> segments;
    while (!path.empty()) {
        if (path.front() == '/') {
            path.remove_prefix(1);
            continue;
        }
        const std::size_t slash = path.find('/');
        segments.push_back(path.substr(0, slash));
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash);
    }
    return segments;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string url_decode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
            hex_value(text[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            i += 2;
        } else {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

}  // namespace

std::optional<std::int64_t> param_int(const PathParams& params, std::string_view name) {
    for (const auto& [key, value] : params) {
        if (key == name) {
            std::int64_t number = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), number);
            if (error == std::errc{} && end == value.data() + value.size() && number > 0) {
                return number;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> query_param(const Request& request, std::string_view name) {
    const std::string_view target = request.target();
    const std::size_t question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view query = target.substr(question + 1);
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        if (url_decode(pair.substr(0, eq)) == name) {
            return eq == std::string_view::npos ? std::string() : url_decode(pair.substr(eq + 1));
        }
    }
    return std::nullopt;
}

void Router::add(http::verb verb, std::string_view pattern, Handler handler) {
    Route route{.verb = verb, .segments = {}, .handler = std::move(handler)};
    for (const std::string_view segment : split_path(pattern)) {
        route.segments.emplace_back(segment);
    }
    routes_.push_back(std::move(route));
}

asio::awaitable<Response> Router::dispatch(const Request& request) const {
    const std::string_view target = request.target();
    const auto segments = split_path(target.substr(0, target.find('?')));
    bool path_known = false;
    // The most specific match wins (fewest {params}), so "/chains/{u}/oi-change" beats
    // "/chains/{u}/{expiry}" whatever the registration order.
    const Route* best = nullptr;
    PathParams best_params;
    for (const Route& route : routes_) {
        if (route.segments.size() != segments.size()) {
            continue;
        }
        PathParams params;
        bool match = true;
        for (std::size_t i = 0; i < segments.size() && match; ++i) {
            const std::string& expected = route.segments[i];
            if (expected.size() > 2 && expected.front() == '{' && expected.back() == '}') {
                params.emplace_back(expected.substr(1, expected.size() - 2),
                                    std::string(segments[i]));
            } else {
                match = expected == segments[i];
            }
        }
        if (!match) {
            continue;
        }
        path_known = true;
        if (route.verb == request.method() && (best == nullptr || params.size() < best_params.size())) {
            best = &route;
            best_params = std::move(params);
        }
    }
    if (best != nullptr) {
        co_return co_await best->handler(request, best_params);
    }
    if (path_known) {
        co_return failure(http::status::method_not_allowed, "InputException", "method not allowed");
    }
    if (fallback_ && !target.starts_with("/api/") && target != "/api") {
        co_return co_await fallback_(request, {});
    }
    co_return failure(http::status::not_found, "NotFoundException", "no such route");
}

}  // namespace mda::gateway

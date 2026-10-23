#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <openssl/ssl.h>

#include <mda/kite/login.hpp>
#include <mda/kite/rest_client.hpp>

namespace mda::kite {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using Code = KiteError::Code;

constexpr std::size_t kSmallBodyLimit = 1 * 1024 * 1024;
constexpr std::size_t kInstrumentsBodyLimit = 128 * 1024 * 1024;

KiteError error(Code code, std::string message, int status = 0) {
    return KiteError{.code = code, .http_status = status, .message = std::move(message)};
}

}  // namespace

std::string to_string(const KiteError& e) {
    std::string_view name;
    switch (e.code) {
        case Code::AuthExpired:
            name = "auth expired";
            break;
        case Code::RateLimited:
            name = "rate limited";
            break;
        case Code::Network:
            name = "network error";
            break;
        case Code::Tls:
            name = "TLS error";
            break;
        case Code::Server:
            name = "server error";
            break;
        case Code::BadResponse:
            name = "bad response";
            break;
    }
    std::string text(name);
    if (e.http_status != 0) {
        text += " (HTTP " + std::to_string(e.http_status) + ")";
    }
    if (!e.message.empty()) {
        text += ": " + e.message;
    }
    return text;
}

KiteError classify_error(int http_status, std::string_view body) {
    // Kite error bodies: {"status":"error","message":"...","error_type":"TokenException"}
    std::string message;
    std::string error_type;
    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(body, parse_error);
    if (!parse_error && json.is_object()) {
        const auto& object = json.as_object();
        if (const auto* m = object.if_contains("message"); m != nullptr && m->is_string()) {
            message = std::string(m->as_string());
        }
        if (const auto* t = object.if_contains("error_type"); t != nullptr && t->is_string()) {
            error_type = std::string(t->as_string());
        }
    } else {
        message = std::string(body.substr(0, 200));
    }

    if (http_status == 403 || error_type == "TokenException") {
        return error(Code::AuthExpired, message, http_status);
    }
    if (http_status == 429) {
        return error(Code::RateLimited, message, http_status);
    }
    if (http_status >= 500) {
        return error(Code::Server, message, http_status);
    }
    return error(Code::BadResponse, message, http_status);
}

std::string quote_key(instruments::Exchange exchange, instruments::Market market,
                      std::string_view trading_symbol) {
    const bool nse = exchange == instruments::Exchange::Nse;
    const bool derivative = market == instruments::Market::Fo;
    const std::string_view prefix = derivative ? (nse ? "NFO:" : "BFO:") : (nse ? "NSE:" : "BSE:");
    return std::string(prefix) + std::string(trading_symbol);
}

std::expected<std::vector<CircuitLimit>, KiteError> parse_circuit_limits(std::string_view body) {
    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(body, parse_error);
    const boost::json::object* data = nullptr;
    if (!parse_error && json.is_object()) {
        if (const auto* d = json.as_object().if_contains("data"); d != nullptr && d->is_object()) {
            data = &d->as_object();
        }
    }
    if (data == nullptr) {
        return std::unexpected(error(Code::BadResponse, "quote: no data object"));
    }
    const auto price = [](const boost::json::object& quote, std::string_view field) {
        const auto* value = quote.if_contains(field);
        const double rupees = value == nullptr     ? 0.0
                              : value->is_double() ? value->as_double()
                              : value->is_int64()  ? static_cast<double>(value->as_int64())
                                                   : 0.0;
        return core::Price::from_raw(std::llround(rupees * core::Price::kScale));
    };
    std::vector<CircuitLimit> limits;
    for (const auto& [key, value] : *data) {
        (void)key;
        if (!value.is_object()) {
            continue;
        }
        const auto& quote = value.as_object();
        const auto* token = quote.if_contains("instrument_token");
        if (token == nullptr || !token->is_int64() || token->as_int64() <= 0) {
            continue;
        }
        limits.push_back({.instrument_token = static_cast<std::uint32_t>(token->as_int64()),
                          .lower = price(quote, "lower_circuit_limit"),
                          .upper = price(quote, "upper_circuit_limit"),
                          .last_price = price(quote, "last_price")});
    }
    return limits;
}

std::expected<std::vector<Candle>, KiteError> parse_candles(std::string_view body) {
    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(body, parse_error);
    const boost::json::array* candles = nullptr;
    if (!parse_error && json.is_object()) {
        if (const auto* d = json.as_object().if_contains("data"); d != nullptr && d->is_object()) {
            if (const auto* c = d->as_object().if_contains("candles");
                c != nullptr && c->is_array()) {
                candles = &c->as_array();
            }
        }
    }
    if (candles == nullptr) {
        return std::unexpected(error(Code::BadResponse, "historical: no candles array"));
    }
    const auto number = [](const boost::json::value& v) {
        return v.is_double()  ? v.as_double()
               : v.is_int64() ? static_cast<double>(v.as_int64())
                              : 0.0;
    };
    const auto price = [&](const boost::json::value& v) {
        return core::Price::from_raw(std::llround(number(v) * core::Price::kScale));
    };
    std::vector<Candle> out;
    out.reserve(candles->size());
    for (const auto& item : *candles) {
        if (!item.is_array() || item.as_array().size() < 6 || !item.as_array()[0].is_string()) {
            return std::unexpected(error(Code::BadResponse, "historical: malformed candle"));
        }
        const auto& c = item.as_array();
        // "2026-10-21T09:15:00+0530"
        const std::string_view text = c[0].as_string();
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, oh = 0, om = 0;
        char sign = '+';
        if (text.size() < 24 ||
            std::sscanf(std::string(text).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c%2d%2d", &y, &mo, &d,
                        &h, &mi, &s, &sign, &oh, &om) != 9) {
            return std::unexpected(
                error(Code::BadResponse, "historical: bad time " + std::string(text)));
        }
        const auto day = std::chrono::sys_days{std::chrono::year{y} / mo / d};
        const std::int64_t offset = (sign == '-' ? -1 : 1) * (oh * 3600 + om * 60);
        const std::int64_t local =
            std::chrono::duration_cast<std::chrono::seconds>(day.time_since_epoch()).count() +
            h * 3600 + mi * 60 + s;
        out.push_back({.start = local - offset,
                       .open = price(c[1]),
                       .high = price(c[2]),
                       .low = price(c[3]),
                       .close = price(c[4]),
                       .volume = std::llround(number(c[5])),
                       .oi = c.size() > 6 ? std::llround(number(c[6])) : 0});
    }
    return out;
}

std::expected<Profile, KiteError> parse_profile(std::string_view body) {
    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(body, parse_error);
    const boost::json::object* data = nullptr;
    if (!parse_error && json.is_object()) {
        if (const auto* d = json.as_object().if_contains("data"); d != nullptr && d->is_object()) {
            data = &d->as_object();
        }
    }
    if (data == nullptr) {
        return std::unexpected(error(Code::BadResponse, "profile: no data object"));
    }
    Profile profile;
    if (const auto* id = data->if_contains("user_id"); id != nullptr && id->is_string()) {
        profile.user_id = std::string(id->as_string());
    }
    if (const auto* name = data->if_contains("user_name"); name != nullptr && name->is_string()) {
        profile.user_name = std::string(name->as_string());
    }
    if (profile.user_id.empty()) {
        return std::unexpected(error(Code::BadResponse, "profile: no user_id"));
    }
    return profile;
}

std::expected<SessionToken, KiteError> parse_session_token(std::string_view body) {
    boost::system::error_code parse_error;
    const boost::json::value json = boost::json::parse(body, parse_error);
    if (!parse_error && json.is_object()) {
        if (const auto* d = json.as_object().if_contains("data"); d != nullptr && d->is_object()) {
            const auto& data = d->as_object();
            SessionToken token;
            if (const auto* t = data.if_contains("access_token"); t != nullptr && t->is_string()) {
                token.access_token = std::string(t->as_string());
            }
            if (const auto* u = data.if_contains("user_id"); u != nullptr && u->is_string()) {
                token.user_id = std::string(u->as_string());
            }
            if (!token.access_token.empty()) {
                return token;
            }
        }
    }
    return std::unexpected(error(Code::BadResponse, "session/token: no access_token"));
}

std::optional<std::filesystem::path> default_ca_file() {
    constexpr std::array<const char*, 4> candidates = {
        "/opt/homebrew/etc/openssl@3/cert.pem",  // Homebrew, Apple Silicon
        "/etc/ssl/cert.pem",                     // macOS system, Alpine
        "/etc/ssl/certs/ca-certificates.crt",    // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",      // RHEL/Fedora
    };
    for (const char* candidate : candidates) {
        std::error_code ignored;
        if (std::filesystem::exists(candidate, ignored)) {
            return std::filesystem::path(candidate);
        }
    }
    return std::nullopt;
}

std::expected<asio::ssl::context, KiteError> make_tls_context(
    const std::filesystem::path& ca_file) {
    asio::ssl::context context(asio::ssl::context::tls_client);
    boost::system::error_code ec;
    context.set_verify_mode(asio::ssl::verify_peer, ec);
    if (!ec) {
        context.load_verify_file(ca_file.string(), ec);
    }
    if (ec) {
        return std::unexpected(
            error(Code::Tls, "CA bundle " + ca_file.string() + ": " + ec.message()));
    }
    return context;
}

asio::awaitable<std::expected<std::vector<CircuitLimit>, KiteError>> RestClient::circuit_limits(
    std::span<const std::string> keys) {
    std::string target = "/quote";
    for (std::size_t i = 0; i < keys.size() && i < kMaxQuoteInstruments; ++i) {
        target += (i == 0 ? "?i=" : "&i=") + url_encode(keys[i]);
    }
    auto body = co_await get(target, 16 * 1024 * 1024);  // ~1.5 KB per instrument
    if (!body) {
        co_return std::unexpected(body.error());
    }
    co_return parse_circuit_limits(*body);
}

asio::awaitable<std::expected<std::vector<Candle>, KiteError>> RestClient::historical(
    std::uint32_t token, std::string_view interval, std::string_view from, std::string_view to,
    bool oi) {
    const std::string target = "/instruments/historical/" + std::to_string(token) + "/" +
                               std::string(interval) + "?from=" + url_encode(from) +
                               "&to=" + url_encode(to) + (oi ? "&oi=1" : "");
    auto body = co_await get(target, 64 * 1024 * 1024);  // 60 days of minutes is ~1.5 MB
    if (!body) {
        co_return std::unexpected(body.error());
    }
    co_return parse_candles(*body);
}

asio::awaitable<std::expected<Profile, KiteError>> RestClient::profile() {
    auto body = co_await get("/user/profile", kSmallBodyLimit);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    co_return parse_profile(*body);
}

asio::awaitable<std::expected<std::string, KiteError>> RestClient::instruments() {
    co_return co_await get("/instruments", kInstrumentsBodyLimit);
}

asio::awaitable<std::expected<SessionToken, KiteError>> RestClient::create_session(
    std::string_view request_token) {
    const std::string form =
        "api_key=" + url_encode(session_.api_key) + "&request_token=" + url_encode(request_token) +
        "&checksum=" + session_checksum(session_.api_key, request_token, session_.api_secret);
    auto body = co_await request(http::verb::post, "/session/token", form, kSmallBodyLimit);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    co_return parse_session_token(*body);
}

asio::awaitable<std::expected<std::string, KiteError>> RestClient::get(std::string_view target,
                                                                       std::size_t body_limit) {
    co_return co_await request(http::verb::get, target, {}, body_limit);
}

asio::awaitable<std::expected<std::string, KiteError>> RestClient::request(http::verb verb,
                                                                           std::string_view target,
                                                                           std::string form_body,
                                                                           std::size_t body_limit) {
    const auto executor = co_await asio::this_coro::executor;
    boost::system::error_code ec;
    const auto token = asio::redirect_error(asio::use_awaitable, ec);

    asio::ip::tcp::resolver resolver(executor);
    const auto endpoints = co_await resolver.async_resolve(host_, "443", token);
    if (ec) {
        co_return std::unexpected(error(Code::Network, "resolve " + host_ + ": " + ec.message()));
    }

    beast::ssl_stream<beast::tcp_stream> stream(executor, *tls_);
    // SNI: the server needs the host name to pick its certificate.
    if (SSL_ctrl(stream.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(host_.c_str())) != 1) {
        co_return std::unexpected(error(Code::Tls, "could not set SNI host name"));
    }
    // Verify the certificate is for this host, not just signed by a trusted CA.
    stream.set_verify_callback(asio::ssl::host_name_verification(host_));

    beast::get_lowest_layer(stream).expires_after(timeout_);
    co_await beast::get_lowest_layer(stream).async_connect(endpoints, token);
    if (ec) {
        co_return std::unexpected(error(Code::Network, "connect: " + ec.message()));
    }
    beast::get_lowest_layer(stream).expires_after(timeout_);
    co_await stream.async_handshake(asio::ssl::stream_base::client, token);
    if (ec) {
        co_return std::unexpected(error(Code::Tls, "handshake: " + ec.message()));
    }

    http::request<http::string_body> request{verb, target, 11};
    request.set(http::field::host, host_);
    request.set(http::field::user_agent, "md_analytics/0.1 " BOOST_BEAST_VERSION_STRING);
    request.set("X-Kite-Version", "3");
    if (!session_.access_token.empty()) {
        request.set(http::field::authorization,
                    "token " + session_.api_key + ":" + session_.access_token);
    }
    if (verb == http::verb::post) {
        request.set(http::field::content_type, "application/x-www-form-urlencoded");
        request.body() = std::move(form_body);
        request.prepare_payload();
    }

    beast::get_lowest_layer(stream).expires_after(timeout_);
    co_await http::async_write(stream, request, token);
    if (ec) {
        co_return std::unexpected(error(Code::Network, "write: " + ec.message()));
    }

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(body_limit);
    beast::get_lowest_layer(stream).expires_after(timeout_);
    co_await http::async_read(stream, buffer, parser, token);
    if (ec) {
        co_return std::unexpected(error(Code::Network, "read: " + ec.message()));
    }
    auto response = parser.release();

    // Polite TLS close; servers often just drop the connection, so errors are ignored.
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(2));
    co_await stream.async_shutdown(token);

    const int status = static_cast<int>(response.result_int());
    if (status != 200) {
        co_return std::unexpected(classify_error(status, response.body()));
    }
    co_return std::move(response.body());
}

}  // namespace mda::kite

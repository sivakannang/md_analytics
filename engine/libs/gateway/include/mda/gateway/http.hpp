#pragma once

// HTTP building blocks for mda-gateway (ADR-0013): request/response types, Kite-style
// JSON envelopes, cookies, and a tiny router.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/value.hpp>

namespace mda::gateway {

namespace asio = boost::asio;
namespace http = boost::beast::http;

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
/// Values of "{name}" segments in the matched route, e.g. {"id", "42"}.
using PathParams = std::vector<std::pair<std::string, std::string>>;
using Handler = std::function<asio::awaitable<Response>(const Request&, const PathParams&)>;

/// A path parameter as a positive integer, or nullopt.
[[nodiscard]] std::optional<std::int64_t> param_int(const PathParams& params,
                                                    std::string_view name);
/// A query-string parameter (URL-decoded), or nullopt.
[[nodiscard]] std::optional<std::string> query_param(const Request& request, std::string_view name);

/// {"status":"success","data":…}
[[nodiscard]] Response success(const boost::json::value& data,
                               http::status status = http::status::ok);
/// {"status":"error","error_type":…,"message":…}
[[nodiscard]] Response failure(http::status status, std::string_view error_type,
                               std::string_view message);

/// Parses the body as a JSON object; nullopt unless Content-Type is application/json and
/// the body is a valid object (requiring JSON also blocks plain cross-site form posts).
[[nodiscard]] std::optional<boost::json::object> json_body(const Request& request);
/// A string field of a JSON object, or nullopt.
[[nodiscard]] std::optional<std::string> string_field(const boost::json::object& object,
                                                      std::string_view name);

/// Value of cookie `name` in the request's Cookie header, if present.
[[nodiscard]] std::optional<std::string> cookie(const Request& request, std::string_view name);

/// ISO-8601 UTC ("2026-10-22T05:10:00Z") from Unix seconds.
[[nodiscard]] std::string iso_utc(std::int64_t unix_seconds);

/// Internal header the server stamps on every request with the client's address (any copy
/// sent by the client is removed first). See HttpServer and ADR-0019.
inline constexpr std::string_view kClientIpHeader = "X-Mda-Client-Ip";

/// The client address stamped by the server ("" if none).
[[nodiscard]] std::string client_ip(const Request& request);

/// Internal headers the server stamps from a local reverse proxy's X-Forwarded-Proto and
/// X-Forwarded-Host (`tailscale serve`, ADR-0027): believed only when the TCP peer is this
/// machine, and any copy sent by the client is removed first, like kClientIpHeader.
inline constexpr std::string_view kViaHttpsHeader = "X-Mda-Via-Https";
inline constexpr std::string_view kPublicHostHeader = "X-Mda-Public-Host";

/// The browser reached us over HTTPS (through the local proxy): cookies get `Secure`.
[[nodiscard]] bool via_https(const Request& request);

/// The host the browser used: the proxy's X-Forwarded-Host, else Host. For the WebSocket
/// Origin check, which must compare against what the browser saw.
[[nodiscard]] std::string public_host(const Request& request);

class Router {
public:
    /// `pattern` is a path whose segments may be "{name}" placeholders,
    /// e.g. "/api/v1/watchlists/{id}/items/{instrument_id}".
    void add(http::verb verb, std::string_view pattern, Handler handler);

    /// Handles any path outside /api that no route matches (the website, ADR-0027).
    void set_fallback(Handler handler) { fallback_ = std::move(handler); }

    /// Runs the matching handler; 404 for unknown paths, 405 for a known path with
    /// another method; paths outside /api go to the fallback when there is one.
    [[nodiscard]] asio::awaitable<Response> dispatch(const Request& request) const;

private:
    struct Route {
        http::verb verb;
        std::vector<std::string> segments;
        Handler handler;
    };
    std::vector<Route> routes_;
    Handler fallback_;
};

}  // namespace mda::gateway

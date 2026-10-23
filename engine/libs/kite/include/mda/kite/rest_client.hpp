#pragma once

// Kite REST API client over HTTPS (ADR-0011, docs/design/kite-connectivity.md §3).
//
// Coroutine-based (Boost.Asio awaitables) so it shares mda-feedd's io_context with the
// WebSocket connections. Errors are values (std::expected), never exceptions.

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/http/verb.hpp>

#include <mda/core/price.hpp>
#include <mda/instruments/contract_key.hpp>
#include <mda/kite/session.hpp>

namespace mda::kite {

namespace asio = boost::asio;

struct KiteError {
    enum class Code {
        AuthExpired,  ///< 403 / TokenException: log in again
        RateLimited,  ///< 429
        Network,      ///< DNS, connect, timeout, connection reset
        Tls,          ///< handshake or certificate verification failed
        Server,       ///< 5xx
        BadResponse,  ///< unexpected status or unparseable body
    };
    Code code;
    int http_status = 0;
    std::string message;
};

[[nodiscard]] std::string to_string(const KiteError& error);

/// Maps a non-200 response to an error, using Kite's JSON error body when present.
[[nodiscard]] KiteError classify_error(int http_status, std::string_view body);

struct Profile {
    std::string user_id;
    std::string user_name;
};

/// Parses GET /user/profile's {"status":"success","data":{...}} body.
[[nodiscard]] std::expected<Profile, KiteError> parse_profile(std::string_view body);

struct SessionToken {
    std::string access_token;
    std::string user_id;
};

/// Parses POST /session/token's {"status":"success","data":{"access_token":...}} body.
[[nodiscard]] std::expected<SessionToken, KiteError> parse_session_token(std::string_view body);

/// Today's price band of one instrument, from GET /quote.
struct CircuitLimit {
    std::uint32_t instrument_token = 0;
    core::Price lower;
    core::Price upper;
    core::Price last_price;  ///< also in the quote; seeds option-chain ATM at startup
};

/// One historical candle (GET /instruments/historical). Times are Unix seconds (the candle's
/// start); OI is 0 unless requested and applicable.
struct Candle {
    std::int64_t start = 0;
    core::Price open;
    core::Price high;
    core::Price low;
    core::Price close;
    std::int64_t volume = 0;
    std::int64_t oi = 0;
};

/// Parses {"status":"success","data":{"candles":[["2026-10-21T09:15:00+0530",o,h,l,c,v,oi?],...]}}.
[[nodiscard]] std::expected<std::vector<Candle>, KiteError> parse_candles(std::string_view body);

/// Most instruments one GET /quote accepts.
inline constexpr std::size_t kMaxQuoteInstruments = 500;

/// Kite's "EXCHANGE:TRADINGSYMBOL" key for the quote API: NSE / BSE for cash and indices,
/// NFO / BFO for F&O.
[[nodiscard]] std::string quote_key(instruments::Exchange exchange, instruments::Market market,
                                    std::string_view trading_symbol);

/// Parses GET /quote's {"status":"success","data":{"NFO:X":{...}}} body into the circuit
/// limits (entries without an instrument_token are skipped).
[[nodiscard]] std::expected<std::vector<CircuitLimit>, KiteError> parse_circuit_limits(
    std::string_view body);

/// First existing CA bundle among the usual macOS/Homebrew/Linux locations, if any.
[[nodiscard]] std::optional<std::filesystem::path> default_ca_file();

/// TLS client context that verifies servers against `ca_file`. Verification is never
/// disabled.
[[nodiscard]] std::expected<asio::ssl::context, KiteError> make_tls_context(
    const std::filesystem::path& ca_file);

class RestClient {
public:
    static constexpr std::string_view kDefaultHost = "api.kite.trade";

    RestClient(asio::ssl::context& tls, Session session,
               std::string host = std::string(kDefaultHost))
        : tls_(&tls), session_(std::move(session)), host_(std::move(host)) {}

    /// GET /user/profile — also the cheapest way to validate the access token.
    asio::awaitable<std::expected<Profile, KiteError>> profile();

    /// GET /instruments — the full daily instrument list as CSV (~10 MB).
    asio::awaitable<std::expected<std::string, KiteError>> instruments();

    /// GET /quote for up to kMaxQuoteInstruments keys (see quote_key) — only the circuit
    /// limits are kept. Kite allows about one quote request per second.
    asio::awaitable<std::expected<std::vector<CircuitLimit>, KiteError>> circuit_limits(
        std::span<const std::string> keys);

    /// GET /instruments/historical/{token}/{interval}?from=&to=&oi= — candles in [from, to],
    /// both "YYYY-MM-DD HH:MM:SS" in IST. Minute data: at most 60 days per request; about 3
    /// requests per second.
    asio::awaitable<std::expected<std::vector<Candle>, KiteError>> historical(
        std::uint32_t token, std::string_view interval, std::string_view from, std::string_view to,
        bool oi);

    /// POST /session/token — exchanges the login's request_token for today's access
    /// token. Needs api_key and api_secret in the session; access_token may be empty.
    asio::awaitable<std::expected<SessionToken, KiteError>> create_session(
        std::string_view request_token);

private:
    asio::awaitable<std::expected<std::string, KiteError>> get(std::string_view target,
                                                               std::size_t body_limit);
    asio::awaitable<std::expected<std::string, KiteError>> request(boost::beast::http::verb verb,
                                                                   std::string_view target,
                                                                   std::string form_body,
                                                                   std::size_t body_limit);

    asio::ssl::context* tls_;
    Session session_;
    std::string host_;
    std::chrono::seconds timeout_{30};
};

}  // namespace mda::kite

#pragma once

// Kite Connect login flow helpers (docs/design/kite-connectivity.md §9, used by mda-login).
//
//   1. Open login_url() in a browser; the user logs in on kite.zerodha.com.
//   2. Kite redirects to the app's registered redirect URL with ?request_token=…
//      (plus our `state`, passed through redirect_params).
//   3. POST /session/token with checksum = sha256(api_key + request_token + api_secret)
//      returns the day's access_token.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mda::kite {

/// A redirect target such as http://127.0.0.1:8080/auth/zerodha/callback.
struct RedirectUrl {
    std::string host;
    std::uint16_t port = 80;
    std::string path;
};

/// Parses an http:// redirect URL. Only plain http on a local address makes sense for a
/// desktop callback; anything else is rejected.
[[nodiscard]] std::optional<RedirectUrl> parse_redirect_url(std::string_view url);

/// Kite's login page for `api_key`; `state` comes back unchanged in the redirect.
[[nodiscard]] std::string login_url(std::string_view api_key, std::string_view state);

struct Callback {
    std::string path;
    std::string request_token;
    std::string status;  ///< "success" when the login succeeded
    std::string state;   ///< must match what we sent (guards against stray requests)
};

/// Parses an HTTP request target such as
/// "/auth/zerodha/callback?action=login&status=success&request_token=abc&state=xyz".
[[nodiscard]] Callback parse_callback(std::string_view target);

/// Hex SHA-256 of api_key + request_token + api_secret, as Kite requires.
[[nodiscard]] std::string session_checksum(std::string_view api_key, std::string_view request_token,
                                           std::string_view api_secret);

/// 32 hex characters from the OS's cryptographic random source.
[[nodiscard]] std::string random_state();

[[nodiscard]] std::string url_encode(std::string_view text);
[[nodiscard]] std::string url_decode(std::string_view text);

}  // namespace mda::kite

#pragma once

// Session tokens (ADR-0014): 32 random bytes from the OS CSPRNG, sent to the client
// once (base64url, 43 characters); the database stores only their SHA-256, so a
// database leak does not leak usable sessions.

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace mda::auth {

using TokenHash = std::array<std::byte, 32>;

/// A new random session token, URL/cookie safe. Empty if no secure randomness.
[[nodiscard]] std::string new_session_token();

/// SHA-256 of the token: the database key for the session.
[[nodiscard]] TokenHash hash_token(std::string_view token);

}  // namespace mda::auth

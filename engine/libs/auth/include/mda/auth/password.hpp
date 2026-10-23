#pragma once

// Password hashing with Argon2id (ADR-0014), via OpenSSL 3.2+'s KDF API.
//
// Hashes are stored as PHC strings, which carry their own parameters and salt:
//   $argon2id$v=19$m=19456,t=2,p=1$<base64 salt>$<base64 hash>
// so parameters can be raised later while old hashes keep verifying.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mda::auth {

struct Argon2Params {
    std::uint32_t memory_kib = 19'456;  ///< 19 MiB: OWASP minimum for Argon2id
    std::uint32_t iterations = 2;
    std::uint32_t lanes = 1;
    std::size_t salt_bytes = 16;
    std::size_t hash_bytes = 32;
};

/// Hashes `password` with a fresh random salt. Error text on failure (e.g. KDF missing).
[[nodiscard]] std::expected<std::string, std::string> hash_password(
    std::string_view password, const Argon2Params& params = {});

/// True if `password` matches the PHC string. False for any malformed or foreign hash.
/// Comparison is constant-time.
[[nodiscard]] bool verify_password(std::string_view password, std::string_view phc);

/// Raw Argon2id (exposed for test vectors).
[[nodiscard]] std::expected<std::vector<unsigned char>, std::string> argon2id(
    std::span<const unsigned char> password, std::span<const unsigned char> salt,
    std::span<const unsigned char> secret, std::span<const unsigned char> associated_data,
    std::uint32_t iterations, std::uint32_t memory_kib, std::uint32_t lanes, std::size_t out_bytes);

/// Standard base64 alphabet without padding, as the PHC format uses.
[[nodiscard]] std::string base64_encode(std::span<const unsigned char> data);
[[nodiscard]] std::optional<std::vector<unsigned char>> base64_decode(std::string_view text);

}  // namespace mda::auth

#include <charconv>
#include <memory>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <mda/auth/password.hpp>

namespace mda::auth {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct KdfDeleter {
    void operator()(EVP_KDF* kdf) const noexcept { EVP_KDF_free(kdf); }
    void operator()(EVP_KDF_CTX* ctx) const noexcept { EVP_KDF_CTX_free(ctx); }
};

std::span<const unsigned char> bytes_of(std::string_view text) {
    return {reinterpret_cast<const unsigned char*>(text.data()), text.size()};
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

/// Parses "m=19456,t=2,p=1".
std::optional<Argon2Params> parse_params(std::string_view text) {
    Argon2Params params;
    int seen = 0;
    for (const std::string_view item : split(text, ',')) {
        if (item.size() < 3 || item[1] != '=') {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        const std::string_view digits = item.substr(2);
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || end != digits.data() + digits.size() || value == 0) {
            return std::nullopt;
        }
        switch (item[0]) {
            case 'm':
                params.memory_kib = value;
                break;
            case 't':
                params.iterations = value;
                break;
            case 'p':
                params.lanes = value;
                break;
            default:
                return std::nullopt;
        }
        ++seen;
    }
    return seen == 3 ? std::optional(params) : std::nullopt;
}

}  // namespace

std::string base64_encode(std::span<const unsigned char> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned v = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const unsigned v = unsigned{data[i]} << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (rest == 2) {
        const unsigned v = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

std::optional<std::vector<unsigned char>> base64_decode(std::string_view text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    std::vector<unsigned char> out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const std::size_t value = kAlphabet.find(c);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::expected<std::vector<unsigned char>, std::string> argon2id(
    std::span<const unsigned char> password, std::span<const unsigned char> salt,
    std::span<const unsigned char> secret, std::span<const unsigned char> associated_data,
    std::uint32_t iterations, std::uint32_t memory_kib, std::uint32_t lanes,
    std::size_t out_bytes) {
    const std::unique_ptr<EVP_KDF, KdfDeleter> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr));
    if (!kdf) {
        return std::unexpected("Argon2id not available in this OpenSSL (need 3.2+)");
    }
    const std::unique_ptr<EVP_KDF_CTX, KdfDeleter> ctx(EVP_KDF_CTX_new(kdf.get()));
    if (!ctx) {
        return std::unexpected("cannot create KDF context");
    }

    std::uint32_t threads = 1;
    // OSSL_PARAM takes non-const pointers but only reads input buffers.
    auto octets = [](const char* key, std::span<const unsigned char> data) {
        return OSSL_PARAM_construct_octet_string(key, const_cast<unsigned char*>(data.data()),
                                                 data.size());
    };
    std::vector<OSSL_PARAM> params = {
        octets(OSSL_KDF_PARAM_PASSWORD, password),
        octets(OSSL_KDF_PARAM_SALT, salt),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory_kib),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
    };
    if (!secret.empty()) {
        params.push_back(octets(OSSL_KDF_PARAM_SECRET, secret));
    }
    if (!associated_data.empty()) {
        params.push_back(octets(OSSL_KDF_PARAM_ARGON2_AD, associated_data));
    }
    params.push_back(OSSL_PARAM_construct_end());

    std::vector<unsigned char> out(out_bytes);
    if (EVP_KDF_derive(ctx.get(), out.data(), out.size(), params.data()) != 1) {
        return std::unexpected("Argon2id derivation failed");
    }
    return out;
}

std::expected<std::string, std::string> hash_password(std::string_view password,
                                                      const Argon2Params& params) {
    std::vector<unsigned char> salt(params.salt_bytes);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        return std::unexpected("no secure random source");
    }
    const auto hash = argon2id(bytes_of(password), salt, {}, {}, params.iterations,
                               params.memory_kib, params.lanes, params.hash_bytes);
    if (!hash) {
        return std::unexpected(hash.error());
    }
    return "$argon2id$v=19$m=" + std::to_string(params.memory_kib) +
           ",t=" + std::to_string(params.iterations) + ",p=" + std::to_string(params.lanes) + "$" +
           base64_encode(salt) + "$" + base64_encode(*hash);
}

bool verify_password(std::string_view password, std::string_view phc) {
    // "" "argon2id" "v=19" "m=…,t=…,p=…" "<salt>" "<hash>"
    const auto parts = split(phc, '$');
    if (parts.size() != 6 || !parts[0].empty() || parts[1] != "argon2id" || parts[2] != "v=19") {
        return false;
    }
    const auto params = parse_params(parts[3]);
    const auto salt = base64_decode(parts[4]);
    const auto expected = base64_decode(parts[5]);
    if (!params || !salt || !expected || salt->size() < 8 || expected->size() < 16 ||
        params->memory_kib > 1024 * 1024) {  // refuse absurd parameters (DoS guard)
        return false;
    }
    const auto actual = argon2id(bytes_of(password), *salt, {}, {}, params->iterations,
                                 params->memory_kib, params->lanes, expected->size());
    return actual && CRYPTO_memcmp(actual->data(), expected->data(), expected->size()) == 0;
}

}  // namespace mda::auth

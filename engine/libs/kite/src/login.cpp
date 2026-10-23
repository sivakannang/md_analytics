#include <array>
#include <charconv>
#include <cstddef>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <mda/kite/login.hpp>

namespace mda::kite {
namespace {

constexpr char kHex[] = "0123456789abcdef";
constexpr char kHexUpper[] = "0123456789ABCDEF";

std::string to_hex(const unsigned char* data, std::size_t size) {
    std::string hex;
    hex.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        hex += kHex[data[i] >> 4];
        hex += kHex[data[i] & 0x0F];
    }
    return hex;
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

}  // namespace

std::optional<RedirectUrl> parse_redirect_url(std::string_view url) {
    constexpr std::string_view kScheme = "http://";
    if (!url.starts_with(kScheme)) {
        return std::nullopt;
    }
    url.remove_prefix(kScheme.size());
    const std::size_t slash = url.find('/');
    const std::string_view authority = url.substr(0, slash);
    RedirectUrl result;
    result.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));

    const std::size_t colon = authority.rfind(':');
    result.host = std::string(authority.substr(0, colon));
    if (colon != std::string_view::npos) {
        const std::string_view port = authority.substr(colon + 1);
        const auto [end, error] =
            std::from_chars(port.data(), port.data() + port.size(), result.port);
        if (error != std::errc{} || end != port.data() + port.size() || result.port == 0) {
            return std::nullopt;
        }
    }
    // The callback carries a one-time login token: only accept loopback listeners.
    if (result.host != "127.0.0.1" && result.host != "localhost") {
        return std::nullopt;
    }
    return result;
}

std::string login_url(std::string_view api_key, std::string_view state) {
    return "https://kite.zerodha.com/connect/login?v=3&api_key=" + url_encode(api_key) +
           "&redirect_params=" + url_encode("state=" + std::string(state));
}

Callback parse_callback(std::string_view target) {
    Callback callback;
    const std::size_t question = target.find('?');
    callback.path = std::string(target.substr(0, question));
    if (question == std::string_view::npos) {
        return callback;
    }
    std::string_view query = target.substr(question + 1);
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view name = pair.substr(0, eq);
        std::string value = url_decode(pair.substr(eq + 1));
        if (name == "request_token") {
            callback.request_token = std::move(value);
        } else if (name == "status") {
            callback.status = std::move(value);
        } else if (name == "state") {
            callback.state = std::move(value);
        }
    }
    return callback;
}

std::string session_checksum(std::string_view api_key, std::string_view request_token,
                             std::string_view api_secret) {
    const std::string input =
        std::string(api_key) + std::string(request_token) + std::string(api_secret);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr);
    return to_hex(digest.data(), length);
}

std::string random_state() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};  // caller treats empty as failure
    }
    return to_hex(bytes.data(), bytes.size());
}

std::string url_encode(std::string_view text) {
    std::string encoded;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            encoded += c;
        } else {
            encoded += '%';
            encoded += kHexUpper[byte >> 4];
            encoded += kHexUpper[byte & 0x0F];
        }
    }
    return encoded;
}

std::string url_decode(std::string_view text) {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        decoded += text[i] == '+' ? ' ' : text[i];
    }
    return decoded;
}

}  // namespace mda::kite

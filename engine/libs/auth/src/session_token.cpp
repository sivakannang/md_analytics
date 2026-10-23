#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <mda/auth/password.hpp>
#include <mda/auth/session_token.hpp>

namespace mda::auth {

std::string new_session_token() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};
    }
    // base64url: replace the two characters that are special in URLs/cookies.
    std::string token = base64_encode(bytes);
    for (char& c : token) {
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    return token;
}

TokenHash hash_token(std::string_view token) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(token.data(), token.size(), digest.data(), &length, EVP_sha256(), nullptr);
    TokenHash hash{};
    std::memcpy(hash.data(), digest.data(), hash.size());
    return hash;
}

}  // namespace mda::auth

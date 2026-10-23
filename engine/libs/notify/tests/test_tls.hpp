#pragma once

// TLS for the channel tests: a throwaway self-signed certificate for "localhost", made in
// memory, and contexts that serve it or trust it.

#include <memory>

#include <boost/asio/ssl/context.hpp>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

namespace mda::notify::test {

struct Certificate {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)> cert{nullptr, X509_free};
};

inline Certificate make_localhost_certificate() {
    Certificate c;
    c.key.reset(EVP_EC_gen("P-256"));
    c.cert.reset(X509_new());
    X509* x = c.cert.get();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -60);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, c.key.get());
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "DNS:localhost");
    X509_add_ext(x, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x, c.key.get(), EVP_sha256());
    return c;
}

inline void serve_with(boost::asio::ssl::context& server, const Certificate& c) {
    SSL_CTX_use_certificate(server.native_handle(), c.cert.get());
    SSL_CTX_use_PrivateKey(server.native_handle(), c.key.get());
}

inline void trust(boost::asio::ssl::context& client, const Certificate& c) {
    client.set_verify_mode(boost::asio::ssl::verify_peer);
    X509_STORE_add_cert(SSL_CTX_get_cert_store(client.native_handle()), c.cert.get());
}

}  // namespace mda::notify::test

#include "core/tls_trust.hpp"

#ifdef _WIN32
// Before OpenSSL: wincrypt's names that clash with OpenSSL's are undone by
// OpenSSL's own headers when they come second.
#include <windows.h>
#include <wincrypt.h>
#endif
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <filesystem>
#include <system_error>

namespace wowee::core {

namespace {

/// OpenSSL's verify step handed to a ChainVerifier: the chain the server
/// sent, leaf first, and the name the client asked for (its SNI).
int verifyWithChainVerifier(X509_STORE_CTX* store, void* arg) {
    const auto* verifier = static_cast<const ChainVerifier*>(arg);
    DerChain chain;
    auto add = [&chain](X509* cert) {
        unsigned char* der = nullptr;
        const int len = i2d_X509(cert, &der);
        if (len > 0) chain.emplace_back(der, der + len);
        OPENSSL_free(der);
    };
    X509* leaf = X509_STORE_CTX_get0_cert(store);
    if (leaf != nullptr) add(leaf);
    // The peer's chain as sent; a client's holds the leaf as well.
    if (STACK_OF(X509)* sent = X509_STORE_CTX_get0_untrusted(store)) {
        for (int i = 0; i < sk_X509_num(sent); ++i) {
            X509* cert = sk_X509_value(sent, i);
            if (leaf == nullptr || X509_cmp(cert, leaf) != 0) add(cert);
        }
    }
    std::string host;
    if (auto* ssl = static_cast<SSL*>(
            X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()))) {
        if (const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) host = name;
    }
    if (leaf != nullptr && verifier != nullptr && verifier->verify != nullptr &&
        verifier->verify(chain, host)) {
        X509_STORE_CTX_set_error(store, X509_V_OK);
        return 1;
    }
    X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_UNTRUSTED);
    return 0;
}

}  // namespace

void useChainVerifier(SSL_CTX* ctx, const ChainVerifier& verifier) {
    SSL_CTX_set_cert_verify_callback(ctx, verifyWithChainVerifier,
                                     const_cast<ChainVerifier*>(&verifier));
}

int addPemCertificates(X509_STORE* store, const std::string& dir) {
    namespace fs = std::filesystem;
    int added = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) continue;
        // A certificate is a few kilobytes; nothing in a certificate folder
        // that is much more is one.
        const auto size = it->file_size(fileEc);
        if (fileEc || size == 0 || size > 1024 * 1024) continue;
        BIO* in = BIO_new_file(it->path().string().c_str(), "r");
        if (in == nullptr) continue;
        // Every certificate in the file: PEM_read skips what is not one, such
        // as the text Android writes after each of its own.
        while (X509* cert = PEM_read_bio_X509(in, nullptr, nullptr, nullptr)) {
            // A duplicate is refused, and that is fine: it is in already.
            if (X509_STORE_add_cert(store, cert) == 1) ++added;
            X509_free(cert);
        }
        BIO_free(in);
    }
    // The end of each file reads as an error; none of it is one.
    ERR_clear_error();
    return added;
}

int loadTrustStore(SSL_CTX* ctx) {
    int added = 0;
#ifdef _WIN32
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    HCERTSTORE roots = CertOpenSystemStoreW(0, L"ROOT");
    if (roots != nullptr) {
        PCCERT_CONTEXT cert = nullptr;
        while ((cert = CertEnumCertificatesInStore(roots, cert)) != nullptr) {
            const unsigned char* der = cert->pbCertEncoded;
            X509* x = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded));
            if (x != nullptr) {
                // A duplicate is refused, and that is fine: it is in already.
                if (X509_STORE_add_cert(store, x) == 1) ++added;
                X509_free(x);
            }
        }
        CertCloseStore(roots, 0);
    }
#elif defined(__APPLE__)
    // No folder of roots to read on iOS at all, and on macOS only where
    // Homebrew put one: the system decides, as it does for every other app.
    static const ChainVerifier kSystemTrust{appleTrustsChain};
    useChainVerifier(ctx, kSystemTrust);
    added = 1;
#elif defined(__ANDROID__)
    // The system's roots, one PEM file each: the updatable set in the
    // Conscrypt module from Android 14, the system image's before that. Read
    // file by file - their names are hashes of an older OpenSSL's, which this
    // one does not look up.
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    added = addPemCertificates(store, "/apex/com.android.conscrypt/cacerts");
    if (added == 0) added = addPemCertificates(store, "/system/etc/security/cacerts");
#else
    if (SSL_CTX_set_default_verify_paths(ctx) == 1) added = 1;
#endif
    ERR_clear_error();
    return added;
}

}  // namespace wowee::core

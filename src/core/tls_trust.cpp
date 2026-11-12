#include "core/tls_trust.hpp"

#ifdef _WIN32
// Before OpenSSL: wincrypt's names that clash with OpenSSL's are undone by
// OpenSSL's own headers when they come second.
#include <windows.h>
#include <wincrypt.h>
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace wowee::core {

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
#else
    if (SSL_CTX_set_default_verify_paths(ctx) == 1) added = 1;
#endif
    ERR_clear_error();
    return added;
}

}  // namespace wowee::core

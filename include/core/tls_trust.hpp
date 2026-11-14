#pragma once

/// The trust store a TLS client verifies a server's certificate against.
///
/// OpenSSL's default verify paths name a folder on the machine OpenSSL was
/// built on. On Windows that folder is not there and the handshake failed
/// with "STORE routines::unregistered scheme"; the system's root certificates
/// - the ones the rest of Windows trusts - are loaded instead. On the phones
/// it was not there either: OpenSSL is built for them on a CI machine, so
/// every server went unverified ("unable to get local issuer certificate"),
/// and the streaming data mode never started - the data screen came back on
/// every launch. iOS (and macOS) now ask the system about the chain the
/// server sent (appleTrustsChain); Android reads its own root folders
/// (addPemCertificates). Elsewhere OpenSSL's default paths are used.
///
/// Shared by every HTTPS client in the program (the update check and the
/// streaming data mode), so they cannot drift apart again.

#include <string>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct x509_store_st X509_STORE;

namespace wowee::core {

/// Fills `ctx`'s certificate store, or hands its verification to the system.
/// Returns the number of root certificates added on Windows and Android; on
/// Apple platforms 1 (the system decides); elsewhere 1 when OpenSSL's default
/// paths were set and 0 when they were not. Leaves the thread's OpenSSL error
/// queue empty.
int loadTrustStore(SSL_CTX* ctx);

/// The certificates a server sent, leaf first, each as DER.
using DerChain = std::vector<std::vector<unsigned char>>;

/// A verifier that decides on a whole chain itself, rather than OpenSSL
/// against its store: true when `chain` is trusted for `host` (the name the
/// client asked for).
struct ChainVerifier {
    bool (*verify)(const DerChain& chain, const std::string& host);
};

/// Every handshake on `ctx` asks `verifier` instead of OpenSSL's own check;
/// its answer is the handshake's. `verifier` must outlive `ctx`.
void useChainVerifier(SSL_CTX* ctx, const ChainVerifier& verifier);

/// Adds every PEM certificate in the files of `dir` to `store` (Android's
/// layout: one root per file, its text after it). Returns the number added; 0
/// for a folder that is not there. Leaves the OpenSSL error queue empty.
int addPemCertificates(X509_STORE* store, const std::string& dir);

#if defined(__APPLE__)
/// The system's decision on `chain` for `host` (Security framework;
/// tls_trust_apple.cpp). False with no host.
bool appleTrustsChain(const DerChain& chain, const std::string& host);
#endif

}  // namespace wowee::core

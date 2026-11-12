#pragma once

/// The trust store a TLS client verifies a server's certificate against.
///
/// OpenSSL's default verify paths name no store on Windows: the MinGW build
/// looks for a folder that is not there, and the handshake then fails with
/// "STORE routines::unregistered scheme". There the system's root
/// certificates - the ones the rest of Windows trusts - are loaded instead.
/// Elsewhere OpenSSL's default paths are used.
///
/// Shared by every HTTPS client in the program (the update check and the
/// streaming data mode), so they cannot drift apart again.

typedef struct ssl_ctx_st SSL_CTX;

namespace wowee::core {

/// Fills `ctx`'s certificate store. Returns the number of root certificates
/// added on Windows; elsewhere 1 when OpenSSL's default paths were set and 0
/// when they were not. Leaves the thread's OpenSSL error queue empty.
int loadTrustStore(SSL_CTX* ctx);

}  // namespace wowee::core

// The trust store the HTTPS clients verify against (core/tls_trust.hpp).
//
// The streaming data mode never started on a phone: the client's trust store
// was OpenSSL's default paths there, which name a folder on the machine that
// built OpenSSL, so every server certificate went unverified - "unable to get
// local issuer certificate" - and the data screen came back. iOS now hands the
// chain to the system (useChainVerifier + appleTrustsChain), Android reads its
// own certificate folders (addPemCertificates). Neither phone is here, so the
// two halves that are not the platform's are pinned on this machine: the
// handshake hands the verifier the chain and the host it asked for, and its
// answer is the handshake's; and a folder of PEM files is read as Android's is.

#include <catch2/catch_amalgamated.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/tls_trust.hpp"

namespace fs = std::filesystem;
using namespace wowee::core;

namespace {

struct Credentials {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    Credentials() = default;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;
    ~Credentials() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};

/// A self-signed certificate for `name`, as a server with no CA would have.
void makeSelfSigned(Credentials& out, const char* name) {
    out.key = EVP_EC_gen("P-256");
    REQUIRE(out.key != nullptr);
    out.cert = X509_new();
    REQUIRE(out.cert != nullptr);
    X509_set_version(out.cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(out.cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(out.cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(out.cert), 3600);
    X509_set_pubkey(out.cert, out.key);
    X509_NAME* subject = X509_get_subject_name(out.cert);
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(name), -1, -1, 0);
    X509_set_issuer_name(out.cert, subject);
    REQUIRE(X509_sign(out.cert, out.key, EVP_sha256()) > 0);
}

std::vector<unsigned char> der(X509* cert) {
    unsigned char* bytes = nullptr;
    const int len = i2d_X509(cert, &bytes);
    std::vector<unsigned char> out;
    if (len > 0) out.assign(bytes, bytes + len);
    OPENSSL_free(bytes);
    return out;
}

// What the verifier was handed, and what it answers.
DerChain g_seenChain;
std::string g_seenHost;
int g_calls = 0;
bool g_answer = false;

bool recordingVerifier(const DerChain& chain, const std::string& host) {
    ++g_calls;
    g_seenChain = chain;
    g_seenHost = host;
    return g_answer;
}
const ChainVerifier kRecording{recordingVerifier};
void useRecording(SSL_CTX* ctx) { useChainVerifier(ctx, kRecording); }

struct Handshake {
    bool ok = false;
    long verifyResult = X509_V_OK;
};

/// A client and a server talking through a memory pipe, the client asking for
/// `host`, its context made ready by `setupClient` (left alone, it has
/// OpenSSL's own store, which is empty here).
Handshake handshake(const Credentials& server, const char* host,
                    const std::function<void(SSL_CTX*)>& setupClient) {
    SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
    REQUIRE(sctx != nullptr);
    REQUIRE(cctx != nullptr);
    REQUIRE(SSL_CTX_use_certificate(sctx, server.cert) == 1);
    REQUIRE(SSL_CTX_use_PrivateKey(sctx, server.key) == 1);
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, nullptr);
    if (setupClient) setupClient(cctx);

    SSL* c = SSL_new(cctx);
    SSL* s = SSL_new(sctx);
    BIO* cbio = nullptr;
    BIO* sbio = nullptr;
    REQUIRE(BIO_new_bio_pair(&cbio, 0, &sbio, 0) == 1);
    SSL_set_bio(c, cbio, cbio);
    SSL_set_bio(s, sbio, sbio);
    SSL_set_connect_state(c);
    SSL_set_accept_state(s);
    SSL_set_tlsext_host_name(c, host);

    Handshake result;
    bool clientDone = false;
    bool serverDone = false;
    for (int round = 0; round < 50 && !(clientDone && serverDone); ++round) {
        if (!clientDone) {
            const int rc = SSL_do_handshake(c);
            if (rc == 1) {
                clientDone = true;
            } else {
                const int err = SSL_get_error(c, rc);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) break;
            }
        }
        if (!serverDone) {
            const int rc = SSL_do_handshake(s);
            if (rc == 1) {
                serverDone = true;
            } else {
                const int err = SSL_get_error(s, rc);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) break;
            }
        }
    }
    result.ok = clientDone && serverDone;
    result.verifyResult = SSL_get_verify_result(c);
    ERR_clear_error();
    SSL_free(c);
    SSL_free(s);
    SSL_CTX_free(cctx);
    SSL_CTX_free(sctx);
    return result;
}

struct Sandbox {
    fs::path root;
    Sandbox() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("wowee_tls_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

std::string pem(X509* cert) {
    BIO* mem = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(mem, cert);
    char* data = nullptr;
    const long len = BIO_get_mem_data(mem, &data);
    std::string out(data, static_cast<size_t>(len));
    BIO_free(mem);
    return out;
}

}  // namespace

TEST_CASE("Without a store that knows it, the server is refused", "[tls_trust]") {
    // What a phone's handshake did with OpenSSL's default paths: no root.
    Credentials server;
    makeSelfSigned(server, "stream.example");
    const Handshake h = handshake(server, "stream.example", {});
    CHECK_FALSE(h.ok);
    CHECK(h.verifyResult != X509_V_OK);
}

TEST_CASE("The chain verifier is handed the chain and the host, and decides", "[tls_trust]") {
    Credentials server;
    makeSelfSigned(server, "stream.example");

    g_calls = 0;
    g_answer = true;
    const Handshake trusted = handshake(server, "stream.example", useRecording);
    CHECK(trusted.ok);
    CHECK(g_calls == 1);
    REQUIRE(g_seenChain.size() == 1);
    CHECK(g_seenChain[0] == der(server.cert));   // leaf first, as DER
    CHECK(g_seenHost == "stream.example");       // the name asked for

    g_calls = 0;
    g_answer = false;
    const Handshake refused = handshake(server, "stream.example", useRecording);
    CHECK_FALSE(refused.ok);
    CHECK(g_calls == 1);
    CHECK(refused.verifyResult == X509_V_ERR_CERT_UNTRUSTED);
}

TEST_CASE("A folder of PEM certificates is read as Android keeps them", "[tls_trust]") {
    Sandbox box;
    Credentials a;
    Credentials b;
    makeSelfSigned(a, "root-a");
    makeSelfSigned(b, "root-b");
    // Android's cacerts files: the PEM block, then the certificate as text.
    std::ofstream(box.root / "5ed36f99.0") << pem(a.cert) << "Certificate:\n    Data: ...\n";
    std::ofstream(box.root / "a94d09e5.0") << pem(b.cert);
    std::ofstream(box.root / "README") << "not a certificate\n";

    X509_STORE* store = X509_STORE_new();
    CHECK(addPemCertificates(store, box.root.string()) == 2);
    // A folder that is not there adds nothing and says nothing.
    CHECK(addPemCertificates(store, (box.root / "missing").string()) == 0);
    CHECK(ERR_peek_error() == 0);
    X509_STORE_free(store);
}

TEST_CASE("A store that knows the root lets the handshake through", "[tls_trust]") {
    // The Android path end to end on this machine: the certificate read from
    // a folder into the client's store, and OpenSSL's own check then passes.
    Sandbox box;
    Credentials server;
    makeSelfSigned(server, "stream.example");
    std::ofstream(box.root / "00000000.0") << pem(server.cert);

    int added = 0;
    const Handshake h = handshake(server, "stream.example", [&](SSL_CTX* ctx) {
        added = addPemCertificates(SSL_CTX_get_cert_store(ctx), box.root.string());
    });
    CHECK(added == 1);
    CHECK(h.ok);
    CHECK(h.verifyResult == X509_V_OK);
}

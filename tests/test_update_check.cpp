/// Version ordering for the update notice.
///
/// The comparison is the whole of the decision: wrong one way it nags
/// somebody who is already current, wrong the other it stays quiet for
/// somebody who is not. No network here - only the ordering.

#include <catch_amalgamated.hpp>

#include "core/update_check.hpp"
#include "core/https_get.hpp"
#include "core/tls_trust.hpp"

#include <openssl/ssl.h>

using wowee::core::isNewerVersion;

TEST_CASE("a later release is newer", "[update]") {
    CHECK(isNewerVersion("v3.1.31", "v3.1.30"));
    CHECK(isNewerVersion("v3.2.0", "v3.1.30"));
    CHECK(isNewerVersion("v4.0.0", "v3.99.99"));
    // Ten is not one, which is what a string comparison would make of it.
    CHECK(isNewerVersion("v3.1.30", "v3.1.9"));
    CHECK(isNewerVersion("v3.10.0", "v3.9.0"));
}

TEST_CASE("the same release is not newer", "[update]") {
    CHECK_FALSE(isNewerVersion("v3.1.30", "v3.1.30"));
}

TEST_CASE("an older release is not newer", "[update]") {
    CHECK_FALSE(isNewerVersion("v3.1.29", "v3.1.30"));
    CHECK_FALSE(isNewerVersion("v2.9.9", "v3.0.0"));
    CHECK_FALSE(isNewerVersion("v3.1.9", "v3.1.30"));
}

TEST_CASE("a build past the tag is not sent backwards", "[update]") {
    // git describe on a build with commits on top of v3.1.30. It is ahead of
    // that release, so that release is not something to go and download.
    CHECK_FALSE(isNewerVersion("v3.1.30", "v3.1.30-7-gabc1234"));
    // A genuinely later release still shows, though.
    CHECK(isNewerVersion("v3.1.31", "v3.1.30-7-gabc1234"));
}

TEST_CASE("nothing to compare means no notice", "[update]") {
    CHECK_FALSE(isNewerVersion("", "v3.1.30"));
    CHECK_FALSE(isNewerVersion("v3.1.31", ""));
    CHECK_FALSE(isNewerVersion("", ""));
    // A tag that is not a version at all reads as 0.0.0 and must not be
    // announced as an upgrade over a real one.
    CHECK_FALSE(isNewerVersion("nightly", "v3.1.30"));
}

TEST_CASE("the v is optional on either side", "[update]") {
    CHECK(isNewerVersion("3.1.31", "v3.1.30"));
    CHECK(isNewerVersion("v3.1.31", "3.1.30"));
    CHECK_FALSE(isNewerVersion("3.1.30", "3.1.30"));
}

TEST_CASE("build-N tags compare by their build number", "[update]") {
    // The fork's releases and its dev builds: build-<N>-<hash>.
    CHECK(isNewerVersion("build-487-f8913a86", "build-482-1a31eac4"));
    CHECK_FALSE(isNewerVersion("build-482-1a31eac4", "build-482-1a31eac4"));
    CHECK_FALSE(isNewerVersion("build-480-500d132b", "build-482-1a31eac4"));
    // Numbers, not text: 1000 is after 999.
    CHECK(isNewerVersion("build-1000-abc1234", "build-999-def5678"));
    // A dev build with commits on top of build-482 is not behind it, and is
    // still behind a later one.
    CHECK_FALSE(isNewerVersion("build-482-1a31eac4", "build-482-1a31eac4-72-g1262e42c"));
    CHECK(isNewerVersion("build-487-f8913a86", "build-482-1a31eac4-72-g1262e42c"));
}

TEST_CASE("a vX.Y.Z pair still compares as versions", "[update]") {
    CHECK(isNewerVersion("v3.1.54", "v3.1.53"));
    CHECK_FALSE(isNewerVersion("v3.1.53", "v3.1.54"));
}

TEST_CASE("a mixed pair compares as before", "[update]") {
    // build-N against vX.Y.Z reads build-N as 0.0.0, as it always has.
    CHECK(isNewerVersion("v3.1.54", "build-482-1a31eac4"));
    CHECK_FALSE(isNewerVersion("build-487-f8913a86", "v3.1.30"));
    // Not a build tag without the hash after the number.
    CHECK_FALSE(isNewerVersion("build-487", "build-482"));
}

TEST_CASE("the trust store has root certificates", "[update][tls]") {
    // OpenSSL's default paths name no store on Windows; the check has to
    // find the system's roots there, or every handshake fails.
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    REQUIRE(ctx != nullptr);
    const int added = wowee::core::loadTrustStore(ctx);
    CHECK(added > 0);
#ifdef _WIN32
    // Windows keeps dozens; a handful would mean the store was misread.
    CHECK(added >= 10);
#endif
    SSL_CTX_free(ctx);
}

// Hidden (needs the network): `test_update_check "[.network]"` runs it.
TEST_CASE("GitHub's release document is fetched over verified TLS", "[.network]") {
    const wowee::core::HttpsResponse reply = wowee::core::httpsGet(
        "api.github.com", "/repos/IchbinkeinReh/WoWee/releases/latest", "WoWee/test");
    INFO("error: " << reply.error);
    REQUIRE(reply.ok);
    CHECK(reply.status == 200);
    CHECK(reply.body.find("tag_name") != std::string::npos);
}

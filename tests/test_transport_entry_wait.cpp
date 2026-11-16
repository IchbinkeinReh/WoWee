// A world load that carries the player on a transport to another map ends
// once the transport is known there (core::transportEntryGround), not with
// the rider at the map's origin.
#include <catch_amalgamated.hpp>

#include "core/transport_entry_wait.hpp"

using wowee::core::kMaxTransportEntryWaitSeconds;
using wowee::core::transportEntryGround;

TEST_CASE("A rider back on the transport stands on it", "[transport_entry]") {
    const auto g = transportEntryGround(/*onTransport=*/true, /*transferPending=*/false, 0.5f);
    CHECK(g.groundReady);
    CHECK_FALSE(g.awaitTransport);
}

TEST_CASE("A transfer not yet put back on its transport keeps the screen up", "[transport_entry]") {
    // Still tied to the old map's transport guid: that is not a floor here.
    const auto g = transportEntryGround(/*onTransport=*/true, /*transferPending=*/true, 6.4f);
    CHECK_FALSE(g.groundReady);
    CHECK(g.awaitTransport);
}

TEST_CASE("The wait for the transport has a cap", "[transport_entry]") {
    const auto g = transportEntryGround(true, true, kMaxTransportEntryWaitSeconds + 0.1f);
    CHECK_FALSE(g.groundReady);
    CHECK_FALSE(g.awaitTransport);  // the ordinary floor checks decide
}

TEST_CASE("Off any transport nothing is said", "[transport_entry]") {
    const auto g = transportEntryGround(false, false, 3.0f);
    CHECK_FALSE(g.groundReady);
    CHECK_FALSE(g.awaitTransport);
}

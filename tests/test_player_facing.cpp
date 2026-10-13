// GetPlayerFacing() answers the server orientation - 0 north, counter-clockwise,
// in [0, 2pi) - as the original client does, not the canonical yaw the movement
// info holds, which is a quarter turn behind it.
#include <catch_amalgamated.hpp>

#include <cmath>

#include "core/coordinates.hpp"

namespace coords = wowee::core::coords;

TEST_CASE("GetPlayerFacing is the server orientation of the canonical yaw", "[lua][coords]") {
    constexpr float kHalfPi = 1.57079632679f;
    // Facing north on the wire is canonical -pi/2; Lua says 0.
    CHECK(coords::canonicalToPlayerFacing(coords::serverToCanonicalYaw(0.0f)) ==
          Catch::Approx(0.0f).margin(1e-5f));
    // West (+y) is a quarter turn counter-clockwise from north.
    CHECK(coords::canonicalToPlayerFacing(coords::serverToCanonicalYaw(kHalfPi)) ==
          Catch::Approx(kHalfPi).margin(1e-5f));
    // Every server orientation comes back as itself, inside [0, 2pi).
    for (int i = 0; i < 64; ++i) {
        const float server = static_cast<float>(i) * (coords::TWO_PI / 64.0f);
        const float facing = coords::canonicalToPlayerFacing(coords::serverToCanonicalYaw(server));
        INFO("server orientation " << server);
        CHECK(facing >= 0.0f);
        CHECK(facing < coords::TWO_PI);
        CHECK(std::fabs(std::remainder(facing - server, coords::TWO_PI)) < 1e-4f);
    }
}

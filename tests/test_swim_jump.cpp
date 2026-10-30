// Jumping out of the water (swim_jump.hpp, 0x00730d30 -> 0x0072eb80).
#include <catch_amalgamated.hpp>
#include "rendering/swim_jump.hpp"

#include <cstring>

using namespace wowee::rendering::swim_jump;

TEST_CASE("Swim jump: at the surface, swim-up held and moving", "[swim]") {
    constexpr float kFloat = 1.45f;  // where a swimmer floats
    CHECK(allowed(true, true, kFloat, kFloat));
    CHECK(allowed(true, true, kFloat + 0.3f, kFloat));
    // Deeper than a third of a yard under the float depth: still swimming up.
    CHECK_FALSE(allowed(true, true, kFloat + 0.34f, kFloat));
    CHECK_FALSE(allowed(true, true, kFloat + 3.0f, kFloat));
    // Treading water in place, or not pressing up: no jump.
    CHECK_FALSE(allowed(true, false, kFloat, kFloat));
    CHECK_FALSE(allowed(false, true, kFloat, kFloat));
}

TEST_CASE("Swim jump: the swimmer's impulse is the client's", "[swim]") {
    // 0xc1118c48, negated: the fall's speeds are downward-positive.
    const unsigned int bits = 0xc1118c48u;
    float v;
    std::memcpy(&v, &bits, sizeof v);
    CHECK(kVerticalSpeed == Catch::Approx(-v));
    CHECK(kSurfaceBand == Catch::Approx(1.0f / 3.0f));
}

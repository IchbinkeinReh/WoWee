// The client's ripples: their life, and what a unit spawns (0x0071cba0).

#include "rendering/water_ripple.hpp"

#include <catch_amalgamated.hpp>

using namespace wowee::rendering::water_ripple;

TEST_CASE("a ripple rises for 40% of its life and is gone at its end", "[water_ripple]") {
    Ripple r = start(glm::vec3(0.0f), 0.0f, 1.0f, 1.0f, 0.16666667f, 2.0f, false);
    REQUIRE(r.peak == Catch::Approx(1.0f));
    REQUIRE(step(r, 0.2f));
    REQUIRE(r.alpha == Catch::Approx(0.5f).margin(1e-4));
    REQUIRE(step(r, 0.3f));
    REQUIRE(r.alpha == Catch::Approx(0.8333f).margin(1e-3));
    REQUIRE(r.size == Catch::Approx(2.0f));
    REQUIRE(step(r, 0.2f));
    REQUIRE(r.alpha == Catch::Approx(0.5f).margin(1e-3));
    REQUIRE_FALSE(step(r, 0.31f));
}

TEST_CASE("a faint ripple peaks lower", "[water_ripple]") {
    const Ripple r = start(glm::vec3(0.0f), 0.0f, 1.0f, 1.0f, 0.1f, 0.0f, false);
    REQUIRE(r.peak == Catch::Approx(0.6f));
}

TEST_CASE("a unit too deep leaves none", "[water_ripple]") {
    Unit u;
    u.height = 2.0f;
    u.depth = 4.0f;
    Ripple r;
    float next = 0.0f;
    REQUIRE_FALSE(spawn(u, Rolls{}, r, next));
}

TEST_CASE("a still unit leaves a small ring, a moving one a wake", "[water_ripple]") {
    Unit u;
    u.height = 2.0f;
    u.depth = 1.0f;
    u.sizeValue = 3.0f;
    Ripple ring;
    float next = 0.0f;
    REQUIRE(spawn(u, Rolls{}, ring, next));
    REQUIRE_FALSE(ring.wake);
    REQUIRE(ring.size == Catch::Approx(0.6f));
    REQUIRE(ring.growth == Catch::Approx(3.75f / 3.0f * 0.25f));
    REQUIRE(next >= 0.4f);
    REQUIRE(next <= 0.45f);

    u.mode = Mode::Moving;
    u.moveFlags = kForward;
    u.speed = 5.0f;
    Ripple wake;
    REQUIRE(spawn(u, Rolls{}, wake, next));
    REQUIRE(wake.wake);
    REQUIRE(wake.size == Catch::Approx(1.0f));
    REQUIRE(wake.growth == Catch::Approx(3.75f / 3.0f * 2.0f));
    REQUIRE(next == Catch::Approx(0.125f));
}

TEST_CASE("past half the reach a ripple fades with depth", "[water_ripple]") {
    Unit u;
    u.height = 2.0f;
    u.depth = 3.0f;  // reach 4, half 2: halfway on
    u.sizeValue = 3.0f;
    u.mode = Mode::Turning;
    Ripple r;
    float next = 0.0f;
    REQUIRE(spawn(u, Rolls{}, r, next));
    REQUIRE(r.size == Catch::Approx(0.75f));
    REQUIRE(r.life == Catch::Approx(0.65f * 0.75f));
}

TEST_CASE("a wake turns with the way the unit moves", "[water_ripple]") {
    REQUIRE(wakeAngle(1.0f, kForward) == Catch::Approx(1.0f));
    REQUIRE(wakeAngle(1.0f, kBackward) == Catch::Approx(1.0f + 3.1415927f));
    REQUIRE(wakeAngle(0.0f, kStrafeLeft) == Catch::Approx(1.5707964f));
    REQUIRE(wakeAngle(0.0f, kStrafeRight | kForward) == Catch::Approx(-0.78539819f));
}

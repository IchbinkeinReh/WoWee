// The tilt of a swimming or flying player's body (swim_pitch.hpp, 0x00719b80).
#include <catch_amalgamated.hpp>
#include "rendering/swim_pitch.hpp"

#include <cmath>

using namespace wowee::rendering::swim_pitch;

TEST_CASE("Swim pitch: the target follows the movement pitch forward and back", "[swim]") {
    Motion fwd;
    fwd.forward = true;
    CHECK(target(fwd, -0.6f) == Catch::Approx(-0.6f));  // diving where the camera looks
    CHECK(target(fwd, 0.3f) == Catch::Approx(0.3f));
    Motion back;
    back.backward = true;
    CHECK(target(back, -0.6f) == Catch::Approx(-0.6f));
    // Standing still in the water, or only strafing: level.
    CHECK(target(Motion{}, -0.6f) == 0.0f);
    Motion strafe = fwd;
    strafe.strafing = true;
    CHECK(target(strafe, -0.6f) == 0.0f);
}

TEST_CASE("Swim pitch: swimming up or down tilts 45 degrees, mirrored backward", "[swim]") {
    Motion up;
    up.forward = true;
    up.ascending = true;
    CHECK(target(up, -0.6f) == Catch::Approx(kQuarterTurn));
    Motion down;
    down.forward = true;
    down.descending = true;
    CHECK(target(down, 0.6f) == Catch::Approx(-kQuarterTurn));
    Motion backUp = up;
    backUp.forward = false;
    backUp.backward = true;
    CHECK(target(backUp, 0.0f) == Catch::Approx(-kQuarterTurn));
    // Rising in place, not moving forward or back: no tilt.
    Motion rise;
    rise.ascending = true;
    CHECK(target(rise, 0.0f) == 0.0f);
}

TEST_CASE("Swim pitch: eases at 4 pi a second and snaps when close", "[swim]") {
    // A 1/60 s frame: 4 pi / 60 of the way.
    const float k = kEaseRate / 60.0f;
    CHECK(step(0.0f, -0.6f, 1.0f / 60.0f) == Catch::Approx(-0.6f * k));
    // The frame time is held to 1/90 .. 1/20 s.
    CHECK(step(0.0f, -0.6f, 0.0f) == Catch::Approx(-0.6f * kEaseRate / 90.0f));
    CHECK(step(0.0f, -0.6f, 1.0f) == Catch::Approx(-0.6f * kEaseRate * 0.05f));
    CHECK(step(0.3f, 0.305f, 1.0f / 60.0f) == 0.305f);
    // Converges.
    float p = 0.0f;
    for (int i = 0; i < 120; ++i) p = step(p, -0.6f, 1.0f / 60.0f);
    CHECK(p == Catch::Approx(-0.6f));
}

TEST_CASE("Swim pitch: on while swimming and moving, or easing back out", "[swim]") {
    Motion still;
    Motion fwd;
    fwd.forward = true;
    CHECK(active(true, fwd, 0.0f));
    CHECK(active(true, still, -0.3f));  // easing back to level
    CHECK_FALSE(active(true, still, 0.0f));
    CHECK_FALSE(active(false, fwd, -0.3f));  // on land: never
}

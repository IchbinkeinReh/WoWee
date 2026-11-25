// The camera's smart pivot.
#include <catch_amalgamated.hpp>

#include "rendering/smart_pivot.hpp"

#include <cmath>

using namespace wowee::rendering::smart_pivot;
using Catch::Approx;

namespace {

constexpr Limits kLimits;

}  // namespace

TEST_CASE("where the view may tilt", "[camera][smart_pivot]") {
    Standing s;
    CHECK_FALSE(mayTilt(s));
    s.distanceHit = true;
    CHECK(mayTilt(s));
    s.pitch = 0.01f;   // from above
    CHECK_FALSE(mayTilt(s));
    s.pitch = -0.3f;
    CHECK(mayTilt(s));
    s.moving = true;
    CHECK_FALSE(mayTilt(s));
    s.moving = false;
    s.on = false;
    CHECK_FALSE(mayTilt(s));
    s.on = true;
    s.distanceHit = false;
    s.heightHit = true;
    CHECK(mayTilt(s));
    s.flying = true;
    CHECK_FALSE(mayTilt(s));
}

TEST_CASE("against the ground the mouse tilts the view", "[camera][smart_pivot]") {
    Pivot p;
    // Nothing in the way: the pitch.
    CHECK(p.turn({0.0f, -0.1f, 1.0f}, false, -0.2f, kLimits, 0).value() == Approx(-0.1f));
    CHECK(p.tilt() == 0.0f);
    // Up: the tilt, and the pitch stays.
    CHECK_FALSE(p.turn({0.0f, -0.1f, 1.0f}, true, -0.2f, kLimits, 0).has_value());
    CHECK_FALSE(p.turn({0.01f, -0.2f, 1.0f}, true, -0.2f, kLimits, 0).has_value());
    CHECK(p.tilt() == Approx(-0.3f));
    // No further than straight up.
    CHECK_FALSE(p.turn({0.0f, -9.0f, 1.0f}, true, -0.2f, kLimits, 0).has_value());
    CHECK(p.tilt() == Approx(kLimits.lowest + 0.2f));
    // Down again: the tilt first, then the pitch by what is left.
    CHECK_FALSE(p.turn({0.0f, 1.0f, 1.0f}, true, -0.2f, kLimits, 0).has_value());
    const float tilted = p.tilt();
    CHECK(p.turn({0.0f, 0.5f, 1.0f}, true, -0.2f, kLimits, 0).value() == Approx(0.5f + tilted));
    CHECK(p.tilt() == 0.0f);
    // None left, and down: the pitch.
    CHECK(p.turn({0.0f, 0.1f, 1.0f}, true, -0.2f, kLimits, 0).value() == Approx(0.1f));
    // Turned more than tilted: not begun by it.
    CHECK(p.turn({0.06f, -0.1f, 1.0f}, true, -0.2f, kLimits, 0).value() == Approx(-0.1f));
    // Level with it, both.
    CHECK(p.turn({0.0f, -0.1f, 1.0f}, true, 0.0f, kLimits, 0).value() == Approx(-0.1f));
}

TEST_CASE("the tilt goes back as the character moves on", "[camera][smart_pivot]") {
    Pivot p;
    (void)p.turn({0.0f, -0.5f, 1.0f}, true, -0.2f, kLimits, 0);
    p.placed(true, 100);
    p.step(5000);
    CHECK(p.tilt() == Approx(-0.5f));
    CHECK_FALSE(p.easing());
    // Ninety degrees a second, half a cosine.
    p.placed(false, 5000);
    CHECK(p.easing());
    const auto ms = static_cast<uint32_t>(0.5f / 1.5707964f * 1000.0f);
    p.step(5000 + ms / 2);
    CHECK(p.tilt() == Approx(-0.25f).margin(0.01f));
    p.placed(false, 5000 + ms / 2);   // goes on
    p.step(5000 + ms + 2);
    CHECK(p.tilt() == 0.0f);
    CHECK_FALSE(p.easing());
    // Held where it may tilt again before it is back.
    (void)p.turn({0.0f, -0.5f, 1.0f}, true, -0.2f, kLimits, 6000);
    p.placed(false, 6000);
    p.step(6000 + ms / 2);
    p.placed(true, 6000 + ms / 2);
    p.step(9000);
    CHECK(p.tilt() == Approx(-0.25f).margin(0.01f));
    p.reset();
    CHECK(p.tilt() == 0.0f);
}

// The touch sticks' arithmetic: which half of the safe area a thumb landed in,
// and how far a held camera stick turns the view.
//
// The events, the camera and ImGui that TouchControls drives need a phone to
// test; these are the parts that decide what a press means.
#include <catch_amalgamated.hpp>

#include "ui/touch_zones.hpp"

#include <cmath>

using namespace wowee::ui::touch;

TEST_CASE("Without a safe area the halves meet at the window's middle", "[touch]") {
    const Area a = usableArea(1920.0f, 1080.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(a.x == 0.0f);
    REQUIRE(a.w == 1920.0f);
    REQUIRE(splitX(a) == 960.0f);
    REQUIRE(halfAt(959.0f, a) == Half::Move);
    REQUIRE(halfAt(960.0f, a) == Half::Look);
    // Full height: the top corners are sticks too.
    REQUIRE(halfAt(10.0f, a) == Half::Move);
    REQUIRE(halfAt(1910.0f, a) == Half::Look);
}

TEST_CASE("Each stick owns one half of the safe area", "[touch]") {
    // A notch of 140 on the left and a rounded corner of 40 on the right.
    const Area a = usableArea(2400.0f, 1080.0f, 140.0f, 40.0f, 0.0f, 60.0f);
    REQUIRE(a.x == 140.0f);
    REQUIRE(a.w == 2220.0f);
    REQUIRE(a.h == 1020.0f);
    // The middle of what is usable, not of the panel: 140 + 1110.
    REQUIRE(splitX(a) == 1250.0f);
    REQUIRE(halfAt(1200.0f, a) == Half::Move);   // left of the split, though right of the panel's middle
    REQUIRE(halfAt(1250.0f, a) == Half::Look);
    // Both halves are the same width.
    REQUIRE(splitX(a) - a.x == (a.x + a.w) - splitX(a));
}

TEST_CASE("A press beyond the safe area is in neither half's zone", "[touch]") {
    const Area a = usableArea(2400.0f, 1080.0f, 140.0f, 40.0f, 0.0f, 60.0f);
    REQUIRE(inside(140.0f, 0.0f, a));
    REQUIRE_FALSE(inside(139.0f, 500.0f, a));    // in the notch
    REQUIRE_FALSE(inside(2360.0f, 500.0f, a));   // in the rounded corner
    REQUIRE_FALSE(inside(1200.0f, 1020.0f, a));  // on the gesture bar
    REQUIRE(inside(2359.0f, 1019.0f, a));
}

TEST_CASE("Insets that leave nothing fall back to the whole window", "[touch]") {
    const Area a = usableArea(800.0f, 600.0f, 500.0f, 400.0f, 0.0f, 0.0f);
    REQUIRE(a.x == 0.0f);
    REQUIRE(a.w == 800.0f);
    REQUIRE(splitX(a) == 400.0f);
}

TEST_CASE("The camera stick turns at a rate, from nothing past its ring", "[touch]") {
    constexpr float kDead = 0.45f;
    constexpr float kRate = 900.0f;
    // Inside the ring: nothing, however long it is held.
    Turn t = lookStickTurn(0.3f, 0.2f, kDead, kRate, 1.0f);
    REQUIRE(t.dx == 0.0f);
    REQUIRE(t.dy == 0.0f);
    // Just past the ring: almost nothing, so the edge is not a jump.
    t = lookStickTurn(0.46f, 0.0f, kDead, kRate, 1.0f);
    REQUIRE(t.dx > 0.0f);
    REQUIRE(t.dx < 20.0f);
    // Full right for half a second: half the full rate, all of it sideways.
    t = lookStickTurn(1.0f, 0.0f, kDead, kRate, 0.5f);
    REQUIRE(std::abs(t.dx - 450.0f) < 0.01f);
    REQUIRE(t.dy == 0.0f);
    // Left and up turn the other way, as a mouse moved that way would.
    t = lookStickTurn(-1.0f, 0.0f, kDead, kRate, 0.5f);
    REQUIRE(std::abs(t.dx + 450.0f) < 0.01f);
    t = lookStickTurn(0.0f, -1.0f, kDead, kRate, 0.5f);
    REQUIRE(std::abs(t.dy + 450.0f) < 0.01f);
    // A diagonal at full deflection turns at the full rate, not faster.
    t = lookStickTurn(1.0f, 1.0f, kDead, kRate, 1.0f);
    REQUIRE(std::abs(std::hypot(t.dx, t.dy) - 900.0f) < 0.01f);
    // No time, no turn.
    t = lookStickTurn(1.0f, 0.0f, kDead, kRate, 0.0f);
    REQUIRE(t.dx == 0.0f);
}

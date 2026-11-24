// What the movement state is told after a step (rendering/mover_report.hpp):
// FALLING and SWIMMING as the client's own step keeps them - a fall ends on
// ground whatever the step before looked like, and a swimmer whose flags do
// not say so starts to swim.
#include <catch_amalgamated.hpp>

#include "rendering/mover_report.hpp"

using namespace wowee::rendering;

namespace {

MoverStep standing() {
    MoverStep s;
    s.onGround = true;
    return s;
}

}  // namespace

TEST_CASE("a fall still on where the mover stands has landed, however it came there", "[mover_report]") {
    // Rising as it met the slope, down again in the frame it jumped in, a
    // tenth of a second in the air: all of them left FALLING set, as nothing
    // but a step that came down for 0.15 s was a landing.
    MoverStep s = standing();
    s.saidFalling = true;
    CHECK(fallChange(s) == FallChange::Landed);
    s.saidFalling = false;
    CHECK(fallChange(s) == FallChange::None);
}

TEST_CASE("the ground gone starts the fall in the step it went (0x007620f0 -> 0x00988370)", "[mover_report]") {
    MoverStep s;
    CHECK(fallChange(s) == FallChange::Began);
    // A jump said it already.
    s.saidFalling = true;
    CHECK(fallChange(s) == FallChange::None);
}

TEST_CASE("nothing falls in water, in flight, without gravity or carried", "[mover_report]") {
    for (int i = 0; i < 4; ++i) {
        MoverStep s;
        s.swimming = i == 0;
        s.flying = i == 1;
        s.floating = i == 2;
        s.carried = i == 3;
        CHECK(fallChange(s) == FallChange::None);
        // Its fall is over, and no ground was landed on.
        s.saidFalling = true;
        CHECK(fallChange(s) == FallChange::Over);
        s.onGround = true;
        CHECK(fallChange(s) == FallChange::Over);
    }
}

TEST_CASE("a swim is told where the flags say otherwise, not where it began", "[mover_report]") {
    MoverStep s;
    s.swimming = true;
    // Logged in afloat: the first frame's START_SWIM went into a world not
    // yet entered, or the flags were cleared after it.
    CHECK(swimChange(s) == SwimChange::Starts);
    s.saidSwimming = true;
    CHECK(swimChange(s) == SwimChange::None);
    s.swimming = false;
    CHECK(swimChange(s) == SwimChange::Stops);
    s.saidSwimming = false;
    CHECK(swimChange(s) == SwimChange::None);
    // A flight path over the sea is no swim's end.
    s.saidSwimming = true;
    s.carried = true;
    CHECK(swimChange(s) == SwimChange::None);
}

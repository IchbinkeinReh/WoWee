// The left press SDL makes of the first finger, and what TouchControls lets
// through of it: a tap is a left click, a push is a left press that drags, and
// a long press is a right click with no left button in it at all.
//
// The events, ImGui and core::Input need a phone; this is the part that
// decides what each of them sees.
#include <catch_amalgamated.hpp>

#include "ui/touch_press.hpp"

using namespace wowee::ui::touch;

TEST_CASE("A tap on a frame is a left click, let through when the finger lifts", "[touch]") {
    HeldPress p;
    // SDL sends the button ahead of the finger.
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.fingerDown(true) == PressStep::None);
    REQUIRE(p.held());
    REQUIRE(p.keptBack());
    REQUIRE(p.update() == PressStep::None);
    // Press and release together: a click.
    REQUIRE(p.buttonUp() == PressStep::DeliverClick);
    REQUIRE_FALSE(p.keptBack());
}

TEST_CASE("A long press is a right click and never a left one", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.fingerDown(true) == PressStep::None);
    REQUIRE(p.longPress());
    // Still kept from core::Input, so the right click is a right click alone
    // (both buttons are walking forward).
    REQUIRE(p.keptBack());
    REQUIRE_FALSE(p.held());
    // A finger held still past the long press and then moved does not bring
    // the press back.
    REQUIRE(p.moved() == PressStep::None);
    REQUIRE_FALSE(p.longPress());
    // Lifting it releases a button nobody saw go down: kept back as well.
    REQUIRE(p.buttonUp() == PressStep::Swallow);
    REQUIRE_FALSE(p.keptBack());
}

TEST_CASE("A finger that moves is a press after all, and drags", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.fingerDown(true) == PressStep::None);
    REQUIRE(p.moved() == PressStep::Deliver);
    REQUIRE_FALSE(p.keptBack());
    // Delivered once.
    REQUIRE(p.moved() == PressStep::None);
    // No long press any more, and the release goes through as it came.
    REQUIRE_FALSE(p.longPress());
    REQUIRE(p.buttonUp() == PressStep::Pass);
}

TEST_CASE("The movement stick's press is let through in the same batch", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    // The finger turns out to be the movement stick: nothing it can be long
    // pressed into, so the press goes through at once.
    REQUIRE(p.fingerDown(false) == PressStep::Deliver);
    REQUIRE_FALSE(p.keptBack());
    REQUIRE_FALSE(p.longPress());
    REQUIRE(p.buttonUp() == PressStep::Pass);
}

TEST_CASE("The finger may be seen before its button", "[touch]") {
    HeldPress hold;
    REQUIRE(hold.fingerDown(true) == PressStep::None);
    REQUIRE(hold.buttonDown(true) == PressStep::Swallow);
    REQUIRE(hold.held());
    REQUIRE(hold.buttonUp() == PressStep::DeliverClick);

    HeldPress pass;
    REQUIRE(pass.fingerDown(false) == PressStep::None);
    REQUIRE(pass.buttonDown(true) == PressStep::Pass);
    REQUIRE_FALSE(pass.keptBack());
    REQUIRE(pass.buttonUp() == PressStep::Pass);

    // What was expected of one finger is not carried to the next.
    HeldPress next;
    REQUIRE(next.fingerDown(true) == PressStep::None);
    REQUIRE(next.buttonDown(true) == PressStep::Swallow);
    REQUIRE(next.buttonUp() == PressStep::DeliverClick);
    REQUIRE(next.buttonDown(true) == PressStep::Swallow);
    REQUIRE(next.update() == PressStep::Deliver);
}

TEST_CASE("Outside the world the press is the mouse's as it came", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(false) == PressStep::Pass);
    REQUIRE(p.fingerDown(true) == PressStep::None);
    REQUIRE_FALSE(p.held());
    REQUIRE_FALSE(p.longPress());
    REQUIRE(p.buttonUp() == PressStep::Pass);
}

TEST_CASE("A press whose finger never shows is let through a frame later", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.update() == PressStep::Deliver);
    REQUIRE(p.update() == PressStep::None);
    REQUIRE(p.buttonUp() == PressStep::Pass);
}

TEST_CASE("A finger lifted before it was placed is still a click", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.buttonUp() == PressStep::DeliverClick);
}

TEST_CASE("A reset with the finger down forgets the press", "[touch]") {
    HeldPress p;
    REQUIRE(p.buttonDown(true) == PressStep::Swallow);
    REQUIRE(p.fingerDown(true) == PressStep::None);
    p.reset();
    REQUIRE_FALSE(p.keptBack());
    REQUIRE_FALSE(p.longPress());
    // Its release reaches only what saw no press, as it would have.
    REQUIRE(p.buttonUp() == PressStep::Pass);
}

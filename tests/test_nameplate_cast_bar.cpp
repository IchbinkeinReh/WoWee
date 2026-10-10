// The target's nameplate cast bar: handed a cast, run on by itself, green and
// fading at its end (0x0098f040, 0x0098e9f0, 0x0098e7e0).
#include <catch_amalgamated.hpp>
#include "ui/nameplate_cast_bar.hpp"

using wowee::ui::PlateCastBar;

TEST_CASE("A cast runs to its end, turns green and fades out", "[nameplate_cast_bar]") {
    PlateCastBar bar;
    bar.set(0.0f, 2.0f, 1.5f, 133, true, false, false);
    REQUIRE(bar.shown);
    REQUIRE(bar.colour == PlateCastBar::Colour::Orange);
    bar.update(0.25f);
    REQUIRE(bar.value == Catch::Approx(1.75f));
    bar.update(0.5f);
    REQUIRE(bar.value == Catch::Approx(2.0f));
    REQUIRE(bar.colour == PlateCastBar::Colour::Green);
    REQUIRE(bar.fade == Catch::Approx(1.0f));
    REQUIRE(bar.alpha == 255);
    // Each frame multiplies what is left by what is left of the second.
    bar.update(0.5f);
    REQUIRE(bar.alpha == 127);
    bar.update(0.25f);
    REQUIRE(bar.alpha == 31);
    REQUIRE(bar.shown);
    bar.update(0.25f);
    REQUIRE_FALSE(bar.shown);
}

TEST_CASE("A channel drains to its start", "[nameplate_cast_bar]") {
    PlateCastBar bar;
    bar.set(0.0f, 5.0f, 0.2f, 5143, true, true, false);
    REQUIRE(bar.filled() == Catch::Approx(0.04f));
    bar.update(0.5f);
    REQUIRE(bar.value == 0.0f);
    REQUIRE(bar.colour == PlateCastBar::Colour::Green);
}

TEST_CASE("Handed over at its end it is red; with no spell, hidden", "[nameplate_cast_bar]") {
    PlateCastBar bar;
    bar.set(0.0f, 5.0f, 0.0f, 5143, true, true, true);
    REQUIRE(bar.colour == PlateCastBar::Colour::Red);
    REQUIRE(bar.fade == 1.0f);
    REQUIRE(bar.notInterruptible);
    bar.set(0.0f, 0.0f, 0.0f, 0, false, false, false);
    REQUIRE_FALSE(bar.shown);
    // A trade skill has no bar.
    bar.set(0.0f, 2.0f, 1.0f, 2018, false, false, false);
    REQUIRE_FALSE(bar.shown);
    // A value past the end leaves the bar as it was.
    bar.set(0.0f, 2.0f, 1.0f, 133, true, false, false);
    bar.set(0.0f, 2.0f, 3.0f, 133, true, false, false);
    REQUIRE(bar.value == 1.0f);
}

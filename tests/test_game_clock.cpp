// The time of day running on from what the server said, at the speed it said
// (Wow.exe 3.3.5a 0x0076cff0 and 0x0076cfa0).
//
// The hour from SMSG_LOGIN_SETTIMESPEED was stored and never advanced, so the
// sky, the sun and the fog stayed at the login hour for the whole session.
#include <catch_amalgamated.hpp>

#include "game/game_clock.hpp"

#include <cmath>

using wowee::game::advanceGameClockHours;
using wowee::game::clampGameTimeSpeed;
using wowee::game::kDefaultGameTimeSpeed;

TEST_CASE("the server's speed is a game day per real day", "[game-clock]") {
    // 1/60 game minutes a second: an hour of real time is an hour of game time.
    CHECK(advanceGameClockHours(20.0f, 3600.0, 1.0f / 60.0f) == Catch::Approx(21.0f));
    CHECK(advanceGameClockHours(20.0f, 0.0, 1.0f / 60.0f) == Catch::Approx(20.0f));
    CHECK(advanceGameClockHours(6.5f, 90.0, 1.0f / 60.0f) ==
          Catch::Approx(6.5f + 1.5f / 60.0f));
}

TEST_CASE("the clock wraps at midnight", "[game-clock]") {
    CHECK(advanceGameClockHours(23.5f, 3600.0, 1.0f / 60.0f) == Catch::Approx(0.5f));
    CHECK(advanceGameClockHours(12.0f, 86400.0, 1.0f / 60.0f) == Catch::Approx(12.0f));
}

TEST_CASE("the speed is clamped as the client clamps it", "[game-clock]") {
    CHECK(clampGameTimeSpeed(1.0f / 60.0f) == kDefaultGameTimeSpeed);
    // Below the floor, zero or not a number: the default, not a stopped clock.
    CHECK(clampGameTimeSpeed(0.0f) == kDefaultGameTimeSpeed);
    CHECK(clampGameTimeSpeed(0.001f) == kDefaultGameTimeSpeed);
    CHECK(clampGameTimeSpeed(std::nanf("")) == kDefaultGameTimeSpeed);
    CHECK(clampGameTimeSpeed(1.0f) == 1.0f);
    CHECK(clampGameTimeSpeed(500.0f) == 60.0f);
    // A zero speed still runs the clock at the default rate.
    CHECK(advanceGameClockHours(10.0f, 3600.0, 0.0f) == Catch::Approx(11.0f));
    // A faster server: one game minute per real second.
    CHECK(advanceGameClockHours(10.0f, 60.0, 1.0f) == Catch::Approx(11.0f));
}

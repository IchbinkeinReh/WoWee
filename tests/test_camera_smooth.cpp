// The camera's follow smoothing under cameraSmoothStyle 4, Smarter.
#include <catch_amalgamated.hpp>
#include "rendering/camera_smooth.hpp"

using namespace wowee::rendering::camera_smooth;
using Catch::Matchers::WithinAbs;

TEST_CASE("the controls make the state, the highest one winning", "[camera][smooth]") {
    CHECK(stateFor(false, false, false, false) == State::Idle);
    CHECK(stateFor(true, false, false, false) == State::Move);
    CHECK(stateFor(true, true, false, false) == State::Strafe);
    CHECK(stateFor(true, true, true, false) == State::Turn);
    CHECK(stateFor(false, false, false, true) == State::Turn);
}

TEST_CASE("moving swings an orbited camera back behind over the angle at 180 a second",
          "[camera][smooth]") {
    const Plan p = plan(State::Move, 90.0f, 20.0f, false, false);
    CHECK(p.easeYaw);
    CHECK(p.yawTo == 0.0f);
    CHECK_FALSE(p.easePitch);  // 20 is inside 0..30
    CHECK_THAT(p.seconds, WithinAbs(0.5, 1e-5));
    // The long way round is never taken: 350 off is 10 the other way.
    CHECK_THAT(plan(State::Move, 350.0f, 20.0f, false, false).seconds, WithinAbs(0.1, 1e-5));
    // Held to 0.1 .. 2 seconds.
    CHECK_THAT(plan(State::Strafe, 5.0f, 20.0f, false, false).seconds, WithinAbs(0.1, 1e-5));
}

TEST_CASE("standing still stops a swing where it is", "[camera][smooth]") {
    const Plan p = plan(State::Idle, 45.0f, 50.0f, false, false);
    CHECK(p.stopYaw);
    CHECK(p.stopPitch);
    CHECK_FALSE(p.easeYaw);
    CHECK_FALSE(p.easePitch);
}

TEST_CASE("the pitch is brought into 0..30 at 45 a second, and only from outside it",
          "[camera][smooth]") {
    Plan p = plan(State::Move, 0.0f, 60.0f, false, false);
    CHECK(p.easePitch);
    CHECK(p.pitchTo == 30.0f);
    CHECK_FALSE(p.easeYaw);
    CHECK_THAT(p.seconds, WithinAbs(30.0 / 45.0, 1e-5));
    p = plan(State::Move, 0.0f, -15.0f, false, false);
    CHECK(p.pitchTo == 0.0f);
    // Both at once take the longer time.
    p = plan(State::Turn, 90.0f, 75.0f, false, false);
    CHECK(p.easeYaw);
    CHECK(p.easePitch);
    CHECK_THAT(p.seconds, WithinAbs(1.0, 1e-5));
    // Swimming or flying, the pitch is the mover's.
    CHECK_FALSE(plan(State::Move, 0.0f, 60.0f, false, true).easePitch);
}

TEST_CASE("a mouse button holding the camera keeps it where it is", "[camera][smooth]") {
    const Plan p = plan(State::Move, 90.0f, 60.0f, true, false);
    CHECK_FALSE(p.easeYaw);
    CHECK_FALSE(p.easePitch);
    CHECK_FALSE(p.stopYaw);
    CHECK_FALSE(p.stopPitch);
}

TEST_CASE("carried by a server spline the camera's state is Fear, over every control", "[camera][smooth]") {
    CHECK(stateFor(true, true, true, true, false, true) == State::Fear);
    CHECK(stateFor(false, false, false, false, false, true) == State::Fear);
    // Track ranks under the player's own controls, over standing still.
    CHECK(stateFor(false, false, false, false, true, false) == State::Track);
    CHECK(stateFor(true, false, false, false, true, false) == State::Move);
    CHECK(stateFor(false, false, false, false) == State::Idle);
}

TEST_CASE("Fear eases behind after 0.4 s, ten times slower", "[camera][smooth]") {
    // 0x005fd910's Smarter Fear: delay 0.4, factor 10.
    Plan p = plan(State::Fear, 9.0f, 15.0f, false, false);
    CHECK(p.easeYaw);
    CHECK(p.yawTo == 0.0f);
    CHECK_THAT(p.delay, WithinAbs(0.4, 1e-6));
    // 9 degrees at 180 a second is 0.05 s, times 10: half a second.
    CHECK_THAT(p.seconds, WithinAbs(0.5, 1e-5));
    // A wide turn is held to the two seconds at most.
    p = plan(State::Fear, 120.0f, 15.0f, false, false);
    CHECK_THAT(p.seconds, WithinAbs(2.0, 1e-6));
    // The controls' states start at once.
    CHECK(plan(State::Move, 90.0f, 15.0f, false, false).delay == 0.0f);
    // A mouse button holding the camera holds it in fear too.
    CHECK_FALSE(plan(State::Fear, 90.0f, 15.0f, true, false).easeYaw);
}

TEST_CASE("a feared player is on its spline though it has no mover", "[camera][smooth]") {
    // Fear takes the player's control first and leaves no active mover; the
    // spline is still the player's, and the camera still looks at it.
    constexpr uint64_t player = 0xc;
    CHECK(playerOnServerSpline(true, false, false, player, player));
    // Not without a spline, on a flight path, in a seat, or looking through
    // a far sight object.
    CHECK_FALSE(playerOnServerSpline(false, false, false, player, player));
    CHECK_FALSE(playerOnServerSpline(true, true, false, player, player));
    CHECK_FALSE(playerOnServerSpline(true, false, true, player, player));
    CHECK_FALSE(playerOnServerSpline(true, false, false, 0x1234, player));
}

TEST_CASE("the Fear ease starts again for a new direction, not for each step of a turn",
          "[camera][smooth]") {
    CHECK(fearPlansAgain(false, 10.0f, 10.0f));
    // The character turning toward the spline's way a little each frame.
    CHECK_FALSE(fearPlansAgain(true, 10.4f, 10.0f));
    CHECK_FALSE(fearPlansAgain(true, 19.0f, 10.0f));
    // A new run the other way.
    CHECK(fearPlansAgain(true, 100.0f, 10.0f));
    CHECK_FALSE(fearPlansAgain(true, -176.0f, 175.0f));  // 9 degrees across the wrap
    CHECK(fearPlansAgain(true, -170.0f, 170.0f));
}

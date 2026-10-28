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

// MountFSM banking: the client banks a flying mount only, rolling it into the
// turn, proportional to the turn rate and clamped to a quarter turn
// (FUN_00719b80). A ground mount is never rolled. This guards the regression
// where the lean was computed in degrees (so any turn pinned it), applied to
// every moving mount, and never gated on flight.
#include <catch_amalgamated.hpp>

#include <cmath>

#include "rendering/animation/mount_fsm.hpp"

using wowee::rendering::MountFSM;

namespace {

// Drive the FSM for a few seconds at a steady turn and return the settled roll.
float settledRoll(MountFSM& fsm, float degPerSec, bool flying) {
    const float dt = 1.0f / 60.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    for (int i = 0; i < 240; ++i) {
        yaw += degPerSec * dt;
        MountFSM::Input in;
        in.moving = true;
        in.movingForward = true;
        in.flying = flying;
        in.grounded = !flying;
        in.deltaTime = dt;
        in.characterYaw = yaw;
        roll = fsm.evaluate(in).mountRoll;
    }
    return roll;
}

}  // namespace

TEST_CASE("a ground mount is never banked", "[mount_fsm]") {
    MountFSM fsm;
    fsm.configure({}, /*taxiFlight=*/false);
    // Even a hard turn leaves a mount on the ground level: the client gates the
    // roll on the movement block's flying bit.
    CHECK(std::abs(settledRoll(fsm, 180.0f, /*flying=*/false)) < 1e-3f);
}

TEST_CASE("a flying mount banks into the turn, proportional and clamped", "[mount_fsm]") {
    const float quarterTurn = 6.28318530717958647692f * 0.125f;  // pi/4

    // A gentle 90 deg/s turn gives a modest bank, nowhere near the cap - the
    // degrees-vs-radians bug used to pin even this at the old 0.25 clamp.
    {
        MountFSM fsm;
        fsm.configure({}, false);
        const float roll = settledRoll(fsm, 90.0f, /*flying=*/true);
        CHECK(roll > 0.15f);
        CHECK(roll < 0.45f);
    }

    // A spin faster than the cap settles exactly at a quarter turn.
    {
        MountFSM fsm;
        fsm.configure({}, false);
        const float roll = settledRoll(fsm, 1000.0f, /*flying=*/true);
        CHECK(roll == Catch::Approx(quarterTurn).margin(1e-3));
    }
}

// A mount's banking: the client banks a flying mount only, rolling it into the
// turn, proportional to the turn rate and clamped to a quarter turn
// (FUN_00719b80). A ground mount is never rolled. This guards the regression
// where the lean was computed in degrees (so any turn pinned it), applied to
// every moving mount, and never gated on flight.
#include <catch_amalgamated.hpp>

#include <cmath>

#include "rendering/animation/mount_motion.hpp"

using wowee::rendering::MountMotion;

namespace {

// A few seconds at a steady turn, and the settled roll.
float settledRoll(MountMotion& mount, float degPerSec, bool flying) {
    const float dt = 1.0f / 60.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    for (int i = 0; i < 240; ++i) {
        yaw += degPerSec * dt;
        MountMotion::Input in;
        in.moving = true;
        in.forwardOrBack = true;
        in.flying = flying;
        in.deltaTime = dt;
        in.characterYaw = yaw;
        roll = mount.update(in).roll;
    }
    return roll;
}

}  // namespace

TEST_CASE("a ground mount is never banked", "[mount_bank]") {
    MountMotion mount;
    mount.reset();
    // Even a hard turn leaves a mount on the ground level: the client gates the
    // roll on the movement block's flying bit.
    CHECK(std::abs(settledRoll(mount, 180.0f, /*flying=*/false)) < 1e-3f);
}

TEST_CASE("a flying mount banks into the turn, proportional and clamped", "[mount_bank]") {
    const float quarterTurn = 6.28318530717958647692f * 0.125f;  // pi/4

    // A gentle 90 deg/s turn gives a modest bank, nowhere near the cap - the
    // degrees-vs-radians bug used to pin even this at the old 0.25 clamp.
    {
        MountMotion mount;
        mount.reset();
        const float roll = settledRoll(mount, 90.0f, /*flying=*/true);
        // A left turn (yaw growing): the left side down, a negative roll.
        CHECK(roll < -0.15f);
        CHECK(roll > -0.45f);
    }

    // A spin faster than the cap settles exactly at a quarter turn.
    {
        MountMotion mount;
        mount.reset();
        const float roll = settledRoll(mount, 1000.0f, /*flying=*/true);
        CHECK(roll == Catch::Approx(-quarterTurn).margin(1e-3));
    }
}

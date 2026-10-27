// MountFSM's jump: the unit's own jump on the mount's model, as the client
// plays it (JumpStart on MSG_MOVE_JUMP, Jump once that ends, JumpEnd on
// landing, 0x0073ed3f and 0x0073b510) - standing or moving, no MountSpecial.

#include <catch_amalgamated.hpp>

#include "rendering/animation/mount_fsm.hpp"

using wowee::rendering::MountFSM;

namespace {

MountFSM::MountAnimSet anims() {
    MountFSM::MountAnimSet a;
    a.stand = 0;
    a.run = 5;
    a.jumpStart = 37;
    a.jumpLoop = 38;
    a.jumpEnd = 39;
    return a;
}

// The input for a frame with the mount playing `anim` at `time` of `duration`.
MountFSM::Input frame(bool grounded, uint32_t anim, float time, float duration, bool moving = false) {
    MountFSM::Input in;
    in.grounded = grounded;
    in.moving = moving;
    in.deltaTime = 0.016f;
    in.haveMountState = true;
    in.curMountAnim = anim;
    in.curMountTime = time;
    in.curMountDuration = duration;
    return in;
}

}  // namespace

TEST_CASE("a standing mount jumps as a unit does, through JumpStart, Jump and JumpEnd", "[mount_fsm]") {
    MountFSM fsm;
    fsm.configure(anims(), false);
    auto in = frame(true, 0, 0.0f, 4000.0f);
    in.jumpKeyPressed = true;
    auto out = fsm.evaluate(in);
    CHECK(out.mountAnimChanged);
    CHECK(out.mountAnimId == 37u);  // not MountSpecial (94)
    CHECK_FALSE(out.mountAnimLoop);
    CHECK(out.triggerMountJump);

    // Still on the ground the frame after: no landing yet.
    out = fsm.evaluate(frame(true, 37, 50.0f, 667.0f));
    CHECK(out.mountAnimId == 37u);
    // In the air, JumpStart through: Jump, looping.
    out = fsm.evaluate(frame(false, 37, 667.0f, 667.0f));
    CHECK(out.mountAnimChanged);
    CHECK(out.mountAnimId == 38u);
    CHECK(out.mountAnimLoop);
    out = fsm.evaluate(frame(false, 38, 300.0f, 800.0f));
    CHECK(out.mountAnimId == 38u);
    // Down: JumpEnd once.
    out = fsm.evaluate(frame(true, 38, 400.0f, 800.0f));
    CHECK(out.mountAnimChanged);
    CHECK(out.mountAnimId == 39u);
    CHECK_FALSE(out.mountAnimLoop);
    CHECK(out.playLandSound);
    // Through: the stand again.
    out = fsm.evaluate(frame(true, 39, 500.0f, 500.0f));
    CHECK(out.mountAnimChanged);
    CHECK(out.mountAnimId == 0u);
    CHECK(fsm.getAction() == MountFSM::MountAction::None);
}

TEST_CASE("a short hop lands before JumpStart is through", "[mount_fsm]") {
    MountFSM fsm;
    fsm.configure(anims(), false);
    auto in = frame(true, 5, 0.0f, 600.0f, true);
    in.jumpKeyPressed = true;
    auto out = fsm.evaluate(in);
    CHECK(out.mountAnimId == 37u);
    out = fsm.evaluate(frame(false, 37, 200.0f, 667.0f, true));
    CHECK(out.mountAnimId == 37u);
    // Landed while JumpStart still plays: JumpEnd, then the run.
    out = fsm.evaluate(frame(true, 37, 400.0f, 667.0f, true));
    CHECK(out.mountAnimId == 39u);
    out = fsm.evaluate(frame(true, 39, 500.0f, 500.0f, true));
    CHECK(out.mountAnimId == 5u);
}

TEST_CASE("a mount with no JumpStart starts on Jump", "[mount_fsm]") {
    MountFSM fsm;
    auto a = anims();
    a.jumpStart = 0;
    fsm.configure(a, false);
    auto in = frame(true, 0, 0.0f, 4000.0f);
    in.jumpKeyPressed = true;
    const auto out = fsm.evaluate(in);
    CHECK(out.mountAnimId == 38u);
    CHECK(out.mountAnimLoop);
}

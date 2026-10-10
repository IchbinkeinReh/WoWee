// LocomotionFSM unit tests
#include <catch_amalgamated.hpp>
#include "rendering/animation/locomotion_fsm.hpp"
#include "rendering/animation/animation_ids.hpp"

using namespace wowee::rendering;
namespace anim = wowee::rendering::anim;

// Helper: create a capability set with basic locomotion resolved
static AnimCapabilitySet makeLocoCaps() {
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.resolvedRun = anim::RUN;
    caps.resolvedSprint = anim::SPRINT;
    caps.resolvedWalkBackwards = anim::WALK_BACKWARDS;
    caps.resolvedJumpStart = anim::JUMP_START;
    caps.resolvedJump = anim::JUMP;
    caps.resolvedJumpEnd = anim::JUMP_END;
    caps.resolvedJumpLandRun = anim::JUMP_LAND_RUN;
    caps.resolvedSwimIdle = anim::SWIM_IDLE;
    caps.resolvedSwim = anim::SWIM;
    caps.hasStand = true;
    caps.hasWalk = true;
    caps.hasRun = true;
    caps.hasSprint = true;
    caps.hasWalkBackwards = true;
    caps.hasJump = true;
    caps.hasSwim = true;
    return caps;
}

static LocomotionFSM::Input idle() {
    LocomotionFSM::Input in;
    in.deltaTime = 0.016f;
    return in;
}

TEST_CASE("LocomotionFSM: IDLE → WALK on move start (non-sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::WALK);
    REQUIRE(out.loop == true);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);
}

TEST_CASE("LocomotionFSM: IDLE → RUN on move start (sprinting)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::RUN);
    CHECK(fsm.getState() == LocomotionFSM::State::RUN);
}

TEST_CASE("LocomotionFSM: WALK → IDLE on move stop (after grace)", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    // Stop moving - grace timer keeps walk for a bit
    in.moving = false;
    in.deltaTime = 0.2f; // > grace period (0.12s)
    auto out = fsm.resolve(in, caps);

    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

TEST_CASE("LocomotionFSM: WALK → JUMP_START on jump", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Start walking
    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);

    // Jump
    in.jumping = true;
    in.grounded = false;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::JUMP_START);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_START);
}

TEST_CASE("LocomotionFSM: SWIM_IDLE → SWIM on move start while swimming", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    // Enter swim idle
    auto in = idle();
    in.swimming = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM_IDLE);

    // Start swimming
    in.moving = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    REQUIRE(out.animId == anim::SWIM);
    CHECK(fsm.getState() == LocomotionFSM::State::SWIM);
}

TEST_CASE("LocomotionFSM: backward walking resolves WALK_BACKWARDS", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    REQUIRE(out.valid);
    // Should use WALK_BACKWARDS when available
    CHECK(out.animId == anim::WALK_BACKWARDS);
}

TEST_CASE("LocomotionFSM: STAY when WALK_BACKWARDS missing from caps", "[locomotion]") {
    LocomotionFSM fsm;
    AnimCapabilitySet caps;
    caps.resolvedStand = anim::STAND;
    caps.resolvedWalk = anim::WALK;
    caps.hasStand = true;
    caps.hasWalk = true;
    // No WALK_BACKWARDS in caps

    auto in = idle();
    in.moving = true;
    in.movingBackward = true;
    auto out = fsm.resolve(in, caps);

    // Should still resolve something - falls back to walk
    REQUIRE(out.valid);
}

TEST_CASE("LocomotionFSM: reset restores IDLE", "[locomotion]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();

    auto in = idle();
    in.moving = true;
    fsm.resolve(in, caps);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    fsm.reset();
    CHECK(fsm.getState() == LocomotionFSM::State::IDLE);
}

// Landing, as the client's FUN_0073d2b0 chooses it: JumpEnd with no direction
// held, JumpLandRun running on, and straight back to moving when walking or
// backing up. WoWee dropped the landing whenever the player was moving.
// The game reports the playing animation; mid-air that is the Jump loop.
static LocomotionFSM::Input airborneAnim(LocomotionFSM::Input in) {
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP;
    in.currentAnimDuration = 1.0f;
    in.currentAnimTime = 0.2f;
    return in;
}

static LocomotionFSM landFrom(LocomotionFSM::Input landing) {
    LocomotionFSM fsm;
    fsm.setState(LocomotionFSM::State::JUMP_MID);
    landing.grounded = true;
    fsm.resolve(airborneAnim(landing), makeLocoCaps());   // touches down
    return fsm;
}

TEST_CASE("LocomotionFSM: landing standing plays JumpEnd", "[locomotion]") {
    auto fsm = landFrom(idle());
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);
    auto out = fsm.resolve(airborneAnim(idle()), makeLocoCaps());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::JUMP_END);
    CHECK_FALSE(out.loop);
}

TEST_CASE("LocomotionFSM: landing while running on plays JumpLandRun", "[locomotion]") {
    auto in = idle();
    in.moving = true;
    in.movingForward = true;
    in.sprinting = true;
    auto fsm = landFrom(in);
    auto out = fsm.resolve(airborneAnim(in), makeLocoCaps());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::JUMP_LAND_RUN);
    CHECK_FALSE(out.loop);
    CHECK(fsm.getState() == LocomotionFSM::State::JUMP_END);

    // Once it has played, the run resumes.
    in.haveAnimState = true;
    in.currentAnimId = anim::JUMP_LAND_RUN;
    in.currentAnimDuration = 0.5f;
    in.currentAnimTime = 0.5f;
    out = fsm.resolve(in, makeLocoCaps());
    REQUIRE(out.valid);
    CHECK(out.animId == anim::RUN);
}

TEST_CASE("LocomotionFSM: landing while walking or backing up has no landing", "[locomotion]") {
    auto walking = idle();
    walking.moving = true;
    walking.movingForward = true;
    auto fsm = landFrom(walking);
    CHECK(fsm.getState() == LocomotionFSM::State::WALK);

    auto backing = idle();
    backing.moving = true;
    backing.movingBackward = true;
    backing.sprinting = true;
    auto fsm2 = landFrom(backing);
    CHECK(fsm2.getState() == LocomotionFSM::State::RUN);
}

// ── Turning on the spot (FUN_0073dab0 / FUN_0071e180) ──────────────────────

TEST_CASE("LocomotionFSM: standing, a turn shuffles the feet", "[locomotion][shuffle]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    caps.resolvedShuffleLeft = anim::SHUFFLE_LEFT;
    caps.resolvedShuffleRight = anim::SHUFFLE_RIGHT;

    auto in = idle();
    in.shuffle = body_yaw::TurnShuffle::Left;
    auto out = fsm.resolve(in, caps);
    REQUIRE(out.valid);
    CHECK(out.animId == anim::SHUFFLE_LEFT);
    CHECK(out.loop);

    in.shuffle = body_yaw::TurnShuffle::Right;
    CHECK(fsm.resolve(in, caps).animId == anim::SHUFFLE_RIGHT);

    // Done turning: the stand comes back.
    in.shuffle = body_yaw::TurnShuffle::None;
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

TEST_CASE("LocomotionFSM: moving, the turn is no shuffle", "[locomotion][shuffle]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    caps.resolvedShuffleLeft = anim::SHUFFLE_LEFT;
    auto in = idle();
    in.moving = true;
    in.sprinting = true;
    in.shuffle = body_yaw::TurnShuffle::Left;
    CHECK(fsm.resolve(in, caps).animId == anim::RUN);
}

TEST_CASE("LocomotionFSM: a model without the shuffle turns in its stand", "[locomotion][shuffle]") {
    LocomotionFSM fsm;
    auto caps = makeLocoCaps();
    auto in = idle();
    in.shuffle = body_yaw::TurnShuffle::Right;
    CHECK(fsm.resolve(in, caps).animId == anim::STAND);
}

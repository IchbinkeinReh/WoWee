// The client's jump landing (FUN_0073d2b0) and the airborne animation hold
// (FUN_00724200 / FUN_0071dc20), shared by the local player's LocomotionFSM
// and other units' animation sync.
#include <catch_amalgamated.hpp>
#include "rendering/animation/jump_landing.hpp"

using namespace wowee::rendering;

namespace {
constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4;
constexpr uint32_t kWalking = 0x100;
constexpr float kWalk = 2.5f, kRun = 7.0f;
}

TEST_CASE("Landing with no direction held plays JumpEnd", "[jump_landing]") {
    CHECK(chooseJumpLanding(false, false, false) == JumpLanding::End);
    CHECK(chooseJumpLanding(false, false, true) == JumpLanding::End);
    CHECK(jumpLandingForFlags(0, 0.0f, kWalk) == JumpLanding::End);
}

TEST_CASE("Landing on the run plays JumpLandRun", "[jump_landing]") {
    CHECK(chooseJumpLanding(true, false, true) == JumpLanding::LandRun);
    CHECK(jumpLandingForFlags(kForward, kRun, kWalk) == JumpLanding::LandRun);
    CHECK(jumpLandingForFlags(kStrafeLeft, kRun, kWalk) == JumpLanding::LandRun);
}

TEST_CASE("Landing walking, backing up or slow goes straight to locomotion", "[jump_landing]") {
    CHECK(chooseJumpLanding(true, true, true) == JumpLanding::None);
    CHECK(chooseJumpLanding(true, false, false) == JumpLanding::None);
    CHECK(jumpLandingForFlags(kForward | kWalking, kRun, kWalk) == JumpLanding::None);
    CHECK(jumpLandingForFlags(kBackward, 4.5f, kWalk) == JumpLanding::None);
    // FUN_00716fa0: at or under twice walk speed is no landing on the run.
    CHECK(jumpLandingForFlags(kForward, 2.0f * kWalk, kWalk) == JumpLanding::None);
    CHECK(jumpLandingForFlags(kForward, 2.0f * kWalk + 0.01f, kWalk) == JumpLanding::LandRun);
}

TEST_CASE("Turning alone is no direction for the landing", "[jump_landing]") {
    constexpr uint32_t kTurnLeft = 0x10;
    CHECK(jumpLandingForFlags(kTurnLeft, 0.0f, kWalk) == JumpLanding::End);
}

TEST_CASE("Airborne units keep their jump animations, or fall", "[jump_landing]") {
    CHECK(airborneAnimSync(true, anim::JUMP_START, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(true, anim::JUMP, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(true, anim::JUMP_END, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(true, anim::FALL, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(true, anim::RUN, false, false) == AirborneAnimSync::Fall);
    CHECK(airborneAnimSync(true, anim::STAND, false, false) == AirborneAnimSync::Fall);
}

TEST_CASE("Grounded units play out the landing, then leave the air loops", "[jump_landing]") {
    CHECK(airborneAnimSync(false, anim::JUMP_END, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(false, anim::JUMP_LAND_RUN, false, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(false, anim::JUMP_START, false, false) == AirborneAnimSync::Refresh);
    CHECK(airborneAnimSync(false, anim::JUMP, false, false) == AirborneAnimSync::Refresh);
    CHECK(airborneAnimSync(false, anim::FALL, false, false) == AirborneAnimSync::Refresh);
    CHECK(airborneAnimSync(false, anim::RUN, false, false) == AirborneAnimSync::None);
    CHECK(airborneAnimSync(false, anim::STAND, false, false) == AirborneAnimSync::None);
}

TEST_CASE("A movement packet cuts a landing short, not a fall", "[jump_landing]") {
    // FUN_0073ed10 asks for the unit's locomotion (FUN_0073ac30) on a start,
    // stop or turn; once the unit is down that replaces the landing.
    CHECK(airborneAnimSync(false, anim::JUMP_END, false, true) == AirborneAnimSync::Refresh);
    CHECK(airborneAnimSync(false, anim::JUMP_LAND_RUN, false, true) == AirborneAnimSync::Refresh);
    CHECK(airborneAnimSync(false, anim::RUN, false, true) == AirborneAnimSync::None);
    // In the air FUN_00724200 still holds the jump.
    CHECK(airborneAnimSync(true, anim::JUMP, false, true) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(true, anim::JUMP_END, false, true) == AirborneAnimSync::Hold);
}

TEST_CASE("Taking off into flight plays JumpStart out", "[jump_landing]") {
    CHECK(airborneAnimSync(false, anim::JUMP_START, true, false) == AirborneAnimSync::Hold);
    CHECK(airborneAnimSync(false, anim::JUMP_START, true, true) == AirborneAnimSync::Hold);
    // Then the flight's locomotion, from the Jump it gives way to.
    CHECK(airborneAnimSync(false, anim::JUMP, true, false) == AirborneAnimSync::Refresh);
    // Landing from flight holds JumpLandRun like any landing.
    CHECK(airborneAnimSync(false, anim::JUMP_LAND_RUN, false, false) == AirborneAnimSync::Hold);
}

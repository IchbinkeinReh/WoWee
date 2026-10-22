// What the active player's own movement events do to its animation
// (0x0073ed10, as 0x00741xxx ends each of them in it).
#include <catch_amalgamated.hpp>
#include "rendering/animation/mover_event.hpp"

using namespace wowee::rendering::mover_event;
using wowee::game::Opcode;

namespace {
constexpr uint32_t kForward = 0x1, kFlying = 0x2000000;
}

TEST_CASE("Starting to move asks the base animation and lets the bow go", "[mover_event]") {
    for (const Opcode op : {Opcode::MSG_MOVE_START_FORWARD, Opcode::MSG_MOVE_START_BACKWARD,
                            Opcode::MSG_MOVE_START_STRAFE_LEFT, Opcode::MSG_MOVE_START_STRAFE_RIGHT,
                            Opcode::MSG_MOVE_START_SWIM, Opcode::MSG_MOVE_START_ASCEND,
                            Opcode::MSG_MOVE_START_DESCEND}) {
        const Reaction r = react(op, kForward, false, false);
        CHECK(r.play == Play::Base);
        CHECK(r.releasesBow);
    }
}

TEST_CASE("Stopping and turning ask the base animation, the bow kept", "[mover_event]") {
    for (const Opcode op : {Opcode::MSG_MOVE_STOP, Opcode::MSG_MOVE_STOP_STRAFE, Opcode::MSG_MOVE_START_TURN_LEFT,
                            Opcode::MSG_MOVE_STOP_TURN, Opcode::MSG_MOVE_STOP_SWIM, Opcode::MSG_MOVE_STOP_ASCEND,
                            Opcode::MSG_MOVE_TELEPORT_ACK, Opcode::CMSG_FORCE_MOVE_ROOT_ACK}) {
        const Reaction r = react(op, 0, false, false);
        CHECK(r.play == Play::Base);
        CHECK_FALSE(r.releasesBow);
    }
}

TEST_CASE("Walk or run mode and speed changes matter only under way", "[mover_event]") {
    CHECK(react(Opcode::MSG_MOVE_SET_WALK_MODE, kForward, false, false).play == Play::Base);
    CHECK(react(Opcode::MSG_MOVE_SET_WALK_MODE, 0, false, false).play == Play::Nothing);
    CHECK(react(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, kForward, false, false).play == Play::Base);
    CHECK(react(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, 0x10, false, false).play == Play::Nothing);
}

TEST_CASE("A jump plays JumpStart, a knockback Fall, unless the seat holds", "[mover_event]") {
    const Reaction jump = react(Opcode::MSG_MOVE_JUMP, kForward, false, false);
    CHECK(jump.play == Play::JumpStart);
    CHECK(jump.releasesBow);
    const Reaction seated = react(Opcode::MSG_MOVE_JUMP, kForward, false, true);
    CHECK(seated.play == Play::Nothing);
    CHECK(seated.releasesBow);
    CHECK(react(Opcode::CMSG_MOVE_KNOCK_BACK_ACK, 0, false, false).play == Play::Fall);
    CHECK(react(Opcode::CMSG_MOVE_KNOCK_BACK_ACK, 0, false, true).play == Play::Nothing);
}

TEST_CASE("Taking off and landing from flight", "[mover_event]") {
    CHECK(react(Opcode::CMSG_MOVE_SET_FLY, kFlying, false, false).play == Play::JumpStart);
    CHECK(react(Opcode::CMSG_MOVE_SET_FLY, kFlying, true, false).play == Play::Base);
    CHECK(react(Opcode::CMSG_MOVE_SET_FLY, 0, false, false).play == Play::JumpLandRun);
}

TEST_CASE("A heartbeat or a landing asks nothing here", "[mover_event]") {
    CHECK(react(Opcode::MSG_MOVE_HEARTBEAT, kForward, false, false).play == Play::Nothing);
    CHECK(react(Opcode::MSG_MOVE_FALL_LAND, kForward, false, false).play == Play::Nothing);
    CHECK(react(Opcode::MSG_MOVE_SET_FACING, 0, false, false).play == Play::Nothing);
}

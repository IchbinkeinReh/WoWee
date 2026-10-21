#include "rendering/animation/mover_event.hpp"

namespace wowee {
namespace rendering {
namespace mover_event {

Reaction react(game::Opcode op, uint32_t flags, bool fallPlaying, bool seatHolds) {
    using game::Opcode;
    constexpr uint32_t kMoving = 0xc0100f, kFlying = 0x2000000;
    switch (op) {
        // Under way, back, a strafe, into water, a climb or a dive.
        case Opcode::MSG_MOVE_START_FORWARD:
        case Opcode::MSG_MOVE_START_BACKWARD:
        case Opcode::MSG_MOVE_START_STRAFE_LEFT:
        case Opcode::MSG_MOVE_START_STRAFE_RIGHT:
        case Opcode::MSG_MOVE_START_SWIM:
        case Opcode::MSG_MOVE_START_SWIM_CHEAT:
        case Opcode::MSG_MOVE_START_ASCEND:
        case Opcode::MSG_MOVE_START_DESCEND:
            return {Play::Base, true};
        // A stop, a turn, out of water, a teleport, a root.
        case Opcode::MSG_MOVE_STOP:
        case Opcode::MSG_MOVE_STOP_STRAFE:
        case Opcode::MSG_MOVE_START_TURN_LEFT:
        case Opcode::MSG_MOVE_START_TURN_RIGHT:
        case Opcode::MSG_MOVE_STOP_TURN:
        case Opcode::MSG_MOVE_TELEPORT:
        case Opcode::MSG_MOVE_TELEPORT_ACK:
        case Opcode::MSG_MOVE_STOP_SWIM:
        case Opcode::MSG_MOVE_TOGGLE_COLLISION_CHEAT:
        case Opcode::CMSG_FORCE_MOVE_ROOT_ACK:
        case Opcode::MSG_MOVE_ROOT:
        case Opcode::SMSG_SPLINE_MOVE_ROOT:
        case Opcode::MSG_MOVE_STOP_SWIM_CHEAT:
        case Opcode::MSG_MOVE_STOP_ASCEND:
        case Opcode::MSG_MOVE_UPDATE_CAN_FLY:
        case Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK:
        case Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK:
        case Opcode::SMSG_SPLINE_MOVE_GRAVITY_DISABLE:
        case Opcode::SMSG_SPLINE_MOVE_GRAVITY_ENABLE:
            return {Play::Base, false};
        // Walk or run, or a speed: only while it moves.
        case Opcode::MSG_MOVE_SET_RUN_MODE:
        case Opcode::MSG_MOVE_SET_WALK_MODE:
        case Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:
        case Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
            return {(flags & kMoving) != 0 ? Play::Base : Play::Nothing, false};
        case Opcode::MSG_MOVE_JUMP:
            return {seatHolds ? Play::Nothing : Play::JumpStart, true};
        case Opcode::CMSG_MOVE_KNOCK_BACK_ACK:
            return {seatHolds ? Play::Nothing : Play::Fall, false};
        // Into flight JumpStart, unless it is falling; down from it
        // JumpLandRun.
        case Opcode::CMSG_MOVE_SET_FLY:
            if ((flags & kFlying) == 0) return {seatHolds ? Play::Nothing : Play::JumpLandRun, false};
            if (fallPlaying) return {Play::Base, false};
            return {seatHolds ? Play::Nothing : Play::JumpStart, false};
        default:
            return {};
    }
}

}  // namespace mover_event
}  // namespace rendering
}  // namespace wowee

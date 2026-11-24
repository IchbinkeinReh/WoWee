#pragma once

#include "game/world_packets.hpp"

#include <cstdint>
#include <optional>

namespace wowee {
namespace game {

/// What ends a fall without a landing (0x00988490: FALLING and FALLING_FAR
/// off), the time it lasted kept as after one (+0x80). No landing is sent for
/// it: that goes out only where a step lands the mover on ground
/// (0x006eb0b0 -> 0x0073d4a0). `fellMs` is how long it fell, none where it
/// did not.
inline void endFall(MovementInfo& m, std::optional<uint32_t> fellMs) {
    m.flags &= ~(static_cast<uint32_t>(MovementFlags::FALLING) | static_cast<uint32_t>(MovementFlags::FALLINGFAR));
    if (fellMs) m.fallTime = *fellMs;
    m.jumpVelocity = 0.0f;
    m.jumpSinAngle = 0.0f;
    m.jumpCosAngle = 0.0f;
    m.jumpXYSpeed = 0.0f;
}

/// What starting to swim makes of a mover's movement, before
/// MSG_MOVE_START_SWIM goes out (0x00989660): SWIMMING on, FLYING and
/// SPLINE_ELEVATION off (flags & 0xf9ffffff | 0x200000), and the fall it came
/// in by over.
inline void startSwimming(MovementInfo& m, std::optional<uint32_t> fellMs) {
    m.flags = (m.flags & 0xf9ffffffu) | static_cast<uint32_t>(MovementFlags::SWIMMING);
    endFall(m, fellMs);
}

/// What taking to the air makes of it (0x00989890), where it is allowed to
/// fly: FLYING on, SWIMMING, SPLINE_ELEVATION and a fall off
/// (flags & 0xfbdfcfff | 0x2000000).
inline void startFlying(MovementInfo& m, std::optional<uint32_t> fellMs) {
    if ((m.flags & static_cast<uint32_t>(MovementFlags::CAN_FLY)) == 0) return;
    m.flags = (m.flags & 0xfbdfcfffu) | static_cast<uint32_t>(MovementFlags::FLYING);
    endFall(m, fellMs);
}

}  // namespace game
}  // namespace wowee

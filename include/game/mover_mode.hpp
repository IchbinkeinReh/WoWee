#pragma once

#include "core/coordinates.hpp"
#include "game/entity.hpp"
#include "game/world_packets.hpp"

#include <cstdint>
#include <optional>

namespace wowee {
namespace game {

/// What ends a fall (0x00988490: FALLING and FALLING_FAR off), the time it
/// lasted kept as after one (+0x80). `fellMs` is how long it fell, none where
/// it did not. A root that came while it fell (0x100000, rootMover) holds it
/// from here: flags & 0xff203f00 | ROOT.
inline void endFall(MovementInfo& m, std::optional<uint32_t> fellMs) {
    m.flags &= ~(static_cast<uint32_t>(MovementFlags::FALLING) | static_cast<uint32_t>(MovementFlags::FALLINGFAR));
    if ((m.flags & static_cast<uint32_t>(MovementFlags::PENDING_ROOT)) != 0)
        m.flags = (m.flags & 0xff203f00u) | static_cast<uint32_t>(MovementFlags::ROOT);
    if (fellMs) m.fallTime = *fellMs;
    m.jumpVelocity = 0.0f;
    m.jumpSinAngle = 0.0f;
    m.jumpCosAngle = 0.0f;
    m.jumpXYSpeed = 0.0f;
}

/// Whether the mover may start to fall where it is (0x0098b710): not swimming,
/// flying or falling already (0x2201000), nor with gravity off (0x400,
/// 0x006eaba0), and not rooted or on a transport (0xa00, 0x00988370).
[[nodiscard]] inline constexpr bool mayStartFall(uint32_t flags) {
    return (flags & (0x2201000u | 0x400u | 0xa00u)) == 0;
}

/// What a fall's start makes of the flags (0x00988370): FALLING on; swimming,
/// ascending, descending and flying off (flags & 0xf91fffff), and the pitch's
/// two with them unless the mover may pitch (flags2 & 0x20; 0xf91fff3f).
[[nodiscard]] inline constexpr uint32_t fallFlags(uint32_t flags, uint16_t flags2) {
    return (flags & ((flags2 & 0x0020) != 0 ? 0xf91fffffu : 0xf91fff3fu)) |
           static_cast<uint32_t>(MovementFlags::FALLING);
}

/// Whether a landing goes out as MSG_MOVE_FALL_LAND (0x0073d4a0): the packet
/// before it told of the fall (0x0071ef80 keeps that in +0xa30 0x80), or it
/// fell further than a thirty-sixth of a yard (0x00986fb0). A step down that
/// broke the ground's contact for a moment lands unsaid.
[[nodiscard]] inline constexpr bool fallLandSent(bool fallSaid, float droppedYards) {
    return fallSaid || droppedYards > 1.0f / 36.0f;
}

/// The jump block of the mover's own fall (outgoingJumpBlock), which is in the
/// wire's axes: its direction goes by the facing as the wire has it, a quarter
/// turn on from the one kept here. Taken from that one, every jump and fall
/// went off to the right of where it was going for everybody else.
[[nodiscard]] inline OutgoingJump ownJumpBlock(const MovementInfo& m, float walkSpeed, float runSpeed,
                                               float runBackSpeed) {
    return outgoingJumpBlock(m.flags, core::coords::canonicalToServerYaw(m.orientation), walkSpeed, runSpeed,
                             runBackSpeed);
}

/// What stopping to swim makes of it, before MSG_MOVE_STOP_SWIM goes out
/// (0x0098bff0): swimming, rising and sinking off (flags & 0xff1fffff) and the
/// pitch with them unless the mover may pitch (0xff1fff3f), and a fall begun
/// where it may (0x006eaba0, 0x00988370) - which the packet carries, and which
/// the fall's next step lands where there is ground. True where it fell.
inline bool stopSwimming(MovementInfo& m) {
    if ((m.flags & static_cast<uint32_t>(MovementFlags::SWIMMING)) == 0) return false;
    const bool mayPitch = (m.flags2 & 0x0020) != 0;
    m.flags &= mayPitch ? 0xff1fffffu : 0xff1fff3fu;
    if (!mayPitch) m.pitch = 0.0f;
    if (!mayStartFall(m.flags)) return false;
    m.flags = fallFlags(m.flags, m.flags2);
    return true;
}

/// A root (0x0098c4f0): held at once, except where it falls - then once the
/// fall ends (endFall).
inline void rootMover(MovementInfo& m) {
    constexpr uint32_t kRoot = static_cast<uint32_t>(MovementFlags::ROOT);
    if ((m.flags & kRoot) != 0) return;
    if ((m.flags & static_cast<uint32_t>(MovementFlags::FALLING)) != 0) {
        m.flags |= static_cast<uint32_t>(MovementFlags::PENDING_ROOT);
        return;
    }
    m.flags = ((m.flags | kRoot) & 0xff203f00u);
}

/// Its end (0x0098c550): the root and a waiting one off (flags & 0xffeff7ff).
inline void unrootMover(MovementInfo& m) {
    m.flags &= ~(static_cast<uint32_t>(MovementFlags::ROOT) | static_cast<uint32_t>(MovementFlags::PENDING_ROOT));
}

/// What starting to swim makes of a mover's movement, before
/// MSG_MOVE_START_SWIM goes out (0x00989660): SWIMMING on, FLYING and
/// SPLINE_ELEVATION off (flags & 0xf9ffffff | 0x200000), and the fall it came
/// in by over.
inline void startSwimming(MovementInfo& m, std::optional<uint32_t> fellMs) {
    m.flags = (m.flags & 0xf9ffffffu) | static_cast<uint32_t>(MovementFlags::SWIMMING);
    endFall(m, fellMs);
}

/// A knockback (0x006e9ff0): out of the water and the air, no flying until the
/// server allows it again (0xfeffffff), a root held back until it lands, and
/// the fall begun as the packet has it - its vertical speed, direction and
/// speed across the ground.
inline void knockBackMover(MovementInfo& m, float dirX, float dirY, float speed, float verticalSpeed) {
    constexpr uint32_t kSwimOrFly =
        static_cast<uint32_t>(MovementFlags::SWIMMING) | static_cast<uint32_t>(MovementFlags::FLYING);
    if ((m.flags & kSwimOrFly) != 0) m.pitch = 0.0f;
    if ((m.flags & static_cast<uint32_t>(MovementFlags::ROOT)) != 0)
        m.flags = (m.flags & ~static_cast<uint32_t>(MovementFlags::ROOT)) |
                  static_cast<uint32_t>(MovementFlags::PENDING_ROOT);
    m.flags &= ~static_cast<uint32_t>(MovementFlags::CAN_FLY);
    m.flags = fallFlags(m.flags, m.flags2) & ~static_cast<uint32_t>(MovementFlags::FALLINGFAR);
    m.fallTime = 0;
    m.jumpVelocity = verticalSpeed;
    m.jumpSinAngle = dirX;  // the wire's order: x, then y
    m.jumpCosAngle = dirY;
    m.jumpXYSpeed = speed;
}

/// What being put somewhere makes of it (0x0098ba90, a teleport's): a fall
/// over, level, and only what is not the mover's own doing kept
/// (flags & 0xf9003f00).
inline void placeMover(MovementInfo& m, std::optional<uint32_t> fellMs) {
    endFall(m, fellMs);
    m.flags &= 0xf9003f00u;
    m.pitch = 0.0f;
}

/// What landing from it makes of it, or being forbidden to fly (0x0098c240):
/// FLYING, rising, sinking and the pitch keys off (flags & 0xfd3fff3f), level,
/// and a fall begun where it may (0x006eaba0, 0x00988370). True where it fell.
inline bool stopFlying(MovementInfo& m) {
    m.flags &= 0xfd3fff3fu;
    m.pitch = 0.0f;
    if (!mayStartFall(m.flags)) return false;
    m.flags = fallFlags(m.flags, m.flags2);
    return true;
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

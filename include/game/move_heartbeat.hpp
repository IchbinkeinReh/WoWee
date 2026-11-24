#pragma once

#include <cstdint>

// When the mover's MSG_MOVE_HEARTBEAT goes out.
namespace wowee::game {

/// The flags it goes out under (0x006f09f0): forward, backward, either strafe,
/// falling, rising or sinking. One that stands or only turns sends none.
inline constexpr uint32_t kHeartbeatFlags = 0x00c0100fu;

/// How long after the mover's last packet, of any kind (0x0071f0c0 ->
/// 0x006e9b70).
inline constexpr uint32_t kHeartbeatMs = 500;

/// 0x006f09f0: not while the server moves it along a spline (0x004f5240).
inline bool heartbeatDue(uint32_t moveFlags, bool onServerSpline, uint32_t nowMs, uint32_t lastSentMs) {
    return (moveFlags & kHeartbeatFlags) != 0 && !onServerSpline &&
           static_cast<int32_t>(nowMs - (lastSentMs + kHeartbeatMs)) >= 0;
}

}  // namespace wowee::game

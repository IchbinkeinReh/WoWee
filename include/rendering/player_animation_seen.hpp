#pragma once

// What the player's base animation was last asked for, and whether it is to be
// asked again. The client asks it on events (0x0073ac30: movement events, a
// turn on the spot, alive or dead, ...); this client's frame sees the states
// those events leave, and asks where one changed.

#include <cstdint>

namespace wowee {
namespace rendering {

struct PlayerAnimationSeen {
    uint32_t instanceId = 0;
    bool fellFar = false;
    bool turning = false;
    bool turningLeft = false;
    bool alive = true;
    bool looting = false;
    bool stealthed = false;
    bool autoAttacking = false;
    bool swimming = false;
    bool onFlightPath = false;
    /// How often a spline has taken the player or let it go. A move of the
    /// server's asks at its end (0x0073c8e0 -> 0x0073ac30), and a spline's end
    /// is a stop (0x006eb0b0 -> 0x007413f0(0xb7) -> 0x0073ed10). A fall it cut
    /// short has no landing to ask, and Fall was played on on the ground until
    /// the next step.
    uint32_t splineMoves = 0;
};

[[nodiscard]] inline bool asksBaseAgain(const PlayerAnimationSeen& before, const PlayerAnimationSeen& now) {
    return now.turning != before.turning || now.turningLeft != before.turningLeft || now.alive != before.alive ||
           now.looting != before.looting || now.stealthed != before.stealthed ||
           now.autoAttacking != before.autoAttacking || now.swimming != before.swimming ||
           now.onFlightPath != before.onFlightPath || now.splineMoves != before.splineMoves;
}

}  // namespace rendering
}  // namespace wowee

#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::rendering::movement {

// The retail client rejects ground steeper than 50 degrees (0x0075d1c0 tests
// the face normal against 0.64278764) and steps up one yard. Keep these limits
// shared by terrain, WMO, and M2 collision paths so release builds cannot
// silently diverge by surface type.
//
// The step: when the move sweep (0x007620f0) runs into a face too steep to
// walk, 0x00761b00 sweeps the character up by the step height (0x006e9520),
// forward, and back down, and climbs only if it lands on walkable ground. The
// step height is the moving object's +0xd0, set by 0x006e9570 to the object's
// scale (at least 1) over its display scale - 1.0 for a player at normal size
// (a creature not moved by the player gets 2.0). It was taken as 0.6 here,
// from nowhere in the client, so a bed 0.93 high was neither a step nor - as a
// building's doodad - a wall, and the character walked into it.
inline constexpr float kMinWalkableNormalZ = 0.642787635f; // cos(50 degrees)
inline constexpr float kMaxStepUp = 1.0f;

/// How far below the feet a walk keeps to the ground. The client's ground
/// step (0x007620f0), its move done, sweeps the body straight down by
/// 1.849399 times the length of the move (the pyramid's slope; the call
/// with param_4 * 1.849399): a ramp or a stair going down is followed as far
/// as the move is long, and only ground further below than that is a fall
/// (0x00988370) - collision_walk.hpp.
inline constexpr float kStepDownPerMove = 1.849399f;

/// The client's fall (0x00986f00): gravity, and the terminal speed it is
/// held to, in yards and seconds.
inline constexpr float kFallGravity = 19.291105f;
inline constexpr float kFallTerminal = 60.148003f;

/// Where a teleport puts the feet: the floor at or just under the position
/// the server gave, never the ground the player stood on before.
///
/// The client takes a teleport's position as it is. MSG_MOVE_TELEPORT_ACK
/// (0x00732450 -> 0x0072d2d0) queues a move event 0x2c (0x006eca00) whose
/// handler (0x006ecf80) calls 0x0098ba90: the position, the facing, the fall's
/// start (+0x4c..+0x54) all become the new one and the fall time is zeroed. No
/// height of the old ground survives it; the next collision pass looks for the
/// floor from where the player now is.
///
/// Here the old ground did survive (lastGroundZ), and the floor probe reaches
/// up from it: teleported from the Goldshire inn's hall (55.99) into its
/// cellar (50.16), the probe started over the hall floor, found it, and put
/// the player back upstairs. A storey overhead is out of reach of this window,
/// and so is a heightfield lying over a cellar; only a floor the feet could
/// step onto from the server's height is taken.
inline constexpr float kTeleportFloorReachUp = kMaxStepUp + 0.5f;
/// How far under the server's height a floor is still taken as the landing.
/// Further down, the server's height is kept and the player falls from it.
///
/// The server's height is its own floor there (its vmaps), so a floor far
/// under it is one the server does not stand the player on: 16 yards took
/// Darnassus' terrain (1328.87) under the temple terrace (1341.39) while
/// the terrace was not loaded yet, and the player stood 12.5 yards down.
inline constexpr float kTeleportFloorReachDown = 2.0f;

inline std::optional<float> teleportLandingFloor(std::initializer_list<std::optional<float>> floors,
                                                 float serverZ) {
    std::optional<float> best;
    for (const std::optional<float>& h : floors) {
        if (!h || *h > serverZ + kTeleportFloorReachUp || *h < serverZ - kTeleportFloorReachDown) continue;
        if (!best || *h > *best) best = h;
    }
    return best;
}

/// Whether what the walls made of a step is a hold, or a throw.
///
/// The client sweeps the body along the step and stops it where it first
/// touches (0x0075f9d0), sliding what is left along the face: a wall only
/// ever takes motion away, so the body ends no farther from where it began
/// than the step was long. The push-out the walls here do instead can add
/// motion - each face it overlaps moves it out by up to a body's radius, and
/// faces packed close (a statue's, a railing's) hand it on from one to the
/// next - and in Silvermoon's Court of the Sun it carried a character eight
/// yards in a frame, past the gold railing at the platform's edge, where it
/// fell. A result farther from `from` than the step plus a body's width of
/// push-out is not the walls holding the body back: it stays where it was.
inline glm::vec3 holdWithinStep(const glm::vec3& from, const glm::vec3& to, const glm::vec3& held, float radius) {
    const glm::vec2 step(to.x - from.x, to.y - from.y);
    const glm::vec2 went(held.x - from.x, held.y - from.y);
    const float reach = glm::length(step) + 2.0f * radius;
    if (glm::dot(went, went) <= reach * reach) return held;
    return glm::vec3(from.x, from.y, held.z);
}

} // namespace wowee::rendering::movement

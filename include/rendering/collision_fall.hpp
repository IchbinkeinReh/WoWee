#pragma once

/// The client's fall (0x007618b0 -> 0x007612b0 -> 0x00760fc0), ported on
/// the sweep, and the probe the step-up makes with it (0x007619c0).
///
/// A fall is not integrated frame by frame. Its height is a function of the
/// time since it began (0x00986f00): from the vertical speed it started
/// with (+0xb8, down positive - a jump starts at -7.955547) under gravity
/// 19.291105 up to a terminal speed of 60.148003, or 7 for a slow fall.
/// Each step asks where that puts the feet now, from the height the fall
/// began at (+0x84), so no error builds up from step to step; the move to
/// there, with the horizontal part carried along at the speed and in the
/// direction the unit left the ground with (+0x8c, +0x70/+0x74), is swept.
/// A sweep that meets walkable ground lands; one that meets a ceiling while
/// still rising ends the rise there; anything else is slid along (0x0075e9c0)
/// and the time the hit took is worked back out of the fall's own curve
/// (0x0075eb00) before the rest of the step goes on from it.

#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/collision_walk.hpp"
#include "rendering/movement_limits.hpp"

namespace wowee::rendering::collision {

inline constexpr float kGravity = movement::kFallGravity;
inline constexpr float kTerminalSpeed = movement::kFallTerminal;
inline constexpr float kSafeFallSpeed = 7.0f;
/// 0x009883f0: a jump's starting speed, and with flag 0x200000.
inline constexpr float kJumpSpeed = -7.955547f;

/// 0x00986f00: how far a fall that began at `v0` (down positive) has gone
/// after `t` seconds.
float fallDistance(float t, bool safe, float v0);
/// 0x00988280: when that distance is `d`; on the way up (`ascending`) the
/// earlier of the two times.
float fallTimeFor(const Mover& m, float d, bool ascending);
/// 0x00986e80: how long a fall rises before it comes down.
float timeToApex(const Mover& m);

/// 0x00988370 with the speed it starts at (0 walking off an edge).
void startFallAt(Mover& m, float v0);
/// 0x00988490, as far as the movement is concerned.
void land(Mover& m);

struct FallResult {
    uint32_t ms = 0;        ///< the time the step took
    bool landed = false;
    bool ceiling = false;   ///< rose into something and stopped rising
};

/// 0x007618b0: fall for `ms` milliseconds, moving `hdist` yards along the
/// ground direction `dir` meanwhile.
FallResult fallStep(Mover& m, uint32_t ms, float hdist, const glm::vec2& dir, Sweeper& sweeper);

/// 0x007619c0: would a unit falling `height` from where it is (moving one
/// yard along its fall direction) come down past `start` by at least its
/// half-width, within 10 degrees of `dir`? Leaves the unit where the fall
/// put it; the caller puts it back.
bool fallsClearOfEdge(Mover& m, const glm::vec2& dir, const glm::vec3& start, float height,
                      Sweeper& sweeper);

}  // namespace wowee::rendering::collision

#pragma once

/// Swimming and flying as the client moves them (0x00760b40), and when it
/// swims at all (0x00730d10).
///
/// A unit swims when the liquid at its feet (collision_liquid.hpp, the
/// level 0x007a1bc0 keeps) stands more than three quarters of its height
/// over them, and stops when that falls a thirty-sixth of a yard short of
/// it, or there is no liquid, unless it flies. Starting clears flying and
/// stepping (0x00989660); stopping drops the unit from where it is unless
/// gravity is off for it (0x0098bff0 -> 0x00988370(0)).
///
/// Swimming and flying share one step: a plain sweep along the move in
/// three dimensions, sliding off what it meets with a thousandth of a yard
/// to spare - no step up, no keeping to the ground. A swimmer is also swept
/// against the liquid's surface turned upside down, with a body three
/// quarters of its height, so the surface is a ceiling three quarters of a
/// body above its feet: that is what holds it at the surface. Rising
/// (ascending) into that ceiling jumps it out (0x009883f0), at the
/// swimmer's jump speed.

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "rendering/collision_walk.hpp"

namespace wowee::rendering::collision {

/// 0x9e9ee4 and 0xa3f854: swim from three quarters of the height, stop a
/// thirty-sixth of a yard under that.
inline constexpr float kSwimDepthShare = kLiquidBodyShare;
inline constexpr float kSwimStopSlack = kMinLookAhead;
/// 0x009883f0: the jump speed out of water (0xc1118c48).
inline constexpr float kSwimJumpSpeed = -9.096748f;

/// 0x00730d10: a swimmer rising this close over the depth it swims from, or
/// less, is near enough to the surface to jump out of it.
inline constexpr float kSurfaceJumpReach = 1.0f / 3.0f;

/// 0x00986ea0: still rising in a jump.
bool risingInJump(const Mover& m);

/// 0x00730d10, for the active player before it asks whether the unit still
/// swims: one swimming and rising (+0x44 0x400000) whose depth (the liquid
/// level over its feet, 0 with none) is no more than kSurfaceJumpReach over
/// the depth it swims from jumps out of the water (0x0072eb80 -> event 10 ->
/// 0x009883f0(1)) - when it moves along (+0x44 & 0xf) and gravity holds it
/// (0x006e9ad0). Whatever is in front of it: the swim step's own jump out
/// (swimFlyStep) is only for a rise that meets the surface before anything
/// solid, and against a wall too high to climb the rise went on past the
/// depth swimming stops at, and swimming stopped and started again every
/// few frames.
bool surfaceJumpDue(const Mover& m, const std::optional<float>& liquidLevel, bool ascending, bool movingAlong,
                    bool gravityDisabled);

/// 0x009883f0(1) for a swimmer: the fall begun at kSwimJumpSpeed, swimming
/// cleared (0x00988370).
void startSwimJump(Mover& m);

/// 0x00730d10 with 0x00989660 / 0x0098bff0: start or stop swimming for the
/// liquid level at the feet (none: not in liquid). `canSwim` is the unit's
/// own say (0x004f53d0); `gravityDisabled` keeps a unit that stops from
/// falling. True when the state changed.
bool updateSwimming(Mover& m, const std::optional<float>& liquidLevel, bool canSwim, bool gravityDisabled);

/// 0x00760b40: the swim or flight step over `ms`, `dist` yards along the
/// unit direction `dir`. `liquid` sweeps against the liquid's surface
/// (facets facing down, gathered only while swimming); null for a flyer.
/// `ascending` is the unit rising (+0x44 0x400000). Answers whether the
/// unit jumped out of the liquid.
bool swimFlyStep(Mover& m, uint32_t ms, float dist, const glm::vec3& dir, Sweeper& solid,
                 Sweeper* liquid, bool gravityDisabled, bool ascending);

/// The liquid's surface as the swimmer's ceiling (0x00783910 with mask
/// 0x20000, each facet then turned over): a facet facing up made to face
/// down.
inline Facet underside(const Facet& f) {
    Facet out = f;
    out.n = -f.n;
    out.d = -f.d;
    return out;
}

}  // namespace wowee::rendering::collision

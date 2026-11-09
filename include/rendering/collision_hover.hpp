#pragma once

/// Hovering as the client moves it (0x00762980): a unit flagged to hover
/// (+0x44 0x40000000) is held its hover height (+0xb4) over the ground.
///
/// Each step looks down twice the hover height with the pyramid's faces
/// alone (0x0075f520). Nothing there and the unit falls (0x0098b710 ->
/// 0x00988370(0), unless it swims or flies) and the step is a fall's
/// (0x007618b0). Otherwise the move's height is pulled toward the hover
/// height at up to 3.5 yards a second, and the whole move is swept and slid:
/// off walkable ground along its normal with a thousandth of a yard to
/// spare, along anything steeper as a walk slides (0x0075e250).

#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/collision_walk.hpp"

namespace wowee::rendering::collision {

/// 0x00762980: how fast the hover height is made up, yards a second.
inline constexpr float kHoverCatchUp = 3.5f;

struct HoverProbe {
    bool found = false;    ///< ground within twice the hover height
    float height = 0.0f;   ///< how far over it the feet are (0 within 1/720)
};

/// 0x0075f520: the ground under a hovering unit.
HoverProbe hoverProbe(const Mover& m, Sweeper& sweeper);

/// 0x00762980: the hover step over `ms`, `dist` yards along the unit
/// direction `dir`. False when there was no ground and the unit started to
/// fall instead (the caller makes the fall's step).
bool hoverStep(Mover& m, uint32_t ms, float dist, const glm::vec3& dir, Sweeper& sweeper,
               bool gravityDisabled);

}  // namespace wowee::rendering::collision

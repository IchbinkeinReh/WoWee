#pragma once

// Jumping out of the water (0x00730d30 -> 0x0072eb80 -> 0x009883f0).
//
// Space while swimming is the swim-up key (JumpOrAscendStart, 0x005fbf80,
// holds ascend and jumps only on land). The jump out of the water is the
// water update's: each frame the active player swims with ascend held and
// is near the surface - its feet under the water by less than a third of a
// yard more than where a swimmer floats (the depth that starts a swim,
// 0.75 x its height) - the client starts a jump (movement event 10,
// MSG_MOVE_JUMP), moving or not: 0x0072eb80 tests the direction keys only for
// a unit with +0x9c0 set, and a player's is not. The fall it starts
// (0x00988370) clears swimming, so the body leaves the water at the
// swimmer's jump speed, 9.096718 yards a second up (0xc1118c48), faster than
// the 7.955547 of a jump from the ground: onto a ledge or a jetty, or back
// in with a splash.

namespace wowee::rendering::swim_jump {

inline constexpr float kVerticalSpeed = 9.096718f;
inline constexpr float kSurfaceBand = 1.0f / 3.0f;

/// Whether a swimmer jumps out this frame: the swim-up key held, and its feet
/// `depthFromFeet` under the surface - no more than `floatDepth` (where a
/// swimmer floats) plus a third of a yard.
[[nodiscard]] inline bool allowed(bool swimUpHeld, float depthFromFeet, float floatDepth) {
    return swimUpHeld && depthFromFeet - floatDepth < kSurfaceBand;
}

}  // namespace wowee::rendering::swim_jump

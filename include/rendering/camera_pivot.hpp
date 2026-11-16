#pragma once

// Where the third-person camera pivots on the unit it follows: the camera's
// target heights (+0x14c / +0x150 / +0x154), worked out by 0x00604e00 when
// it takes a unit to look at and kept between [0.8333, the unit's ceiling].
//
//  - A unit with a model: its Breath attachment (17, looked up through the
//    model's attachment lookup, 0x00827460) at its bind height, plus 3.5/36,
//    at the unit's scale (object scale x the display's model scale). Without
//    one, nine tenths of the model's collision box height (the M2 header's
//    +0xbc..+0xd0 box) at that scale.
//  - The ceiling (0x00718080): the unit's collision height (+0x854: the
//    display's CreatureModelData CollisionHeight at its scale) less 1/6; a
//    unit that is no player at most 73/36 less 1/6; 15 where that comes to
//    nothing.
//
// A shapeshift form's display is the unit's display, so Metamorphosis'
// demon (DemonForm.m2 at the display's 0.5) is looked at by its own Breath
// attachment and kept below its own collision height - not by the head of
// whatever model it was before.

#include <optional>

namespace wowee::rendering::camera_pivot {

/// 3.5/36: what 0x00604e00 adds over the Breath attachment.
inline constexpr float kBreathLift = 0.097222224f;
/// The lowest pivot (0x00604e00's clamp).
inline constexpr float kMinPivot = 0.8333333f;
/// The default collision height (73/36) a unit that is no player is held to.
inline constexpr float kUnitCeilingHeight = 2.0277777f;
/// What the ceiling stays below the collision height by.
inline constexpr float kBelowCollisionTop = 0.16666667f;
/// The ceiling where the unit has no height to give one.
inline constexpr float kNoCeiling = 15.0f;
/// M2 attachment id 17, Breath.
inline constexpr unsigned kBreathAttachment = 17;

/// The pivot the model gives, above its feet, before the clamp.
[[nodiscard]] inline float modelPivot(std::optional<float> breathZ, float boxMinZ, float boxMaxZ, float scale) {
    if (breathZ) return (*breathZ + kBreathLift) * scale;
    return (boxMaxZ - boxMinZ) * scale * 0.9f;
}

/// 0x00718080: the highest pivot for a unit of that collision height.
[[nodiscard]] inline float pivotCeiling(float collisionHeight, bool isPlayer) {
    if (!isPlayer && collisionHeight > kUnitCeilingHeight) return kUnitCeilingHeight - kBelowCollisionTop;
    return collisionHeight - kBelowCollisionTop;
}

/// 0x00604e00's clamp: below the floor the floor, at or past the ceiling
/// the ceiling (a ceiling of nothing reads as 15).
[[nodiscard]] inline float clampPivot(float pivot, float ceiling) {
    if (ceiling <= 0.0f) ceiling = kNoCeiling;
    if (pivot < kMinPivot) return kMinPivot;
    if (pivot >= ceiling) return ceiling;
    return pivot;
}

}  // namespace wowee::rendering::camera_pivot

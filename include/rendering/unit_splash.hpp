#pragma once

// A unit's splash as it wades into or out of water.
//
// Each update (0x0073ab20 -> 0x00730d10), a unit not swimming has its depth
// below the water's surface (the liquid level its collision finds less its
// feet, 0 with none) weighed against four tenths of its collision height
// (CMovement +0xcc, unit +0x854); where the depth crosses it either way
// since the last update, the unit plays its race's SplashSoundID
// (ChrRaces, kept at +0x8f0) - type 8, two at once - through 0x00746720:
// the unit the client moves, with the listener at the character, where the
// listener is and at 0.65; any other where the unit is. The depth is kept
// only while not swimming, from 0 as the unit is made.
//
// The collision height is the display's CreatureModelData CollisionHeight
// times its scale for a player, and for a creature whose cache sets type
// flag 0x20000; any other creature's is 73/36 (0x006ebd30, 0x00725f50,
// 0x006d78c0). The scale is the object scale, at least 1, times the
// display's CreatureModelScale and the model's ModelScale, 1 where that
// comes to nothing (0x006cf350, 0x0071c110).

#include <algorithm>
#include <cstdint>

namespace wowee::rendering::unit_splash {

constexpr float kDefaultCollisionHeight = 73.0f / 36.0f;
constexpr float kSplashDepthFraction = 0.4f;
/// The creature cache's type flag that sizes it by its model.
constexpr uint32_t kTypeFlagModelCollision = 0x20000u;

/// ChrRaces' SplashSoundID column.
constexpr uint32_t kChrRacesSplashSoundColumn = 10;
/// CreatureDisplayInfo CreatureModelScale; CreatureModelData ModelScale and
/// CollisionHeight.
constexpr uint32_t kDisplayScaleColumn = 4;
constexpr uint32_t kModelScaleColumn = 4;
constexpr uint32_t kModelCollisionHeightColumn = 15;

inline float collisionHeight(float modelHeight, float objectScale, float displayScale, float modelScale) {
    float shape = displayScale * modelScale;
    if (!(shape > 0.0f)) shape = 1.0f;
    return modelHeight * std::max(objectScale, 1.0f) * shape;
}

/// Whether the depth crossed four tenths of the height since the last.
constexpr bool crossed(float height, float depth, float lastDepth) {
    const float mark = height * kSplashDepthFraction;
    return (mark < depth) != (mark < lastDepth);
}

}  // namespace wowee::rendering::unit_splash

#pragma once

/// Casting at a place on the ground, as the client does it (Spell_C.cpp,
/// WorldFrame.cpp). A spell whose targets want a location leaves the cursor
/// holding it; the ground under the cursor is shown with a circle, and a click
/// on the world casts it there. Arithmetic only: the state is the spell
/// handler's, the circle the renderer's.

#include <algorithm>
#include <cstdint>

namespace wowee::game::ground_target {

/// SpellCastTargets flags (0x009ab8b0 writes them).
inline constexpr uint32_t kTargetFlagUnit = 0x2;
inline constexpr uint32_t kTargetFlagSourceLocation = 0x20;
inline constexpr uint32_t kTargetFlagDestLocation = 0x40;

/// 0x0080c790: the targets a spell still needs - Spell.dbc Targets with the
/// first effect's ImplicitTargetA adding or removing bits. Of those, only the
/// two locations matter here: TARGET_DEST_TRAJ (89) with a missile whose
/// SpellMissile row is flagged 1 drops the destination (the trajectory picks
/// it instead).
constexpr uint32_t requiredTargets(uint32_t targets, uint32_t implicitTargetA0, bool trajectoryMissile) {
    uint32_t required = targets;
    if (implicitTargetA0 == 89 && trajectoryMissile) required &= ~kTargetFlagDestLocation;
    return required;
}

/// 0x007fd750: the cursor waits for a location while either is still wanted.
constexpr bool wantsLocation(uint32_t required) {
    return (required & (kTargetFlagSourceLocation | kTargetFlagDestLocation)) != 0;
}

/// 0x0080c340: which location a click fills - the source first when both are
/// wanted, then the destination.
constexpr uint32_t locationFilledByClick(uint32_t required) {
    if (required & kTargetFlagSourceLocation) return kTargetFlagSourceLocation;
    if (required & kTargetFlagDestLocation) return kTargetFlagDestLocation;
    return 0;
}

/// 0x008019c0: the spell's area - the larger of its first two effects'
/// radius plus the caster's level times the radius per level.
constexpr float spellRadius(const float radius[2], const float perLevel[2], uint32_t level) {
    const float a = radius[0] + static_cast<float>(level) * perLevel[0];
    const float b = radius[1] + static_cast<float>(level) * perLevel[1];
    return std::max(a, b);
}

/// 0x00803ee0: what the cursor's place is to the spell - inside its range
/// (min squared to max squared, measured from the caster in three
/// dimensions), past it, or past four times it, where the client draws no
/// circle at all.
enum class Placement { Acceptable = 0, Unacceptable = 1, TooFar = 2 };
constexpr Placement placement(float distSq, float minRange, float maxRange) {
    if (minRange * minRange <= distSq && distSq <= maxRange * maxRange) return Placement::Acceptable;
    if (maxRange * maxRange * 16.0f < distSq) return Placement::TooFar;
    return Placement::Unacceptable;
}

/// 0x004f66c0 and 0x004f8a40: the circle's radius - the spell's area held to
/// 20 where the place is acceptable, none otherwise; and none drawn as
/// 1.3888888 (a circle the size of the texture's own ring).
inline constexpr float kMaxCircleRadius = 20.0f;
inline constexpr float kEmptyCircleRadius = 1.3888888f;
constexpr float circleRadius(Placement p, float spellArea) {
    const float r = p == Placement::Acceptable ? std::min(spellArea, kMaxCircleRadius) : 0.0f;
    return r == 0.0f ? kEmptyCircleRadius : r;
}

/// 0x004f8a40: the box the circle is gathered from is the radius either way
/// across the ground and two yards either way up and down.
inline constexpr float kCircleHalfHeight = 2.0f;

/// Which art the circle is drawn with (0x00ac799c): Spell-Shadow-Acceptable
/// for 0, Spell-Shadow-Unacceptable for 1; nothing for TooFar. The cursor is
/// Interface\Cursor\Cast (2 in 0x00ad2808) or UnableCast (0x1c).
inline constexpr const char* kAcceptableTexture = "Interface\\SpellShadow\\Spell-Shadow-Acceptable.blp";
inline constexpr const char* kUnacceptableTexture = "Interface\\SpellShadow\\Spell-Shadow-Unacceptable.blp";
inline constexpr const char* kCastCursor = "Interface\\Cursor\\Cast.blp";
inline constexpr const char* kUnableCastCursor = "Interface\\Cursor\\UnableCast.blp";
constexpr const char* cursorFor(Placement p) {
    return p == Placement::Acceptable ? kCastCursor : kUnableCastCursor;
}

}  // namespace wowee::game::ground_target

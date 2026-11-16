#pragma once

// A ranged spell's visual completed from the caster's ranged weapon
// (0x00720f80, the unit's override of vfunc +0xbc).

#include <array>
#include <cstdint>

namespace wowee::rendering::spell_visual_merge {

/// One SpellVisual.dbc row, its 32 columns as 3.3.5a stores them.
using Row = std::array<int32_t, 32>;

// The columns 0x00720f80 fills, by index.
inline constexpr int kPrecastKit = 1;
inline constexpr int kCastKit = 2;
inline constexpr int kImpactKit = 3;
inline constexpr int kHasMissile = 7;
inline constexpr int kMissileModel = 8;
inline constexpr int kMissileDestinationAttachment = 10;
inline constexpr int kMissileSound = 11;
inline constexpr int kAnimEventSound = 12;
inline constexpr int kCasterImpactKit = 14;
inline constexpr int kTargetImpactKit = 15;
inline constexpr int kMissileAttachment = 16;
inline constexpr int kMissileFollowGroundHeight = 17;
inline constexpr int kMissileMotion = 21;

/// 0x00720f80: for a spell with Attributes 0x2, every one of these columns
/// the spell's own visual leaves 0 is taken from the visual of the ranged
/// weapon's ItemDisplayInfo SpellVisualID - the five kits, the missile
/// (HasMissile set to 1 with the weapon's model, when the spell has none),
/// both attachments, the missile's ground following and motion, and both
/// sounds. The rest - flags, state, channel and area kits, offsets - stay
/// the spell's. With no visual of its own the spell starts from an empty row.
[[nodiscard]] inline Row fillFromWeapon(const Row* spell, const Row& weapon) {
    Row out{};
    if (spell) out = *spell;
    for (const int column : {kPrecastKit, kCastKit, kImpactKit, kCasterImpactKit, kTargetImpactKit})
        if (out[column] == 0) out[column] = weapon[column];
    if (out[kHasMissile] == 0 && weapon[kHasMissile] != 0) {
        out[kHasMissile] = 1;
        out[kMissileModel] = weapon[kMissileModel];
    }
    for (int column = kMissileAttachment; column <= kMissileMotion; ++column)
        if (out[column] == 0) out[column] = weapon[column];
    for (const int column : {kMissileDestinationAttachment, kMissileSound, kAnimEventSound})
        if (out[column] == 0) out[column] = weapon[column];
    return out;
}

}  // namespace wowee::rendering::spell_visual_merge

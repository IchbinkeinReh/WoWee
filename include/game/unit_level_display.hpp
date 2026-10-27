#pragma once

/// What UnitLevel answers for a unit (0x0060f9e0): its level, or -1 - the
/// target frame's skull - for a unit hostile to the player and ten or more
/// levels above them, and for a boss (its creature template's type flag 0x4,
/// CREATURE_TYPE_FLAG_BOSS, as 0x00715d70 reads it from the creature cache)
/// at any level and any reaction.
///
/// "Hostile" is how the unit regards the player (0x007251c0 asked by the
/// unit, 0 hated to 7 exalted): 1 or less. A friendly or neutral NPC far
/// above the player shows its level; an elite or rare does too unless it is
/// a boss. Pure: tests/test_unit_level_display.cpp checks it.
///
/// And a drunk player sees an enemy as lower than it is: with PLAYER_BYTES_3's
/// drunk byte (+0x1d) or PLAYER_FAKE_INEBRIATION (+0x2b8) set, a unit the
/// player may attack (0x00729740, the player attacking) shows its level less
/// round(share x 5 + 0.5), never below 1 - the share being how drunk, out of
/// 100 (0x004f7290), and the rounding the x87's, to the nearest even.

#include <cmath>

namespace wowee::game {

/// The reaction (0 hated .. 7 exalted) at or below which a unit counts as hostile.
inline constexpr int kUnitLevelHostileReaction = 1;
/// How far above the player a hostile unit's level becomes a skull.
inline constexpr int kUnitLevelSkullGap = 10;
/// CREATURE_TYPE_FLAG_BOSS in the creature template's type flags.
inline constexpr unsigned kCreatureTypeFlagBoss = 0x4u;

/// 0x004f7290: how drunk the active player is, as a share of one - the larger
/// of the drunk byte and the fake inebriation, held to 100, over 100.
inline float playerDrunkShare(int drunk, int fakeInebriation) {
    const int most = drunk <= fakeInebriation ? fakeInebriation : drunk;
    if (most >= 100) return 1.0f;
    return static_cast<float>(static_cast<long double>(most) * 0.01L);
}

/// The levels a drunk player's view takes off an enemy: the share times five
/// (0x0060f9e0, stored as a float), then 0x0048bcf0 - add a half and round to
/// the nearest, ties to even, as the x87 does. Zero sober, six fully drunk.
inline int drunkLevelReduction(int drunk, int fakeInebriation) {
    const float scaled = static_cast<float>(static_cast<double>(playerDrunkShare(drunk, fakeInebriation)) * 5.0);
    return static_cast<int>(std::nearbyint(static_cast<double>(scaled) + 0.5));
}

/// `havePlayer` false: no active player (its level, the reaction and the
/// drunk state unused). `drunk` is PLAYER_BYTES_3's byte 1, `fakeInebriation`
/// PLAYER_FAKE_INEBRIATION, and `playerMayAttack` whether the player may
/// attack the unit.
inline int displayedUnitLevel(int unitLevel, bool havePlayer, int playerLevel, int reactionToPlayer,
                              unsigned creatureTypeFlags, int drunk = 0, int fakeInebriation = 0,
                              bool playerMayAttack = false) {
    if (havePlayer && reactionToPlayer <= kUnitLevelHostileReaction &&
        playerLevel <= unitLevel - kUnitLevelSkullGap) {
        return -1;
    }
    if ((creatureTypeFlags & kCreatureTypeFlagBoss) != 0u) return -1;
    int level = unitLevel;
    if (level > 0 && havePlayer && (drunk != 0 || fakeInebriation != 0) && playerMayAttack) {
        level -= drunkLevelReduction(drunk, fakeInebriation);
        if (level < 1) level = 1;
    }
    return level;
}

}  // namespace wowee::game

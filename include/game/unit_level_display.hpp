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
/// Not here: the client then lowers the level shown of a unit the player may
/// attack by up to five, by a share it keeps in the active player's data
/// (+0x1d / +0x2b8, through 0x004f7290); what sets that share is not known.

namespace wowee::game {

/// The reaction (0 hated .. 7 exalted) at or below which a unit counts as hostile.
inline constexpr int kUnitLevelHostileReaction = 1;
/// How far above the player a hostile unit's level becomes a skull.
inline constexpr int kUnitLevelSkullGap = 10;
/// CREATURE_TYPE_FLAG_BOSS in the creature template's type flags.
inline constexpr unsigned kCreatureTypeFlagBoss = 0x4u;

/// `havePlayer` false: no active player (its level and the reaction unused).
inline int displayedUnitLevel(int unitLevel, bool havePlayer, int playerLevel, int reactionToPlayer,
                              unsigned creatureTypeFlags) {
    if (havePlayer && reactionToPlayer <= kUnitLevelHostileReaction &&
        playerLevel <= unitLevel - kUnitLevelSkullGap) {
        return -1;
    }
    if ((creatureTypeFlags & kCreatureTypeFlagBoss) != 0u) return -1;
    return unitLevel;
}

}  // namespace wowee::game

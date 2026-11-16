#pragma once

// The spellbook's tabs, as the client files the spells it learns
// (0x00542030) and orders the tabs (0x0053ba40).
//
// A spell goes under its skill line - the first SkillLineAbility row for it
// that the player's race and class may have (0x0071a670 -> 0x00812410) -
// where SkillRaceClassInfo has a row for that line, race and class
// (0x00810ed0) without flag 0x80; anything else goes under General (skill
// line 0). Flag 0x80 is on every profession, every secondary skill and the
// riding lines, so a warlock's book is General and its three trees, and a
// miner's smelting sits in General - no category of SkillLine.dbc decides
// it. The tabs then stand General first and the rest by their SkillLine
// name, without case (0x0076ec80). General is named GENERAL_SPELLS with
// the Ability_Kick icon (GetSpellTabInfo, 0x0053be70).

#include <cstdint>
#include <optional>
#include <string>

#include "game/client_compare.hpp"

namespace wowee::game::spell_book_tabs {

/// SkillRaceClassInfo flag 0x80: the line's spells go under General.
inline constexpr uint32_t kRaceClassFlagGeneral = 0x80u;
/// General's icon (0x0053be70).
inline constexpr const char* kGeneralIcon = "Interface\\Icons\\Ability_Kick";

/// The tab a spell goes under: its skill line, or 0 for General.
/// `raceClassFlags` are the flags of the line's SkillRaceClassInfo row for
/// the player's race and class, none where it has no row.
[[nodiscard]] inline uint32_t tabSkillLine(uint32_t skillLine, std::optional<uint32_t> raceClassFlags) {
    if (skillLine == 0 || !raceClassFlags) return 0;
    return (*raceClassFlags & kRaceClassFlagGeneral) != 0 ? 0u : skillLine;
}

/// 0x0053ba40: whether tab a stands before tab b - General (0) first, the
/// rest by name without case.
[[nodiscard]] inline bool tabBefore(uint32_t lineA, const std::string& nameA, uint32_t lineB,
                                    const std::string& nameB) {
    if (lineA == 0) return lineB != 0;
    if (lineB == 0) return false;
    return compareCaseless(nameA, nameB) < 0;
}

}  // namespace wowee::game::spell_book_tabs

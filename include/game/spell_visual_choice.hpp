#pragma once

/// Which of a spell's two SpellVisuals the client shows (0x007fa290): the
/// second (Spell.dbc +0x210) where it has one and violenceLevel is under 2,
/// else the first (+0x20c). Every visual the client takes for a spell -
/// cast, missile, impact, aura, dynamic object - comes through there.
///
/// violenceLevel (0x007f3a10, "Sets the violence level of the game") is kept
/// by 0x007f39b0: its number, where that is below 0 or above the locale's
/// limit, the limit - 2, but 1 for koKR and zhCN (0x00af4e14 by the locale's
/// place in 0x00ad2fe0). Its default is the limit. It is not
/// spellEffectLevel ("Spell Detail", default 9), which only scales a kit's
/// falling models (0x007fb5b0); 3.3.5a's options panels expose neither.

#include <cstdint>
#include <cstdlib>
#include <string>

namespace wowee::game::spell_visual {

/// 0x00af4e14: the locales' limit - all of this client's (enUS, deDE, ...).
inline constexpr int kViolenceLevelLimit = 2;

/// 0x007f39b0: the cvar's text as the level it keeps.
inline int violenceLevel(const std::string& text) {
    const int level = std::atoi(text.c_str());
    return level < 0 || level > kViolenceLevelLimit ? kViolenceLevelLimit : level;
}

/// 0x007fa290: the SpellVisual a spell shows.
constexpr uint32_t choose(uint32_t first, uint32_t second, int violenceLevel) {
    return violenceLevel < 2 && second != 0 ? second : first;
}

}  // namespace wowee::game::spell_visual

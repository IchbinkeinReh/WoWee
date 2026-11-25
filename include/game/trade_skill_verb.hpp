#pragma once

#include <cstdint>

namespace wowee::game {

/// Whether a recipe goes by its skill's own verb, SkillLine.dbc's "Enchant"
/// (GetTradeSkillInfo, 0x005db2a0): one whose first effect makes no item -
/// neither CREATE_ITEM (24) nor CREATE_RANDOM_ITEM (59) nor CREATE_ITEM_2 (157).
[[nodiscard]] constexpr bool recipeHasSkillVerb(uint32_t firstEffect) {
    return firstEffect != 24 && firstEffect != 59 && firstEffect != 157;
}

}  // namespace wowee::game

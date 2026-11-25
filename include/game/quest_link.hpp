#pragma once

#include "game/grey_triviality.hpp"

#include <cstdint>
#include <string>

namespace wowee::game {

/// The colour of a quest of a level to a player of one (0x0061e540): red more
/// than four above, orange more than two, yellow down to two below, green down
/// to the green range (0x005e2860), grey below that.
[[nodiscard]] inline const char* questColour(int questLevel, int playerLevel) {
    const int above = questLevel - playerLevel;
    if (above > 4) return "|cffff2020";
    if (above > 2) return "|cffff8040";
    if (above > -3) return "|cffffff00";
    return -above > triviality::greyGap(static_cast<uint32_t>(playerLevel)) ? "|cff808080" : "|cff40c040";
}

/// A quest's chat link (0x005e51d0, 0x0061e5c0): its level as the template has
/// it, -1 for one of the player's, coloured as that.
[[nodiscard]] inline std::string questChatLink(uint32_t questId, int questLevel, const std::string& title,
                                               int playerLevel) {
    return std::string(questColour(questLevel == -1 ? playerLevel : questLevel, playerLevel)) + "|Hquest:" +
           std::to_string(questId) + ":" + std::to_string(questLevel) + "|h[" + title + "]|h|r";
}

}  // namespace wowee::game

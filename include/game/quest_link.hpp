#pragma once

#include "game/grey_triviality.hpp"

#include <cstdint>
#include <cstdlib>
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

/// 0x00509dd0 on an incoming chat line: every quest link not escaped ('||')
/// gets the RECEIVER's colour for its level (-1 = the player's own) written
/// over the ten characters before '|Hquest:' - the sender's colour code. A
/// link without its two ':' or with no room for the code is left alone.
[[nodiscard]] inline std::string recolourQuestLinks(std::string text, int playerLevel) {
    static const std::string kTag = "|Hquest:";
    for (size_t at = text.find(kTag); at != std::string::npos; at = text.find(kTag, at + kTag.size())) {
        if (at == 0 || text[at - 1] == '|' || at < 10) continue;
        const size_t c1 = text.find(':', at);
        const size_t c2 = c1 == std::string::npos ? c1 : text.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;
        int level = std::atoi(text.c_str() + c2 + 1);
        if (level == -1) level = playerLevel;
        text.replace(at - 10, 10, questColour(level, playerLevel));
    }
    return text;
}

}  // namespace wowee::game

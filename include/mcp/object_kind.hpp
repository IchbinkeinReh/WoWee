#pragma once

// GAMEOBJECT_TYPE_ID by its name, as the tools say it.

#include <array>
#include <cstddef>
#include <cstdint>

namespace wowee::mcp {

constexpr const char* objectKind(uint32_t type) {
    constexpr std::array<const char*, 37> kNames = {
        "door", "button", "questgiver", "chest", "binder", "generic", "trap", "chair", "spell_focus", "text",
        "goober", "transport", "area_damage", "camera", "map_object", "mo_transport", "duel_arbiter", "fishing_bobber",
        "ritual", "mailbox", "auction_house", "guard_post", "spellcaster", "meeting_stone", "flag_stand",
        "fishing_hole", "flag_drop", "mini_game", "lottery", "capture_point", "aura_generator", "dungeon_difficulty",
        "barber_chair", "destructible", "guild_bank", "trapdoor", "?"};
    return kNames[type < kNames.size() ? type : kNames.size() - 1];
}

}  // namespace wowee::mcp

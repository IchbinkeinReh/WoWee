#pragma once

/// An item's stat and resistance lines, worded as the client's item tooltip
/// (0x006277f0) words them from the interface's GlobalStrings: the primary
/// stats in white as ITEM_MOD_<STAT> ("%c%d Stamina" - "+18 Ausdauer"), the
/// ratings and powers in green as ITEM_SPELL_TRIGGER_ONEQUIP followed by
/// ITEM_MOD_<STAT> ("Equip: Increases your hit rating by %d." - "Anlegen:
/// Erhöht Trefferwertung um 20."), and the resistances as ITEM_RESIST_SINGLE
/// with SPELL_SCHOOL<n>_CAP, or ITEM_RESIST_ALL when all five are the same.
///
/// The order is the client's, not the item's: two fixed tables
/// (0x00a262f0 for the white ones, 0x00a25f78 for the green) walked in turn,
/// each taking the first of the item's stats of that type.
///
/// Pure: the text source is an argument (game::interfaceString in the game);
/// tests/test_item_stat_text.cpp checks it.

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "game/chat_channels.hpp"

namespace wowee::game {

using ItemTextSource = std::function<std::string(const char* key, const char* enUS)>;

/// The ITEM_MOD key for a stat type (the client's table at 0x00ad6640) and
/// its enUS wording, for a client with no interface. Null for a type the
/// client names nothing for.
struct ItemModKey {
    const char* key = nullptr;
    const char* enUS = nullptr;
};

inline ItemModKey itemModKey(uint32_t type) {
    switch (type) {
        case 0:  return {"ITEM_MOD_MANA", "%c%d Mana"};
        case 1:  return {"ITEM_MOD_HEALTH", "%c%d Health"};
        case 3:  return {"ITEM_MOD_AGILITY", "%c%d Agility"};
        case 4:  return {"ITEM_MOD_STRENGTH", "%c%d Strength"};
        case 5:  return {"ITEM_MOD_INTELLECT", "%c%d Intellect"};
        case 6:  return {"ITEM_MOD_SPIRIT", "%c%d Spirit"};
        case 7:  return {"ITEM_MOD_STAMINA", "%c%d Stamina"};
        case 12: return {"ITEM_MOD_DEFENSE_SKILL_RATING", "Increases defense rating by %d."};
        case 13: return {"ITEM_MOD_DODGE_RATING", "Increases your dodge rating by %d."};
        case 14: return {"ITEM_MOD_PARRY_RATING", "Increases your parry rating by %d."};
        case 15: return {"ITEM_MOD_BLOCK_RATING", "Increases your shield block rating by %d."};
        case 16: return {"ITEM_MOD_HIT_MELEE_RATING", "Improves melee hit rating by %d."};
        case 17: return {"ITEM_MOD_HIT_RANGED_RATING", "Improves ranged hit rating by %d."};
        case 18: return {"ITEM_MOD_HIT_SPELL_RATING", "Improves spell hit rating by %d."};
        case 19: return {"ITEM_MOD_CRIT_MELEE_RATING", "Improves melee critical strike rating by %d."};
        case 20: return {"ITEM_MOD_CRIT_RANGED_RATING", "Improves ranged critical strike rating by %d."};
        case 21: return {"ITEM_MOD_CRIT_SPELL_RATING", "Improves spell critical strike rating by %d."};
        case 22: return {"ITEM_MOD_HIT_TAKEN_MELEE_RATING", "Improves melee hit avoidance rating by %d."};
        case 23: return {"ITEM_MOD_HIT_TAKEN_RANGED_RATING", "Improves ranged hit avoidance rating by %d."};
        case 24: return {"ITEM_MOD_HIT_TAKEN_SPELL_RATING", "Improves spell hit avoidance rating by %d."};
        case 25: return {"ITEM_MOD_CRIT_TAKEN_MELEE_RATING", "Improves melee critical avoidance rating by %d."};
        case 26: return {"ITEM_MOD_CRIT_TAKEN_RANGED_RATING", "Improves ranged critical avoidance rating by %d."};
        case 27: return {"ITEM_MOD_CRIT_TAKEN_SPELL_RATING", "Improves spell critical avoidance rating by %d."};
        case 28: return {"ITEM_MOD_HASTE_MELEE_RATING", "Improves melee haste rating by %d."};
        case 29: return {"ITEM_MOD_HASTE_RANGED_RATING", "Improves ranged haste rating by %d."};
        case 30: return {"ITEM_MOD_HASTE_SPELL_RATING", "Improves spell haste rating by %d."};
        case 31: return {"ITEM_MOD_HIT_RATING", "Improves hit rating by %d."};
        case 32: return {"ITEM_MOD_CRIT_RATING", "Improves critical strike rating by %d."};
        case 33: return {"ITEM_MOD_HIT_TAKEN_RATING", "Improves hit avoidance rating by %d."};
        case 34: return {"ITEM_MOD_CRIT_TAKEN_RATING", "Improves critical avoidance rating by %d."};
        case 35: return {"ITEM_MOD_RESILIENCE_RATING", "Improves your resilience rating by %d."};
        case 36: return {"ITEM_MOD_HASTE_RATING", "Improves haste rating by %d."};
        case 37: return {"ITEM_MOD_EXPERTISE_RATING", "Increases your expertise rating by %d."};
        case 38: return {"ITEM_MOD_ATTACK_POWER", "Increases attack power by %d."};
        case 39: return {"ITEM_MOD_RANGED_ATTACK_POWER", "Increases ranged attack power by %d."};
        case 41: return {"ITEM_MOD_SPELL_HEALING_DONE", "Increases healing done by magical spells and effects by up to %d."};
        case 42: return {"ITEM_MOD_SPELL_DAMAGE_DONE", "Increases damage done by magical spells and effects by up to %d."};
        case 43: return {"ITEM_MOD_MANA_REGENERATION", "Restores %d mana per 5 sec."};
        case 44: return {"ITEM_MOD_ARMOR_PENETRATION_RATING", "Increases your armor penetration rating by %d."};
        case 45: return {"ITEM_MOD_SPELL_POWER", "Increases spell power by %d."};
        case 46: return {"ITEM_MOD_HEALTH_REGEN", "Restores %d health per 5 sec."};
        case 47: return {"ITEM_MOD_SPELL_PENETRATION", "Increases spell penetration by %d."};
        case 48: return {"ITEM_MOD_BLOCK_VALUE", "Increases the block value of your shield by %d."};
        default: return {};
    }
}

/// The white stats, in the client's order (0x00a262f0).
inline constexpr uint32_t kItemWhiteStatOrder[] = {4, 3, 7, 5, 6, 1, 0, 8, 9, 2, 10};
/// The green ones (0x00a25f78): the ratings and powers, 12 to 48 with the
/// spell damage before the healing.
inline constexpr uint32_t kItemEquipStatOrder[] = {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                                                   25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
                                                   38, 39, 40, 42, 41, 43, 44, 45, 46, 47, 48};

struct ItemStatLine {
    std::string text;
    bool equip = false;  ///< green, "Equip: ..."; white otherwise
};

/// The sign as the client passes it to %c: '-' for a value below one.
inline std::string itemStatSign(int32_t value) { return value < 1 ? "-" : "+"; }

/// One stat's line, white or green by which table holds the type. Empty text
/// where the interface has no wording for it.
inline ItemStatLine itemStatLine(uint32_t type, int32_t value, bool equip, const ItemTextSource& text) {
    ItemStatLine line;
    line.equip = equip;
    const ItemModKey k = itemModKey(type);
    if (!k.key) return line;
    const std::string fmt = text(k.key, k.enUS);
    if (fmt.empty()) return line;
    const std::string magnitude = std::to_string(std::abs(value));
    const bool signed_ = fmt.find("%c") != std::string::npos || fmt.find("$c") != std::string::npos;
    const std::string body = signed_ ? chat_channels::luaFormat(fmt, {itemStatSign(value), magnitude})
                                     : chat_channels::luaFormat(fmt, {std::to_string(value)});
    line.text = equip ? text("ITEM_SPELL_TRIGGER_ONEQUIP", "Equip:") + " " + body : body;
    return line;
}

/// Every stat line of an item whose stats are `stats` (type, value), white
/// ones first, each table in its own order and taking the first stat of each
/// type; a stat of zero is left out.
inline std::vector<ItemStatLine> itemStatLines(const std::vector<std::pair<uint32_t, int32_t>>& stats,
                                               const ItemTextSource& text) {
    std::vector<ItemStatLine> out;
    auto walk = [&](const uint32_t* order, size_t n, bool equip) {
        for (size_t i = 0; i < n; ++i) {
            for (const auto& [type, value] : stats) {
                if (type != order[i] || value == 0) continue;
                ItemStatLine line = itemStatLine(type, value, equip, text);
                if (!line.text.empty()) out.push_back(std::move(line));
                break;
            }
        }
    };
    walk(kItemWhiteStatOrder, std::size(kItemWhiteStatOrder), false);
    walk(kItemEquipStatOrder, std::size(kItemEquipStatOrder), true);
    return out;
}

/// The resistance lines: fire, nature, frost, shadow and arcane (schools 2
/// to 6) each as ITEM_RESIST_SINGLE with SPELL_SCHOOL<n>_CAP, or one
/// ITEM_RESIST_ALL line when all five equal the holy figure and it is not
/// zero. Holy on its own is never named. `res` is holy, fire, nature, frost,
/// shadow, arcane.
inline std::vector<std::string> itemResistLines(const int32_t (&res)[6], const ItemTextSource& text) {
    std::vector<std::string> out;
    bool allSame = true;
    for (int i = 1; i < 6; ++i) allSame = allSame && res[i] == res[0];
    if (allSame) {
        if (res[0] != 0) {
            out.push_back(chat_channels::luaFormat(text("ITEM_RESIST_ALL", "%c%d to All Resistances"),
                                                   {itemStatSign(res[0]), std::to_string(std::abs(res[0]))}));
        }
        return out;
    }
    static const char* const kSchoolEnUS[] = {"Fire", "Nature", "Frost", "Shadow", "Arcane"};
    for (int i = 1; i < 6; ++i) {
        if (res[i] == 0) continue;
        const std::string schoolKey = "SPELL_SCHOOL" + std::to_string(i + 1) + "_CAP";
        const std::string school = text(schoolKey.c_str(), kSchoolEnUS[i - 1]);
        out.push_back(chat_channels::luaFormat(text("ITEM_RESIST_SINGLE", "%c%d %s Resistance"),
                                               {itemStatSign(res[i]), std::to_string(std::abs(res[i])), school}));
    }
    return out;
}

}  // namespace wowee::game

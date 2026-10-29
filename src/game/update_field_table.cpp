#include "game/update_field_table.hpp"
#include "game/json_table_scan.hpp"
#include "core/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace wowee {
namespace game {

static const UpdateFieldTable* g_activeUpdateFieldTable = nullptr;

void setActiveUpdateFieldTable(const UpdateFieldTable* table) { g_activeUpdateFieldTable = table; }
const UpdateFieldTable* getActiveUpdateFieldTable() { return g_activeUpdateFieldTable; }

struct UFNameEntry {
    const char* name;
    UF field;
};

static const UFNameEntry kUFNames[] = {
    {.name = "OBJECT_FIELD_ENTRY", .field = UF::OBJECT_FIELD_ENTRY},
    {.name = "OBJECT_FIELD_SCALE_X", .field = UF::OBJECT_FIELD_SCALE_X},
    {.name = "UNIT_FIELD_TARGET_LO", .field = UF::UNIT_FIELD_TARGET_LO},
    {.name = "UNIT_FIELD_TARGET_HI", .field = UF::UNIT_FIELD_TARGET_HI},
    {.name = "UNIT_FIELD_CHANNEL_OBJECT", .field = UF::UNIT_FIELD_CHANNEL_OBJECT},
    {.name = "UNIT_CHANNEL_SPELL", .field = UF::UNIT_CHANNEL_SPELL},
    {.name = "UNIT_FIELD_BYTES_0", .field = UF::UNIT_FIELD_BYTES_0},
    {.name = "UNIT_FIELD_BYTES_1", .field = UF::UNIT_FIELD_BYTES_1},
    {.name = "UNIT_FIELD_HEALTH", .field = UF::UNIT_FIELD_HEALTH},
    {.name = "UNIT_FIELD_POWER1", .field = UF::UNIT_FIELD_POWER1},
    {.name = "UNIT_FIELD_MAXHEALTH", .field = UF::UNIT_FIELD_MAXHEALTH},
    {.name = "UNIT_FIELD_MAXPOWER1", .field = UF::UNIT_FIELD_MAXPOWER1},
    {.name = "UNIT_FIELD_LEVEL", .field = UF::UNIT_FIELD_LEVEL},
    {.name = "UNIT_FIELD_FACTIONTEMPLATE", .field = UF::UNIT_FIELD_FACTIONTEMPLATE},
    {.name = "UNIT_FIELD_FLAGS", .field = UF::UNIT_FIELD_FLAGS},
    {.name = "UNIT_FIELD_FLAGS_2", .field = UF::UNIT_FIELD_FLAGS_2},
    {.name = "UNIT_FIELD_AURASTATE", .field = UF::UNIT_FIELD_AURASTATE},
    {.name = "UNIT_FIELD_BOUNDINGRADIUS", .field = UF::UNIT_FIELD_BOUNDINGRADIUS},
    {.name = "UNIT_FIELD_COMBATREACH", .field = UF::UNIT_FIELD_COMBATREACH},
    {.name = "UNIT_FIELD_DISPLAYID", .field = UF::UNIT_FIELD_DISPLAYID},
    {.name = "UNIT_FIELD_MOUNTDISPLAYID", .field = UF::UNIT_FIELD_MOUNTDISPLAYID},
    {.name = "UNIT_FIELD_AURAS", .field = UF::UNIT_FIELD_AURAS},
    {.name = "UNIT_FIELD_AURAFLAGS", .field = UF::UNIT_FIELD_AURAFLAGS},
    {.name = "UNIT_NPC_FLAGS", .field = UF::UNIT_NPC_FLAGS},
    {.name = "UNIT_NPC_EMOTESTATE", .field = UF::UNIT_NPC_EMOTESTATE},
    {.name = "UNIT_DYNAMIC_FLAGS", .field = UF::UNIT_DYNAMIC_FLAGS},
    {.name = "UNIT_FIELD_MINDAMAGE", .field = UF::UNIT_FIELD_MINDAMAGE},
    {.name = "UNIT_FIELD_MAXDAMAGE", .field = UF::UNIT_FIELD_MAXDAMAGE},
    {.name = "UNIT_FIELD_MINOFFHANDDAMAGE", .field = UF::UNIT_FIELD_MINOFFHANDDAMAGE},
    {.name = "UNIT_FIELD_MAXOFFHANDDAMAGE", .field = UF::UNIT_FIELD_MAXOFFHANDDAMAGE},
    {.name = "UNIT_FIELD_MINRANGEDDAMAGE", .field = UF::UNIT_FIELD_MINRANGEDDAMAGE},
    {.name = "UNIT_FIELD_MAXRANGEDDAMAGE", .field = UF::UNIT_FIELD_MAXRANGEDDAMAGE},
    {.name = "UNIT_FIELD_ATTACK_POWER_MODS", .field = UF::UNIT_FIELD_ATTACK_POWER_MODS},
    {.name = "UNIT_FIELD_ATTACK_POWER_MULTIPLIER", .field = UF::UNIT_FIELD_ATTACK_POWER_MULTIPLIER},
    {.name = "UNIT_FIELD_RANGED_ATTACK_POWER_MODS", .field = UF::UNIT_FIELD_RANGED_ATTACK_POWER_MODS},
    {.name = "UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER", .field = UF::UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER},
    {.name = "UNIT_FIELD_PETEXPERIENCE", .field = UF::UNIT_FIELD_PETEXPERIENCE},
    {.name = "UNIT_FIELD_PETNUMBER", .field = UF::UNIT_FIELD_PETNUMBER},
    {.name = "UNIT_FIELD_PETNEXTLEVELEXP", .field = UF::UNIT_FIELD_PETNEXTLEVELEXP},
    {.name = "UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE", .field = UF::UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE},
    {.name = "UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE", .field = UF::UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE},
    {.name = "UNIT_FIELD_RESISTANCES", .field = UF::UNIT_FIELD_RESISTANCES},
    {.name = "UNIT_FIELD_STAT0", .field = UF::UNIT_FIELD_STAT0},
    {.name = "UNIT_FIELD_STAT1", .field = UF::UNIT_FIELD_STAT1},
    {.name = "UNIT_FIELD_STAT2", .field = UF::UNIT_FIELD_STAT2},
    {.name = "UNIT_FIELD_STAT3", .field = UF::UNIT_FIELD_STAT3},
    {.name = "UNIT_FIELD_STAT4", .field = UF::UNIT_FIELD_STAT4},
    {.name = "UNIT_FIELD_POSSTAT0", .field = UF::UNIT_FIELD_POSSTAT0},
    {.name = "UNIT_FIELD_POSSTAT1", .field = UF::UNIT_FIELD_POSSTAT1},
    {.name = "UNIT_FIELD_POSSTAT2", .field = UF::UNIT_FIELD_POSSTAT2},
    {.name = "UNIT_FIELD_POSSTAT3", .field = UF::UNIT_FIELD_POSSTAT3},
    {.name = "UNIT_FIELD_POSSTAT4", .field = UF::UNIT_FIELD_POSSTAT4},
    {.name = "UNIT_FIELD_NEGSTAT0", .field = UF::UNIT_FIELD_NEGSTAT0},
    {.name = "UNIT_FIELD_NEGSTAT1", .field = UF::UNIT_FIELD_NEGSTAT1},
    {.name = "UNIT_FIELD_NEGSTAT2", .field = UF::UNIT_FIELD_NEGSTAT2},
    {.name = "UNIT_FIELD_NEGSTAT3", .field = UF::UNIT_FIELD_NEGSTAT3},
    {.name = "UNIT_FIELD_NEGSTAT4", .field = UF::UNIT_FIELD_NEGSTAT4},
    {.name = "UNIT_END", .field = UF::UNIT_END},
    {.name = "UNIT_FIELD_ATTACK_POWER", .field = UF::UNIT_FIELD_ATTACK_POWER},
    {.name = "UNIT_FIELD_RANGED_ATTACK_POWER", .field = UF::UNIT_FIELD_RANGED_ATTACK_POWER},
    {.name = "UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER", .field = UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER},
    {.name = "UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER", .field = UF::UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER},
    {.name = "PLAYER_FLAGS", .field = UF::PLAYER_FLAGS},
    {.name = "PLAYER_BYTES", .field = UF::PLAYER_BYTES},
    {.name = "PLAYER_BYTES_2", .field = UF::PLAYER_BYTES_2},
    {.name = "PLAYER_XP", .field = UF::PLAYER_XP},
    {.name = "PLAYER_NEXT_LEVEL_XP", .field = UF::PLAYER_NEXT_LEVEL_XP},
    {.name = "PLAYER_FIELD_COINAGE", .field = UF::PLAYER_FIELD_COINAGE},
    {.name = "PLAYER_QUEST_LOG_START", .field = UF::PLAYER_QUEST_LOG_START},
    {.name = "PLAYER_FIELD_INV_SLOT_HEAD", .field = UF::PLAYER_FIELD_INV_SLOT_HEAD},
    {.name = "PLAYER_FIELD_PACK_SLOT_1", .field = UF::PLAYER_FIELD_PACK_SLOT_1},
    {.name = "PLAYER_FIELD_KEYRING_SLOT_1", .field = UF::PLAYER_FIELD_KEYRING_SLOT_1},
    {.name = "PLAYER_FIELD_BANK_SLOT_1", .field = UF::PLAYER_FIELD_BANK_SLOT_1},
    {.name = "PLAYER_FIELD_BANKBAG_SLOT_1", .field = UF::PLAYER_FIELD_BANKBAG_SLOT_1},
    {.name = "PLAYER_SKILL_INFO_START", .field = UF::PLAYER_SKILL_INFO_START},
    {.name = "PLAYER_EXPLORED_ZONES_START", .field = UF::PLAYER_EXPLORED_ZONES_START},
    {.name = "UNIT_FIELD_CRITTER", .field = UF::UNIT_FIELD_CRITTER},
    {.name = "UNIT_FIELD_CHARM", .field = UF::UNIT_FIELD_CHARM},
    {.name = "UNIT_FIELD_SUMMON", .field = UF::UNIT_FIELD_SUMMON},
    {.name = "UNIT_FIELD_CHARMEDBY", .field = UF::UNIT_FIELD_CHARMEDBY},
    {.name = "UNIT_FIELD_SUMMONEDBY_LO", .field = UF::UNIT_FIELD_SUMMONEDBY_LO},
    {.name = "UNIT_FIELD_SUMMONEDBY_HI", .field = UF::UNIT_FIELD_SUMMONEDBY_HI},
    {.name = "UNIT_FIELD_CREATEDBY", .field = UF::UNIT_FIELD_CREATEDBY},
    {.name = "UNIT_CREATED_BY_SPELL", .field = UF::UNIT_CREATED_BY_SPELL},
    {.name = "GAMEOBJECT_FIELD_CREATED_BY", .field = UF::GAMEOBJECT_FIELD_CREATED_BY},
    {.name = "GAMEOBJECT_DISPLAYID", .field = UF::GAMEOBJECT_DISPLAYID},
    {.name = "UNIT_FIELD_BYTES_2", .field = UF::UNIT_FIELD_BYTES_2},
    {.name = "UNIT_VIRTUAL_ITEM_SLOT_ID", .field = UF::UNIT_VIRTUAL_ITEM_SLOT_ID},
    {.name = "UNIT_VIRTUAL_ITEM_SLOT_DISPLAY", .field = UF::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY},
    {.name = "UNIT_VIRTUAL_ITEM_INFO", .field = UF::UNIT_VIRTUAL_ITEM_INFO},
    {.name = "CORPSE_FIELD_FLAGS", .field = UF::CORPSE_FIELD_FLAGS},
    {.name = "CORPSE_FIELD_DISPLAY_ID", .field = UF::CORPSE_FIELD_DISPLAY_ID},
    {.name = "CORPSE_FIELD_ITEM", .field = UF::CORPSE_FIELD_ITEM},
    {.name = "CORPSE_FIELD_BYTES_1", .field = UF::CORPSE_FIELD_BYTES_1},
    {.name = "CORPSE_FIELD_BYTES_2", .field = UF::CORPSE_FIELD_BYTES_2},
    {.name = "CORPSE_FIELD_GUILD", .field = UF::CORPSE_FIELD_GUILD},
    {.name = "CORPSE_FIELD_DYNAMIC_FLAGS", .field = UF::CORPSE_FIELD_DYNAMIC_FLAGS},
    {.name = "PLAYER_DUEL_ARBITER", .field = UF::PLAYER_DUEL_ARBITER},
    {.name = "PLAYER_DUEL_TEAM", .field = UF::PLAYER_DUEL_TEAM},
    {.name = "UNIT_MOD_CAST_SPEED", .field = UF::UNIT_MOD_CAST_SPEED},
    {.name = "UNIT_FIELD_BASE_MANA", .field = UF::UNIT_FIELD_BASE_MANA},
    {.name = "UNIT_FIELD_BASE_HEALTH", .field = UF::UNIT_FIELD_BASE_HEALTH},
    {.name = "UNIT_FIELD_POWER_COST_MODIFIER", .field = UF::UNIT_FIELD_POWER_COST_MODIFIER},
    {.name = "UNIT_FIELD_POWER_COST_MULTIPLIER", .field = UF::UNIT_FIELD_POWER_COST_MULTIPLIER},
    {.name = "PLAYER_FIELD_BYTES", .field = UF::PLAYER_FIELD_BYTES},
    {.name = "UNIT_FIELD_RANGEDATTACKTIME", .field = UF::UNIT_FIELD_RANGEDATTACKTIME},
    {.name = "UNIT_FIELD_BASEATTACKTIME", .field = UF::UNIT_FIELD_BASEATTACKTIME},
    {.name = "UNIT_FIELD_OFFHANDATTACKTIME", .field = UF::UNIT_FIELD_OFFHANDATTACKTIME},
    {.name = "PLAYER_RUNE_REGEN_1", .field = UF::PLAYER_RUNE_REGEN_1},
    {.name = "PLAYER_FARSIGHT", .field = UF::PLAYER_FARSIGHT},
    {.name = "PLAYER_AMMO_ID", .field = UF::PLAYER_AMMO_ID},
    {.name = "PLAYER_SELF_RES_SPELL", .field = UF::PLAYER_SELF_RES_SPELL},
    {.name = "PLAYER_NO_REAGENT_COST_1", .field = UF::PLAYER_NO_REAGENT_COST_1},
    {.name = "PLAYER_FIELD_GLYPH_SLOTS_1", .field = UF::PLAYER_FIELD_GLYPH_SLOTS_1},
    {.name = "PLAYER_FIELD_GLYPHS_1", .field = UF::PLAYER_FIELD_GLYPHS_1},
    {.name = "PLAYER_GLYPHS_ENABLED", .field = UF::PLAYER_GLYPHS_ENABLED},
    {.name = "PLAYER_FIELD_PET_SPELL_POWER", .field = UF::PLAYER_FIELD_PET_SPELL_POWER},
    {.name = "GAMEOBJECT_FLAGS", .field = UF::GAMEOBJECT_FLAGS},
    {.name = "GAMEOBJECT_BYTES_1", .field = UF::GAMEOBJECT_BYTES_1},
    {.name = "GAMEOBJECT_DYNAMIC", .field = UF::GAMEOBJECT_DYNAMIC},
    {.name = "GAMEOBJECT_LEVEL", .field = UF::GAMEOBJECT_LEVEL},
    {.name = "DYNAMICOBJECT_BYTES", .field = UF::DYNAMICOBJECT_BYTES},
    {.name = "DYNAMICOBJECT_SPELLID", .field = UF::DYNAMICOBJECT_SPELLID},
    {.name = "DYNAMICOBJECT_RADIUS", .field = UF::DYNAMICOBJECT_RADIUS},
    {.name = "DYNAMICOBJECT_CASTER", .field = UF::DYNAMICOBJECT_CASTER},
    {.name = "DYNAMICOBJECT_CASTTIME", .field = UF::DYNAMICOBJECT_CASTTIME},
    {.name = "GAMEOBJECT_FACTION", .field = UF::GAMEOBJECT_FACTION},
    {.name = "GAMEOBJECT_PARENTROTATION", .field = UF::GAMEOBJECT_PARENTROTATION},
    {.name = "ITEM_FIELD_STACK_COUNT", .field = UF::ITEM_FIELD_STACK_COUNT},
    {.name = "ITEM_FIELD_DURABILITY", .field = UF::ITEM_FIELD_DURABILITY},
    {.name = "ITEM_FIELD_MAXDURABILITY", .field = UF::ITEM_FIELD_MAXDURABILITY},
    {.name = "PLAYER_REST_STATE_EXPERIENCE", .field = UF::PLAYER_REST_STATE_EXPERIENCE},
    {.name = "PLAYER_CHOSEN_TITLE", .field = UF::PLAYER_CHOSEN_TITLE},
    {.name = "PLAYER_BYTES_3", .field = UF::PLAYER_BYTES_3},
    {.name = "PLAYER_FAKE_INEBRIATION", .field = UF::PLAYER_FAKE_INEBRIATION},
    {.name = "PLAYER_FIELD_BYTES2", .field = UF::PLAYER_FIELD_BYTES2},
    {.name = "PLAYER_FIELD_KILLS", .field = UF::PLAYER_FIELD_KILLS},
    {.name = "PLAYER_FIELD_TODAY_CONTRIBUTION", .field = UF::PLAYER_FIELD_TODAY_CONTRIBUTION},
    {.name = "PLAYER_FIELD_YESTERDAY_CONTRIBUTION", .field = UF::PLAYER_FIELD_YESTERDAY_CONTRIBUTION},
    {.name = "PLAYER_FIELD_LIFETIME_HONORBALE_KILLS", .field = UF::PLAYER_FIELD_LIFETIME_HONORBALE_KILLS},
    {.name = "PLAYER_FIELD_WATCHED_FACTION_INDEX", .field = UF::PLAYER_FIELD_WATCHED_FACTION_INDEX},
    {.name = "PLAYER_FIELD_MOD_DAMAGE_DONE_POS", .field = UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS},
    {.name = "PLAYER_FIELD_MOD_DAMAGE_DONE_NEG", .field = UF::PLAYER_FIELD_MOD_DAMAGE_DONE_NEG},
    {.name = "PLAYER_FIELD_MOD_DAMAGE_DONE_PCT", .field = UF::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT},
    {.name = "PLAYER_FIELD_MOD_HEALING_DONE_POS", .field = UF::PLAYER_FIELD_MOD_HEALING_DONE_POS},
    {.name = "PLAYER_FIELD_MOD_TARGET_RESISTANCE", .field = UF::PLAYER_FIELD_MOD_TARGET_RESISTANCE},
    {.name = "PLAYER_CHARACTER_POINTS2", .field = UF::PLAYER_CHARACTER_POINTS2},
    {.name = "PLAYER_TRACK_CREATURES", .field = UF::PLAYER_TRACK_CREATURES},
    {.name = "PLAYER_TRACK_RESOURCES", .field = UF::PLAYER_TRACK_RESOURCES},
    {.name = "PLAYER_BLOCK_PERCENTAGE", .field = UF::PLAYER_BLOCK_PERCENTAGE},
    {.name = "PLAYER_DODGE_PERCENTAGE", .field = UF::PLAYER_DODGE_PERCENTAGE},
    {.name = "PLAYER_PARRY_PERCENTAGE", .field = UF::PLAYER_PARRY_PERCENTAGE},
    {.name = "PLAYER_CRIT_PERCENTAGE", .field = UF::PLAYER_CRIT_PERCENTAGE},
    {.name = "PLAYER_RANGED_CRIT_PERCENTAGE", .field = UF::PLAYER_RANGED_CRIT_PERCENTAGE},
    {.name = "PLAYER_SPELL_CRIT_PERCENTAGE1", .field = UF::PLAYER_SPELL_CRIT_PERCENTAGE1},
    {.name = "PLAYER_SHIELD_BLOCK", .field = UF::PLAYER_SHIELD_BLOCK},
    {.name = "PLAYER_FIELD_COMBAT_RATING_1", .field = UF::PLAYER_FIELD_COMBAT_RATING_1},
    {.name = "PLAYER_EXPERTISE", .field = UF::PLAYER_EXPERTISE},
    {.name = "PLAYER_OFFHAND_EXPERTISE", .field = UF::PLAYER_OFFHAND_EXPERTISE},
    {.name = "PLAYER_FIELD_HONOR_CURRENCY", .field = UF::PLAYER_FIELD_HONOR_CURRENCY},
    {.name = "PLAYER_FIELD_ARENA_CURRENCY", .field = UF::PLAYER_FIELD_ARENA_CURRENCY},
    {.name = "PLAYER_FIELD_KNOWN_CURRENCIES", .field = UF::PLAYER_FIELD_KNOWN_CURRENCIES},
    {.name = "PLAYER_FIELD_KNOWN_TITLES", .field = UF::PLAYER_FIELD_KNOWN_TITLES},
    {.name = "PLAYER_FIELD_ARENA_TEAM_INFO_1_1", .field = UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1},
    {.name = "CONTAINER_FIELD_NUM_SLOTS", .field = UF::CONTAINER_FIELD_NUM_SLOTS},
    {.name = "CONTAINER_FIELD_SLOT_1", .field = UF::CONTAINER_FIELD_SLOT_1},
};


bool UpdateFieldTable::loadFromJson(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("UpdateFieldTable: cannot open ", path);
        return false;
    }

    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    fieldMap_.clear();
    size_t loaded = 0;

    forEachJsonKeyValue(json, [&](const std::string& key, const std::string& valStr) {
        uint32_t parsed = 0;
        // Skipped rather than stored when it is not wholly a number. Storing
        // what a partial parse returned would put index 0 - the object GUID -
        // under a name that reads correctly everywhere it is used.
        if (!parseTableNumber(valStr, parsed)) return;
        const uint16_t idx = static_cast<uint16_t>(parsed);

        for (auto entry : kUFNames) {
            if (key == entry.name) {
                fieldMap_[static_cast<uint16_t>(entry.field)] = idx;
                ++loaded;
                break;
            }
        }
    });

    if (loaded == 0) {
        LOG_WARNING("UpdateFieldTable: no fields loaded from ", path);
        return false;
    }

    LOG_INFO("UpdateFieldTable: loaded ", loaded, " fields from ", path);
    return true;
}

uint16_t UpdateFieldTable::index(UF field) const {
    auto it = fieldMap_.find(static_cast<uint16_t>(field));
    return (it != fieldMap_.end()) ? it->second : 0xFFFF;
}

bool UpdateFieldTable::hasField(UF field) const {
    return fieldMap_.count(static_cast<uint16_t>(field)) > 0;
}

} // namespace game
} // namespace wowee

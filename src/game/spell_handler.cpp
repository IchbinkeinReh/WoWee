#include "game/spell_handler.hpp"
#include "game/shapeshift_forms.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/protocol_constants.hpp"
#include "game/gather_spells.hpp"
#include "game/spell_classification.hpp"
#include "game/ground_target.hpp"
#include "game/spell_target_kind.hpp"
#include "game/pet_action.hpp"
#include "game/pvp_flags.hpp"
#include "game/spell_unit_target.hpp"
#include "game/vehicle_mover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/go_interaction_anim.hpp"
#include "game/transport_manager.hpp"
#include "game/spell_target_point.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/spell_missile.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/spell_sound_manager.hpp"
#include "core/app_clock.hpp"
#include "core/application.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/interface_text.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "audio/ui_sound_manager.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>

namespace wowee {
namespace game {

namespace {

/// SKILL_RANK_UP with the skill and its new value put in: "%s"/"%1$s" takes
/// the name and "%d"/"%2$d" the value, as the localized strings write them.
std::string formatSkillRankUp(const std::string& format, const std::string& skill, uint32_t value) {
    std::string out;
    int nextArg = 1;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            out += format[i];
            continue;
        }
        size_t j = i + 1;
        int arg = 0;
        if (std::isdigit(static_cast<unsigned char>(format[j])) && j + 1 < format.size() && format[j + 1] == '$') {
            arg = format[j] - '0';
            j += 2;
        }
        if (j >= format.size()) { out += format[i]; continue; }
        const char conv = format[j];
        if (conv == 's' || conv == 'd') {
            if (arg == 0) arg = nextArg++;
            out += arg == 1 ? skill : std::to_string(value);
            i = j;
        } else if (conv == '%') {
            out += '%';
            i = j;
        } else {
            out += format[i];
        }
    }
    return out;
}

}  // namespace

namespace {
constexpr uint8_t kSpellFailedNotReady = 67;
constexpr uint8_t kSpellFailedAlreadyOpen = 8;
constexpr uint8_t kSpellFailedChestInUse = 25;
constexpr uint8_t kSpellFailedTryAgain = 132;
constexpr uint8_t kSpellFailedItemNotReady = 45;
constexpr uint8_t kSpellFailedMoving = 51;
constexpr uint8_t kSpellFailedNotBehind = 57;
constexpr uint8_t kSpellFailedNoPower = 85;
constexpr uint8_t kSpellFailedOutOfRange = 97;
constexpr uint8_t kSpellFailedSpellInProgress = 105;
constexpr uint8_t kSpellFailedTooClose = 128;
constexpr uint8_t kSpellFailedUnitNotInFront = 134;

/// A rejection the player caused by pressing a key a moment too early, or from
/// half a step out of place.
///
/// These arrive once per keypress and a held key repeats them, so a few seconds
/// of a fight produces several a second: the cooldown, the power cost, the
/// range, the facing. The red line above the middle of the screen is what the
/// real client shows for them - no chat line, and no error chime, since a beep
/// per rejected keypress is the same spam in another channel. The character's
/// line for them is still spoken (sayCastFailure), one at a time.
///
/// Codes checked against AzerothCore's SharedDefines.h rather than inferred:
/// 128 is TOO_CLOSE, not the in-progress code it reads like.
bool isRoutineCastRejection(uint8_t result) {
    switch (result) {
        case kSpellFailedItemNotReady:
        case kSpellFailedMoving:
        case kSpellFailedNotBehind:
        case kSpellFailedNotReady:
        case kSpellFailedNoPower:
        case kSpellFailedOutOfRange:
        case kSpellFailedSpellInProgress:
        case kSpellFailedTooClose:
        case kSpellFailedTryAgain:
        case kSpellFailedUnitNotInFront:
            return true;
        default:
            return false;
    }
}

/// A spell's name as the learn and unlearn lines give it: "Name (Rank)", or
/// the name alone where Spell.dbc has no rank (0x00542030, 0x006e71d0).
std::string spellNameWithRank(const GameHandler& owner, uint32_t spellId) {
    const std::string& name = owner.getSpellName(spellId);
    const std::string& rank = owner.getSpellRank(spellId);
    return rank.empty() ? name : name + " (" + rank + ")";
}

/// The chat line a newly learned spell gets, as 0x00542030 picks it: nothing
/// for a spell hidden from the spell book (Attributes 0x80),
/// ERR_LEARN_RECIPE_S for a trade spell (0x20), ERR_LEARN_COMPANION_S with
/// the creature's name for a companion or mount (0x008009b0), and otherwise
/// ERR_LEARN_ABILITY_S (0x10) or ERR_LEARN_SPELL_S. Empty where the spell, or
/// a companion's creature, is not known - the original waits on the creature
/// query for that one and prints when it answers; this does not.
std::string learnedSpellText(const GameHandler& owner, uint32_t spellId) {
    const auto attrs = owner.getSpellAttributes(spellId);
    const std::string& name = owner.getSpellName(spellId);
    if (!attrs || name.empty() || (*attrs & 0x80u) != 0) return {};
    if ((*attrs & 0x20u) != 0)
        return interfaceFormat("ERR_LEARN_RECIPE_S", "You have learned how to create a new item: %s.", {name});
    for (const bool mounts : {false, true}) {
        for (const Companion& c : owner.getCompanions(mounts)) {
            if (c.spellId != spellId) continue;
            const std::string creature = owner.getCachedCreatureName(c.creatureId);
            if (creature.empty()) return {};
            return interfaceFormat("ERR_LEARN_COMPANION_S", "You have added %s to your collection.", {creature});
        }
    }
    if ((*attrs & 0x10u) != 0)
        return interfaceFormat("ERR_LEARN_ABILITY_S", "You have learned a new ability: %s.",
                               {spellNameWithRank(owner, spellId)});
    return interfaceFormat("ERR_LEARN_SPELL_S", "You have learned a new spell: %s.",
                           {spellNameWithRank(owner, spellId)});
}

/// SpellCastResult -> its SPELL_FAILED_ key, less the prefix, as 0x007fcca0
/// names them; anything past the end is SPELL_FAILED_UNKNOWN.
constexpr const char* kSpellFailedNames[] = {
    "SUCCESS", "AFFECTING_COMBAT", "ALREADY_AT_FULL_HEALTH", "ALREADY_AT_FULL_MANA",
    "ALREADY_AT_FULL_POWER", "ALREADY_BEING_TAMED", "ALREADY_HAVE_CHARM", "ALREADY_HAVE_SUMMON",
    "ALREADY_OPEN", "AURA_BOUNCED", "AUTOTRACK_INTERRUPTED", "BAD_IMPLICIT_TARGETS", "BAD_TARGETS",
    "CANT_BE_CHARMED", "CANT_BE_DISENCHANTED", "CANT_BE_DISENCHANTED_SKILL", "CANT_BE_MILLED",
    "CANT_BE_PROSPECTED", "CANT_CAST_ON_TAPPED", "CANT_DUEL_WHILE_INVISIBLE",
    "CANT_DUEL_WHILE_STEALTHED", "CANT_STEALTH", "CASTER_AURASTATE", "CASTER_DEAD", "CHARMED",
    "CHEST_IN_USE", "CONFUSED", "DONT_REPORT", "EQUIPPED_ITEM", "EQUIPPED_ITEM_CLASS",
    "EQUIPPED_ITEM_CLASS_MAINHAND", "EQUIPPED_ITEM_CLASS_OFFHAND", "ERROR", "FIZZLE", "FLEEING",
    "FOOD_LOWLEVEL", "HIGHLEVEL", "HUNGER_SATIATED", "IMMUNE", "INCORRECT_AREA", "INTERRUPTED",
    "INTERRUPTED_COMBAT", "ITEM_ALREADY_ENCHANTED", "ITEM_GONE", "ITEM_NOT_FOUND",
    "ITEM_NOT_READY", "LEVEL_REQUIREMENT", "LINE_OF_SIGHT", "LOWLEVEL", "LOW_CASTLEVEL",
    "MAINHAND_EMPTY", "MOVING", "NEED_AMMO", "NEED_AMMO_POUCH", "NEED_EXOTIC_AMMO",
    "NEED_MORE_ITEMS", "NOPATH", "NOT_BEHIND", "NOT_FISHABLE", "NOT_FLYING", "NOT_HERE",
    "NOT_INFRONT", "NOT_IN_CONTROL", "NOT_KNOWN", "NOT_MOUNTED", "NOT_ON_TAXI", "NOT_ON_TRANSPORT",
    "NOT_READY", "NOT_SHAPESHIFT", "NOT_STANDING", "NOT_TRADEABLE", "NOT_TRADING",
    "NOT_UNSHEATHED", "NOT_WHILE_GHOST", "NOT_WHILE_LOOTING", "NO_AMMO", "NO_CHARGES_REMAIN",
    "NO_CHAMPION", "NO_COMBO_POINTS", "NO_DUELING", "NO_ENDURANCE", "NO_FISH",
    "NO_ITEMS_WHILE_SHAPESHIFTED", "NO_MOUNTS_ALLOWED", "NO_PET", "NO_POWER", "NOTHING_TO_DISPEL",
    "NOTHING_TO_STEAL", "ONLY_ABOVEWATER", "ONLY_DAYTIME", "ONLY_INDOORS", "ONLY_MOUNTED",
    "ONLY_NIGHTTIME", "ONLY_OUTDOORS", "ONLY_SHAPESHIFT", "ONLY_STEALTHED", "ONLY_UNDERWATER",
    "OUT_OF_RANGE", "PACIFIED", "POSSESSED", "REAGENTS", "REQUIRES_AREA", "REQUIRES_SPELL_FOCUS",
    "ROOTED", "SILENCED", "SPELL_IN_PROGRESS", "SPELL_LEARNED", "SPELL_UNAVAILABLE", "STUNNED",
    "TARGETS_DEAD", "TARGET_AFFECTING_COMBAT", "TARGET_AURASTATE", "TARGET_DUELING",
    "TARGET_ENEMY", "TARGET_ENRAGED", "TARGET_FRIENDLY", "TARGET_IN_COMBAT", "TARGET_IS_PLAYER",
    "TARGET_IS_PLAYER_CONTROLLED", "TARGET_NOT_DEAD", "TARGET_NOT_IN_PARTY", "TARGET_NOT_LOOTED",
    "TARGET_NOT_PLAYER", "TARGET_NO_POCKETS", "TARGET_NO_WEAPONS", "TARGET_NO_RANGED_WEAPONS",
    "TARGET_UNSKINNABLE", "THIRST_SATIATED", "TOO_CLOSE", "TOO_MANY_OF_ITEM", "TOTEM_CATEGORY",
    "TOTEMS", "TRY_AGAIN", "UNIT_NOT_BEHIND", "UNIT_NOT_INFRONT", "WRONG_PET_FOOD",
    "NOT_WHILE_FATIGUED", "TARGET_NOT_IN_INSTANCE", "NOT_WHILE_TRADING", "TARGET_NOT_IN_RAID",
    "TARGET_FREEFORALL", "NO_EDIBLE_CORPSES", "ONLY_BATTLEGROUNDS", "TARGET_NOT_GHOST",
    "TRANSFORM_UNUSABLE", "WRONG_WEATHER", "DAMAGE_IMMUNE", "PREVENTED_BY_MECHANIC", "PLAY_TIME",
    "REPUTATION", "MIN_SKILL", "NOT_IN_ARENA", "NOT_ON_SHAPESHIFT", "NOT_ON_STEALTHED",
    "NOT_ON_DAMAGE_IMMUNE", "NOT_ON_MOUNTED", "TOO_SHALLOW", "TARGET_NOT_IN_SANCTUARY",
    "TARGET_IS_TRIVIAL", "BM_OR_INVISGOD", "EXPERT_RIDING_REQUIREMENT",
    "ARTISAN_RIDING_REQUIREMENT", "NOT_IDLE", "NOT_INACTIVE", "PARTIAL_PLAYTIME", "NO_PLAYTIME",
    "NOT_IN_BATTLEGROUND", "NOT_IN_RAID_INSTANCE", "ONLY_IN_ARENA",
    "TARGET_LOCKED_TO_RAID_INSTANCE", "ON_USE_ENCHANT", "NOT_ON_GROUND", "CUSTOM_ERROR",
    "CANT_DO_THAT_RIGHT_NOW", "TOO_MANY_SOCKETS", "INVALID_GLYPH", "UNIQUE_GLYPH",
    "GLYPH_SOCKET_LOCKED", "NO_VALID_TARGETS", "ITEM_AT_MAX_CHARGES", "NOT_IN_BARBERSHOP",
    "FISHING_TOO_LOW", "ITEM_ENCHANT_TRADE_WINDOW", "SUMMON_PENDING", "MAX_SOCKETS",
    "PET_CAN_RENAME", "TARGET_CANNOT_BE_RESURRECTED",
};

/// The SPELL_FAILED_* text for a result, unformatted, or the English this
/// client had where there is no interface.
std::string spellFailedText(uint8_t result, int powerType) {
    const std::string key = std::string("SPELL_FAILED_") +
        (result < std::size(kSpellFailedNames) ? kSpellFailedNames[result] : "UNKNOWN");
    const char* enUS = getSpellCastResultString(result, powerType);
    return interfaceString(key.c_str(), enUS ? enUS : "Unknown reason");
}

/// What 0x00807f10 and 0x00808200 read off the failed spell's row
/// (cast_failure::SpellRow); all zero for a spell this client has no row of.
cast_failure::SpellRow spellRowOf(const GameHandler& owner, uint32_t spellId) {
    cast_failure::SpellRow row;
    owner.loadSpellNameCache();
    const auto it = owner.spellNameCacheRef().find(spellId);
    if (it == owner.spellNameCacheRef().end()) return row;
    const auto& e = it->second;
    row.category = e.category;
    row.attributes = e.attr;
    row.targets = e.targetFlags;
    row.powerType = static_cast<int32_t>(e.powerType);
    // A PowerDisplay row by the id (0x0065c290): none for 0 or -1.
    if (e.powerDisplayId != 0 && e.powerDisplayId != 0xFFFFFFFFu) {
        auto* am = owner.services().assetManager;
        auto dbc = am ? am->loadDBCOptional("PowerDisplay.dbc") : nullptr;
        row.powerDisplay = dbc && dbc->findRecordById(e.powerDisplayId) >= 0;
    }
    return row;
}

/// A game error's words in English, for an interface without them.
const char* castFailureEnUS(std::string_view key) {
    struct Words { std::string_view key; const char* enUS; };
    static constexpr Words kWords[] = {
        {"ERR_SPELL_FAILED_ALREADY_AT_FULL_HEALTH", "You are already at full health."},
        {"ERR_SPELL_FAILED_ALREADY_AT_FULL_MANA", "You are already at full mana."},
        {"ERR_SPELL_FAILED_ALREADY_AT_FULL_POWER_S", "You are already at full %s."},
        {"ERR_GENERIC_NO_TARGET", "You have no target."},
        {"ERR_INVALID_ITEM_TARGET", "That item is not a valid target."},
        {"ERR_SPELL_FAILED_EQUIPPED_ITEM", "Must have the proper item equipped."},
        {"ERR_ITEM_COOLDOWN", "Item is not ready yet."},
        {"ERR_POTION_COOLDOWN", "You cannot drink any more yet."},
        {"ERR_FOOD_COOLDOWN", "You are too full to eat more now."},
        {"ERR_ABILITY_COOLDOWN", "Ability is not ready yet."},
        {"ERR_SPELL_COOLDOWN", "Spell is not ready yet."},
        {"ERR_SPELL_FAILED_NOTUNSHEATHED", "You have to be unsheathed to do that!"},
        {"ERR_SPELL_OUT_OF_RANGE", "Out of range."},
        {"ERR_PLAY_TIME_EXCEEDED", "Max play time exceeded"},
        {"ERR_BAD_ON_USE_ENCHANT", "This item already has an on use effect."},
        {"ERR_TOO_MANY_SOCKETS", "Too many sockets"},
        {"ERR_GENERIC_NO_VALID_TARGETS", "No valid targets."},
        {"ERR_MAX_SOCKETS", "This item already has the maximum number of sockets"},
    };
    for (const Words& w : kWords) {
        if (w.key == key) return w.enUS;
    }
    // ERR_SPELL_FAILED_S and the other "%s" ones; and ERR_INVALID_GLYPH_SLOT,
    // whose words this client does not carry - the SPELL_FAILED_ text stands
    // in for them.
    return "%s";
}

/// The ERR_ line a player's cast failure is shown through, as 0x00807f10
/// picks it (cast_failure::gameError). Most are ERR_SPELL_FAILED_S, which is
/// "%s" and shows the SPELL_FAILED_ text as it is; the others put that text
/// inside their own words or replace it. NO_POWER's is worded by
/// castFailureMessage, by the power.
struct CastFailureLine { const char* key; const char* enUS; };
CastFailureLine castFailureLine(const GameHandler& owner, uint32_t spellId, uint8_t result) {
    if (result == 0x55) return {"ERR_SPELL_FAILED_S", "%s"};
    // A string literal, so its view is terminated.
    const std::string_view key = cast_failure::gameError(result, spellRowOf(owner, spellId));
    return {key.data(), castFailureEnUS(key)};
}

/// The interface's name for a power, as 0x007fe4b0 lists them.
const char* powerTokenKey(int powerType) {
    static constexpr const char* kKeys[] = {"MANA", "RAGE", "FOCUS", "ENERGY", "HAPPINESS", "RUNES", "RUNIC_POWER"};
    return (powerType >= 0 && powerType < 7) ? kKeys[powerType] : nullptr;
}

/// The line a player's failed cast shows on screen, as 0x00808200 words it:
/// the SPELL_FAILED_ text, with the name it takes put in for the results that
/// name something, inside the ERR_ line 0x00807f10 picks. Empty for a result
/// the original reports nothing for - SPELL_FAILED_DONT_REPORT, or a named
/// thing that is not known.
///
/// This used to be English throughout, and invented lines of its own for
/// fishing, bandages, aura states, crafting stations, tools and gathering;
/// the original shows the same SPELL_FAILED_ text for those as for anything
/// else. Where a result names something this client does not look up - an
/// item class, a form, a mechanic, a custom error - the English it had is
/// kept rather than showing an unfilled %s.
std::string castFailureMessage(const GameHandler& owner, uint32_t spellId,
                               uint8_t result, int powerType, uint32_t miscArg = 0,
                               uint32_t miscArg2 = 0) {
    if (result == 0 || result == 0x1b) return {};  // SUCCESS, DONT_REPORT
    std::string text = spellFailedText(result, powerType);
    auto fill = [&](const std::vector<std::string>& args) {
        text = chat_channels::luaFormat(text, args);
    };
    auto itemName = [&](uint32_t itemId) -> std::string {
        const auto* info = owner.getItemInfo(itemId);
        return (info && info->valid) ? info->name : std::string();
    };
    switch (result) {
        case 0x04: {  // ALREADY_AT_FULL_POWER: the power's own name
            const char* key = powerTokenKey(powerType);
            if (!key) return {};
            text = interfaceString(key, key);
            break;
        }
        case 0x55: {  // NO_POWER: ERR_OUT_OF_<power> alone (table 0x00a439c8)
            static constexpr const char* kOutOf[][2] = {
                {"ERR_OUT_OF_MANA", "Not enough mana"},   {"ERR_OUT_OF_RAGE", "Not enough rage"},
                {"ERR_OUT_OF_FOCUS", "Not enough focus"}, {"ERR_OUT_OF_ENERGY", "Not enough energy"},
                {"ERR_NOT_HAPPY_ENOUGH", "Your pet is not happy enough"},
                {"ERR_OUT_OF_RUNES", "Not enough runes"}, {"ERR_OUT_OF_RUNIC_POWER", "Not enough runic power"},
            };
            if (powerType == -2) return interfaceString("ERR_OUT_OF_HEALTH", "Not enough health");
            if (powerType >= 0 && powerType < 7)
                return interfaceString(kOutOf[powerType][0], kOutOf[powerType][1]);
            break;
        }
        case 0x65: {  // REQUIRES_AREA: the area's name, or the text as it is
            const std::string area = miscArg ? owner.getAreaName(miscArg) : std::string();
            if (!area.empty()) fill({area});
            break;
        }
        case kCastResultRequiresSpellFocus: {  // the focus object's name, or the text as it is
            const std::string& focus = owner.getSpellFocusName(miscArg);
            if (!focus.empty()) fill({focus});
            break;
        }
        case kCastResultTotemCategory: {  // the tool's name, or nothing at all
            const std::string& tool = owner.getTotemCategoryName(miscArg);
            if (tool.empty()) return {};
            fill({tool});
            break;
        }
        case kCastResultTotems:
        case 0x64: {  // TOTEMS, REAGENTS: the item's name, or nothing at all
            const std::string name = itemName(miscArg);
            if (name.empty()) return {};
            fill({name});
            break;
        }
        case 0x37: {  // NEED_MORE_ITEMS: how many of which item
            const std::string name = itemName(miscArg);
            if (name.empty()) return {};
            fill({std::to_string(miscArg2), name});
            break;
        }
        case 0x96: {  // MIN_SKILL: the skill line and the rank it wants
            std::string skill = owner.getSkillLineName(miscArg);
            if (skill.empty()) skill = "UNKNOWN";
            fill({skill, std::to_string(miscArg2)});
            break;
        }
        case 0xb5:  // FISHING_TOO_LOW: the rank it wants
            fill({std::to_string(miscArg)});
            break;
        case 0x1d: case 0x1e: case 0x1f: case 0x36: case 0x5e: case 0x93: case 0xac: case 0xb0: {
            const char* enUS = getSpellCastResultString(result, powerType);
            if (enUS) text = enUS;
            break;
        }
        default:
            break;
    }
    const CastFailureLine line = castFailureLine(owner, spellId, result);
    return chat_channels::luaFormat(interfaceString(line.key, line.enUS), {text});
}

bool isGatherSpellId(uint32_t spellId) {
    return isGatherRank(spellId);
}

bool shouldDespawnGatherTarget(uint8_t result) {
    return result == kSpellFailedAlreadyOpen || result == kSpellFailedChestInUse;
}

} // namespace

static CombatTextEntry::Type combatTextTypeFromSpellMissInfo(uint8_t missInfo) {
    switch (missInfo) {
        case SpellMissInfo::MISS:    return CombatTextEntry::MISS;
        case SpellMissInfo::DODGE:   return CombatTextEntry::DODGE;
        case SpellMissInfo::PARRY:   return CombatTextEntry::PARRY;
        case SpellMissInfo::BLOCK:   return CombatTextEntry::BLOCK;
        case SpellMissInfo::EVADE:   return CombatTextEntry::EVADE;
        case SpellMissInfo::IMMUNE:  return CombatTextEntry::IMMUNE;
        case SpellMissInfo::DEFLECT: return CombatTextEntry::DEFLECT;
        case SpellMissInfo::ABSORB:  return CombatTextEntry::ABSORB;
        case SpellMissInfo::RESIST:  return CombatTextEntry::RESIST;
        case SpellMissInfo::IMMUNE2:
        case SpellMissInfo::IMMUNE3:
            return CombatTextEntry::IMMUNE;
        case SpellMissInfo::REFLECT: return CombatTextEntry::REFLECT;
        default: return CombatTextEntry::MISS;
    }
}

static audio::SpellSoundManager::MagicSchool schoolMaskToMagicSchool(uint32_t mask) {
    if (mask & 0x04) return audio::SpellSoundManager::MagicSchool::FIRE;
    if (mask & 0x10) return audio::SpellSoundManager::MagicSchool::FROST;
    if (mask & 0x02) return audio::SpellSoundManager::MagicSchool::HOLY;
    if (mask & 0x08) return audio::SpellSoundManager::MagicSchool::NATURE;
    if (mask & 0x20) return audio::SpellSoundManager::MagicSchool::SHADOW;
    if (mask & 0x40) return audio::SpellSoundManager::MagicSchool::ARCANE;
    return audio::SpellSoundManager::MagicSchool::ARCANE;
}

// ---- Extracted helpers to reduce nesting in handleSpellGo ----

audio::SpellSoundManager::MagicSchool SpellHandler::resolveSpellSchool(uint32_t spellId) {
    owner_.loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it != owner_.spellNameCacheRef().end() && it->second.schoolMask)
        return schoolMaskToMagicSchool(it->second.schoolMask);
    return audio::SpellSoundManager::MagicSchool::ARCANE;
}

void SpellHandler::playSpellCastSound(uint32_t spellId) {
    auto* ac = owner_.services().audioCoordinator;
    if (!ac) return;
    auto* ssm = ac->getSpellSoundManager();
    if (!ssm) return;
    ssm->playCast(resolveSpellSchool(spellId));
}

// ---- Spell visual effect helpers ----

uint32_t SpellHandler::resolveSpellVisualId(uint32_t spellId) { return owner_.getSpellVisualId(spellId); }

bool SpellHandler::resolveUnitPosition(uint64_t guid, glm::vec3& outPos) {
    auto* renderer = owner_.services().renderer;
    if (!renderer) return false;
    if (guid == owner_.getPlayerGuid()) {
        outPos = renderer->getCharacterPosition();
        return true;
    }
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity) return false;
    glm::vec3 canonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
    outPos = core::coords::canonicalToRender(canonical);
    return true;
}

std::optional<glm::vec3> SpellHandler::pointInObjectFrame(uint64_t guid, const glm::vec3& wirePoint) {
    // The object's world matrix (0x0074b430 -> vtable +0xc4): a transport the
    // manager runs through its own, any other object - a vehicle - by its
    // place and facing.
    if (TransportManager* transports = owner_.getTransportManager(); transports && transports->getTransport(guid))
        return transports->getPlayerWorldPosition(guid, transports->serverToTransportLocal(guid, wirePoint));
    if (const auto object = owner_.getEntityManager().getEntity(guid)) {
        return spell_target_point::inObjectFrame(glm::vec3(object->getX(), object->getY(), object->getZ()),
                                                 object->getOrientation(), wirePoint);
    }
    return std::nullopt;
}

glm::vec3 SpellHandler::targetPointInWorld(uint64_t frameGuid, float x, float y, float z) {
    return spell_target_point::worldPoint(frameGuid, glm::vec3(x, y, z),
                                          [this](uint64_t guid, const glm::vec3& wire) {
                                              return pointInObjectFrame(guid, wire);
                                          });
}

void SpellHandler::triggerCastVisual(uint32_t spellId, uint64_t casterGuid, uint32_t castTimeMs) {
    LOG_INFO("SpellVisual: triggerCastVisual spellId=", spellId, " casterGuid=0x", std::hex, casterGuid, std::dec);
    auto* renderer = owner_.services().renderer;
    if (!renderer) { LOG_WARNING("SpellVisual: triggerCastVisual - no renderer"); return; }
    auto* svs = renderer->getSpellVisualSystem();
    if (!svs) { LOG_WARNING("SpellVisual: triggerCastVisual - no SpellVisualSystem"); return; }
    uint32_t visualId = resolveSpellVisualId(spellId);
    if (visualId == 0) { LOG_WARNING("SpellVisual: triggerCastVisual - visualId=0 for spellId=", spellId); return; }
    glm::vec3 casterPos;
    if (!resolveUnitPosition(casterGuid, casterPos)) { LOG_DEBUG("SpellVisual: triggerCastVisual - cannot resolve caster position for guid=0x", std::hex, casterGuid, std::dec); return; }
    LOG_INFO("SpellVisual: triggerCastVisual visualId=", visualId, " pos=(", casterPos.x, ",", casterPos.y, ",", casterPos.z, ") castTimeMs=", castTimeMs);
    svs->playSpellVisualPrecast(visualId, casterPos, castTimeMs,
                                owner_.resolveUnitRenderInstance(casterGuid), spellId);
}

void SpellHandler::triggerImpactVisual(uint32_t spellId, uint64_t targetGuid, uint64_t casterGuid) {
    LOG_INFO("SpellVisual: triggerImpactVisual spellId=", spellId, " targetGuid=0x", std::hex, targetGuid, std::dec);
    auto* renderer = owner_.services().renderer;
    if (!renderer) return;
    auto* svs = renderer->getSpellVisualSystem();
    if (!svs) return;
    uint32_t visualId = resolveSpellVisualId(spellId);
    if (visualId == 0) { LOG_WARNING("SpellVisual: triggerImpactVisual - visualId=0 for spellId=", spellId); return; }
    glm::vec3 targetPos;
    if (!resolveUnitPosition(targetGuid, targetPos)) return;
    LOG_INFO("SpellVisual: triggerImpactVisual visualId=", visualId, " pos=(", targetPos.x, ",", targetPos.y, ",", targetPos.z, ")");
    // 0x00801f10: the CasterImpactKit on the caster, else the TargetImpactKit.
    svs->playSpellVisual(visualId, targetPos, /*useImpactKit=*/true,
                         owner_.resolveUnitRenderInstance(targetGuid), targetGuid == casterGuid, spellId);
}

void SpellHandler::launchRangedWeaponProjectile(uint32_t spellId, uint64_t targetGuid) {
    if (targetGuid == 0) targetGuid = owner_.getTargetGuid();
    auto* renderer = owner_.services().renderer;
    auto* assets = owner_.services().assetManager;
    if (!renderer || !assets || targetGuid == 0) return;
    auto* visuals = renderer->getSpellVisualSystem();
    auto* characters = renderer->getCharacterRenderer();
    if (!visuals || !characters) return;

    glm::vec3 start = renderer->getCharacterPosition() + glm::vec3(0.0f, 0.0f, 1.0f);
    glm::mat4 handTransform(1.0f);
    if (characters->getAttachmentTransform(renderer->getCharacterInstanceId(), 1, handTransform))
        start = glm::vec3(handTransform[3]);

    glm::vec3 end;
    if (!resolveUnitPosition(targetGuid, end)) return;
    end.z += 1.0f;

    const auto& ranged = owner_.getInventory().getEquipSlot(EquipSlot::RANGED);
    std::string modelPath;
    std::string texturePath;
    bool spin = false;

    if (spellId == 2764 || (!ranged.empty() && ranged.item.inventoryType == InvType::THROWN)) {
        spin = true;
        if (!ranged.empty() && ranged.item.displayInfoId != 0) {
            auto displayDbc = assets->loadDBC("ItemDisplayInfo.dbc");
            if (displayDbc && displayDbc->isLoaded()) {
                const int32_t row = displayDbc->findRecordById(ranged.item.displayInfoId);
                if (row >= 0) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                    std::string model = displayDbc->getString(
                        static_cast<uint32_t>(row), layout ? (*layout)["LeftModel"] : 1);
                    std::string texture = displayDbc->getString(
                        static_cast<uint32_t>(row), layout ? (*layout)["LeftModelTexture"] : 3);
                    if (!model.empty()) {
                        const size_t dot = model.rfind('.');
                        if (dot != std::string::npos) model = model.substr(0, dot);
                        modelPath = "Item\\ObjectComponents\\Weapon\\" + model + ".m2";
                    }
                    if (!texture.empty())
                        texturePath = "Item\\ObjectComponents\\Weapon\\" + texture + ".blp";
                }
            }
        }
        if (modelPath.empty())
            modelPath = "Item\\ObjectComponents\\Weapon\\Thrown_1H_Dagger_A_01.m2";
    } else if (spellId == 75 || (!ranged.empty() &&
               (ranged.item.inventoryType == InvType::RANGED_BOW ||
                ranged.item.subclassName == "Bow" || ranged.item.subclassName == "Crossbow"))) {
        modelPath = "Item\\ObjectComponents\\Ammo\\ArrowFlight_01.m2";
    } else {
        modelPath = "Item\\ObjectComponents\\Ammo\\BulletFlight_01.m2";
    }

    const float distance = glm::length(end - start);
    const float duration = std::clamp(distance / 35.0f, 0.12f, 0.8f);
    visuals->playPhysicalProjectile(modelPath, texturePath, start, end, duration, spin);
}


// FUN_007fffb0 and FUN_00732ff0: a spell with a speed flies its visual's
// missile from the caster to each target it was cast at - hit or missed - or,
// cast at the ground, one to the point, from the source point when the cast
// names one too. The impact kit waits for the missile: on the target it flew
// to, or for a ground missile on everything it hit (FUN_00700c80 hands the
// hit list to it) and on the point itself (0x00700e20).
std::vector<uint64_t> SpellHandler::launchSpellMissiles(const SpellGoData& data, uint32_t visualId,
                                                       bool* flewToPlace) {
    namespace sm = rendering::spell_missile;
    using MissileEnd = rendering::SpellVisualSystem::MissileEnd;
    std::vector<uint64_t> carried;
    auto* renderer = owner_.services().renderer;
    auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr;
    if (!svs) return carried;
    owner_.loadSpellNameCache();
    auto spellIt = owner_.spellNameCacheRef().find(data.spellId);
    const float speed = spellIt != owner_.spellNameCacheRef().end() ? spellIt->second.missileSpeed : 0.0f;
    if (!(speed > 0.0f)) return carried;
    const auto* missile = svs->findMissileVisual(visualId);
    if (!missile) return carried;

    // ADJUST_MISSILE (cast flag 0x20000): every missile of the cast flies the
    // arc the packet's elevation and delay give it (0x00732ff0 copies them
    // onto the missile, 0x00700880 plans it).
    rendering::SpellVisualSystem::MissileTrajectory trajectoryData;
    const rendering::SpellVisualSystem::MissileTrajectory* trajectory = nullptr;
    if ((data.castFlags & 0x20000u) && data.hasMissileTrajectory) {
        trajectoryData.elevation = data.missileElevation;
        trajectoryData.flightSeconds = static_cast<float>(data.missileDelayMs) * 0.001f;
        trajectoryData.spellMissileId = spellIt->second.spellMissileId;
        trajectory = &trajectoryData;
    }

    MissileEnd from;
    if (!resolveUnitPosition(data.casterUnit, from.position)) return carried;
    from.renderInstanceId = owner_.resolveUnitRenderInstance(data.casterUnit);
    auto unitEnd = [&](uint64_t guid, MissileEnd& end) {
        if (guid == 0 || !resolveUnitPosition(guid, end.position)) return false;
        end.renderInstanceId = owner_.resolveUnitRenderInstance(guid);
        return true;
    };

    const size_t targetCount = data.hitTargets.size() + data.missTargets.size();
    switch (sm::chooseTargeting(data.targetFlags, targetCount, missile->flags)) {
    case sm::Targeting::EachTarget: {
        for (uint64_t guid : data.hitTargets) {
            MissileEnd to;
            if (unitEnd(guid, to) && svs->launchSpellMissile(visualId, speed, from, to, {to}, trajectory,
                                                             data.spellId, data.casterUnit, data.castCount))
                carried.push_back(guid);
        }
        for (const auto& miss : data.missTargets) {
            MissileEnd to;
            if (unitEnd(miss.targetGuid, to))
                svs->launchSpellMissile(visualId, speed, from, to, {}, trajectory, data.spellId, data.casterUnit,
                                        data.castCount);
        }
        break;
    }
    case sm::Targeting::Location: {
        // The wire's points (0x00732ff0): the one it flies to in the frame
        // of its transport guid; with both, the source is where it starts,
        // taken as a place in the world.
        const auto point = [](const glm::vec3& canonical) {
            return MissileEnd{0, core::coords::canonicalToRender(canonical)};
        };
        MissileEnd to;
        if (data.hasDestLocation) {
            to = point(targetPointInWorld(data.destTransportGuid, data.destX, data.destY, data.destZ));
            if (data.hasSourceLocation)
                from = point(core::coords::serverToCanonical(glm::vec3(data.sourceX, data.sourceY, data.sourceZ)));
        } else if (data.hasSourceLocation) {
            to = point(targetPointInWorld(data.sourceTransportGuid, data.sourceX, data.sourceY, data.sourceZ));
        } else {
            break;
        }
        std::vector<MissileEnd> impacts;
        for (uint64_t guid : data.hitTargets) {
            MissileEnd end;
            if (unitEnd(guid, end)) impacts.push_back(end);
        }
        // 0x00700e20: and at the point it flies to, whatever it hit.
        impacts.push_back(to);
        if (svs->launchSpellMissile(visualId, speed, from, to, std::move(impacts), trajectory, data.spellId,
                                    data.casterUnit, data.castCount)) {
            carried = data.hitTargets;
            if (flewToPlace) *flewToPlace = true;
        }
        break;
    }
    case sm::Targeting::None:
        break;
    }
    return carried;
}

void SpellHandler::rememberDestLocationCount(uint64_t caster, uint8_t counter) {
    const int64_t now = historyNowMs();
    // 0x0080e100 keeps it ten seconds.
    std::erase_if(destLocationCounts_, [now](const auto& e) { return e.second.expiresMs <= now; });
    destLocationCounts_[caster] = {counter, now + 10000};
}

void SpellHandler::handleNotifyDestLocSpellCast(network::Packet& packet) {
    NotifyDestLocSpellCastData d;
    if (!parseNotifyDestLocSpellCast(packet, d)) {
        packet.skipAll();
        return;
    }
    // 0x00810050: a caster's count no newer than the one kept is a cast
    // already played; a newer one is kept. A cast with no caster is played.
    if (d.caster != 0) {
        const auto it = destLocationCounts_.find(d.caster);
        if (it != destLocationCounts_.end() && it->second.expiresMs > historyNowMs() &&
            static_cast<int8_t>(d.counter - it->second.counter) < 1)
            return;
        rememberDestLocationCount(d.caster, d.counter);
    }
    const uint32_t visualId = resolveSpellVisualId(d.spellId);
    auto* renderer = owner_.services().renderer;
    auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr;
    if (visualId == 0 || !svs) return;
    const auto place = [](const float* p) {
        return core::coords::canonicalToRender(core::coords::serverToCanonical(glm::vec3(p[0], p[1], p[2])));
    };
    const glm::vec3 to = place(d.dest);
    // 0x008022d0: a visual with a missile and a spell with a speed fly the
    // missile from the source to the place - on an arc where the packet
    // gives a speed of its own, landing its delay from now - and the missile
    // carries the ImpactAreaKit there.
    loadSpellNameCache();
    const auto spellIt = owner_.spellNameCacheRef().find(d.spellId);
    const float speed = spellIt != owner_.spellNameCacheRef().end() ? spellIt->second.missileSpeed : 0.0f;
    bool flew = false;
    if (speed > 0.0f && svs->findMissileVisual(visualId)) {
        using SVS = rendering::SpellVisualSystem;
        SVS::MissileTrajectory trajectory;
        const bool onArc = d.speed > 0.0f;
        if (onArc) {
            trajectory.elevation = d.elevation;
            trajectory.castSpeed = d.speed;
            trajectory.flightSeconds = static_cast<float>(d.delayMs) * 0.001f;
            trajectory.spellMissileId = spellIt->second.spellMissileId;
        }
        // Aimed at the place alone (no target, +8 cleared), so no unit's
        // impact kit waits for it.
        flew = svs->launchSpellMissile(visualId, speed, SVS::MissileEnd{0, place(d.source)}, SVS::MissileEnd{0, to},
                                       {}, onArc ? &trajectory : nullptr, d.spellId, d.caster, d.castCount);
    }
    // The InstantAreaKit now; the ImpactAreaKit the delay from now where no
    // missile carries it.
    svs->playDestinationAreaKits(visualId, to, flew, d.spellId, d.casterObject, d.delayMs);
}

SpellHandler::SpellHandler(GameHandler& owner)
    : owner_(owner) {}

// The five values FrameXML unpacks from any UNIT_SPELLCAST_* event:
//   unit, spell name, rank, cast id, spell id
//
// Every one of these was fired as the unit and the spell id alone, which put
// the id where the name belongs and left the cast id absent. Two things went
// wrong with that.
//
// The cast bar takes its own cast id from UnitCastingInfo - which answers the
// spell id for it - and then, on STOP, compares `select(4, ...)` against it
// before finishing the bar. That argument was nil, so the comparison was false
// every time and the branch that flashes the bar, fills it and clears
// self.casting never ran. UNIT_SPELLCAST_FAILED and _INTERRUPTED check the
// same argument the same way.
//
// And CastRandomManager_OnEvent reads `local unit, name, rank = ...` then calls
// strlower on both, so a /castsequence or /castrandom macro raised on a nil
// rank the first time any spell of the player's succeeded.
//
// The cast id is the spell id here, which is what UnitCastingInfo reports, so
// the two agree. It is not what the server calls a cast id - this client does
// not keep the counter - but nothing compares it to anything else.
std::vector<std::string> SpellHandler::spellcastArgs(const std::string& unitId,
                                                     uint32_t spellId) const {
    const std::string& name = owner_.getSpellName(spellId);
    const std::string& rank = owner_.getSpellRank(spellId);
    // A name that is not cached yet would be empty, and an empty string is a
    // string - which is what the readers want. Nil would raise in strlower.
    return {unitId, name, rank, std::to_string(spellId), std::to_string(spellId)};
}

void SpellHandler::requestPetName(uint64_t petGuid) {
    if (petGuid == 0 || !owner_.getSocket()) return;
    if (owner_.getState() != WorldState::IN_WORLD) return;
    const uint32_t key = nextPetNameQueryKey_++;
    pendingPetNameQueries_[key] = petGuid;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_NAME_QUERY));
    pkt.writeUInt32(key);
    pkt.writeUInt64(petGuid);
    owner_.getSocket()->send(pkt);
}

void SpellHandler::registerOpcodes(DispatchTable& table) {
    // The reply, which was read to the end and thrown away - a skip handler
    // for a message nothing had asked for, since nothing sent the request.
    //
    // Shape from AzerothCore's SendPetNameQuery: the key back, the name, and
    // the timestamp the name was set. A pet it could not find answers a zero
    // byte where the name would be, which is the empty string and means "keep
    // what you have" rather than "the pet has no name".
    table[Opcode::SMSG_PET_NAME_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
        const uint32_t key = packet.readUInt32();
        const std::string name = packet.readString();
        packet.skipAll();

        auto it = pendingPetNameQueries_.find(key);
        if (it == pendingPetNameQueries_.end()) return;
        const uint64_t guid = it->second;
        pendingPetNameQueries_.erase(it);
        owner_.combatLogFeed().petNameAnswered(guid, name, !name.empty());
        if (name.empty()) return;

        auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity) return;
        auto unit = std::dynamic_pointer_cast<Unit>(entity);
        if (!unit) return;
        unit->setName(name);
        LOG_INFO("Pet name: 0x", std::hex, guid, std::dec, " is '", name, "'");
        // Both, because the two frames listen for different ones: the pet
        // frame redraws its name from UNIT_NAME_UPDATE, and the pet bar and
        // anything else keyed on the pet rebuild from PET_UI_UPDATE.
        if (guid == owner_.petGuidRef()) {
            owner_.fireAddonEvent("UNIT_NAME_UPDATE", {"pet"});
            owner_.fireAddonEvent("PET_UI_UPDATE", {});
        }
    };

    table[Opcode::SMSG_INITIAL_SPELLS] = [this](network::Packet& packet) { handleInitialSpells(packet); };
    table[Opcode::SMSG_CAST_FAILED] = [this](network::Packet& packet) { handleCastFailed(packet); };
    table[Opcode::SMSG_SPELL_START] = [this](network::Packet& packet) { handleSpellStart(packet); };
    table[Opcode::SMSG_SPELL_GO] = [this](network::Packet& packet) { handleSpellGo(packet); };
    table[Opcode::SMSG_NOTIFY_DEST_LOC_SPELL_CAST] = [this](network::Packet& packet) {
        handleNotifyDestLocSpellCast(packet);
    };
    // 0x008005a0: the missiles of a cast stop at a point.
    table[Opcode::SMSG_SET_PROJECTILE_POSITION] = [this](network::Packet& packet) {
        ProjectilePositionData d;
        if (!parseProjectilePosition(packet, d)) {
            packet.skipAll();
            return;
        }
        auto* renderer = owner_.services().renderer;
        if (auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr)
            svs->setProjectilePosition(d.caster, d.castCount,
                                       core::coords::canonicalToRender(
                                           core::coords::serverToCanonical(glm::vec3(d.x, d.y, d.z))));
    };
    table[Opcode::SMSG_SPELL_UPDATE_CHAIN_TARGETS] = [this](network::Packet& packet) {
        handleSpellUpdateChainTargets(packet);
    };
    table[Opcode::SMSG_SPELL_COOLDOWN] = [this](network::Packet& packet) { handleSpellCooldown(packet); };
    table[Opcode::SMSG_COOLDOWN_EVENT] = [this](network::Packet& packet) { handleCooldownEvent(packet); };
    table[Opcode::SMSG_AURA_UPDATE] = [this](network::Packet& packet) {
        handleAuraUpdate(packet, false);
    };
    table[Opcode::SMSG_AURA_UPDATE_ALL] = [this](network::Packet& packet) {
        handleAuraUpdate(packet, true);
    };
    table[Opcode::SMSG_LEARNED_SPELL] = [this](network::Packet& packet) { handleLearnedSpell(packet); };
    table[Opcode::SMSG_SUPERCEDED_SPELL] = [this](network::Packet& packet) { handleSupercededSpell(packet); };
    table[Opcode::SMSG_REMOVED_SPELL] = [this](network::Packet& packet) { handleRemovedSpell(packet); };
    table[Opcode::SMSG_SEND_UNLEARN_SPELLS] = [this](network::Packet& packet) { handleUnlearnSpells(packet); };
    table[Opcode::SMSG_TALENTS_INFO] = [this](network::Packet& packet) { handleTalentsInfo(packet); };
    // Server asks the player to confirm a talent reset (guid + gold cost).
    // Must be handled here: this handler owns the pending-wipe state that the
    // confirm dialog reads.
    table[Opcode::MSG_TALENT_WIPE_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        talentWipeNpcGuid_ = packet.readUInt64();
        talentWipeCost_    = packet.readUInt32();
        talentWipePending_ = true;
        LOG_INFO("MSG_TALENT_WIPE_CONFIRM: npc=0x", std::hex, talentWipeNpcGuid_, std::dec,
                 " cost=", talentWipeCost_);
        owner_.fireAddonEvent("CONFIRM_TALENT_WIPE", {std::to_string(talentWipeCost_)});
    };
    // SMSG_PET_UNLEARN_CONFIRM: uint64 petGuid + uint32 cost (copper). Handled
    // here for the same reason - this handler owns the pending-unlearn state.
    // The other pet opcodes have different formats and must NOT set unlearn state.
    table[Opcode::SMSG_PET_UNLEARN_CONFIRM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            packet.readUInt64();   // the pet's guid; the cost is what is asked for
            petUnlearnCost_ = packet.readUInt32();
            petUnlearnPending_ = true;
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_ACHIEVEMENT_EARNED] = [this](network::Packet& packet) {
        handleAchievementEarned(packet);
    };
    // SMSG_EQUIPMENT_SET_LIST - owned by InventoryHandler::registerOpcodes

    // ---- Cast result / spell visuals / cooldowns / modifiers ----
    table[Opcode::SMSG_CAST_RESULT] = [this](network::Packet& p) { handleCastResult(p); };
    table[Opcode::SMSG_SPELL_FAILED_OTHER] = [this](network::Packet& p) { handleSpellFailedOther(p); };
    table[Opcode::SMSG_CLEAR_COOLDOWN] = [this](network::Packet& p) { handleClearCooldown(p); };
    table[Opcode::SMSG_MODIFY_COOLDOWN] = [this](network::Packet& p) { handleModifyCooldown(p); };
    // 0x00804110: the player's history, or the pet's, emptied (0x00802a50).
    table[Opcode::SMSG_COOLDOWN_CHEAT] = [this](network::Packet& p) {
        if (!p.hasRemaining(8)) return;
        const uint64_t guid = p.readUInt64();
        if (guid == owner_.getPlayerGuid()) playerHistory_.clear();
        else if (guid != 0 && guid == owner_.getPetGuid()) petHistory_.clear();
        playerCooldownsChanged();
    };
    table[Opcode::SMSG_PLAY_SPELL_VISUAL] = [this](network::Packet& p) { handlePlaySpellVisual(p); };
    table[Opcode::SMSG_SET_FLAT_SPELL_MODIFIER] = [this](network::Packet& p) { handleSpellModifier(p, true); };
    table[Opcode::SMSG_SET_PCT_SPELL_MODIFIER]  = [this](network::Packet& p) { handleSpellModifier(p, false); };
    table[Opcode::SMSG_SPELL_DELAYED] = [this](network::Packet& p) { handleSpellDelayed(p); };

    // ---- Spell log / aura / dispel / totem / channel handlers ----
    table[Opcode::SMSG_SPELLLOGMISS] = [this](network::Packet& p) { handleSpellLogMiss(p); };
    table[Opcode::SMSG_SPELL_FAILURE] = [this](network::Packet& p) { handleSpellFailure(p); };
    table[Opcode::SMSG_ITEM_COOLDOWN] = [this](network::Packet& p) { handleItemCooldown(p); };
    table[Opcode::SMSG_DISPEL_FAILED] = [this](network::Packet& p) { handleDispelFailed(p); };
    table[Opcode::SMSG_TOTEM_CREATED] = [this](network::Packet& p) { handleTotemCreated(p); };
    table[Opcode::SMSG_PERIODICAURALOG] = [this](network::Packet& p) { handlePeriodicAuraLog(p); };
    table[Opcode::SMSG_SPELLENERGIZELOG] = [this](network::Packet& p) { handleSpellEnergizeLog(p); };
    table[Opcode::SMSG_INIT_EXTRA_AURA_INFO_OBSOLETE] = [this](network::Packet& p) { handleExtraAuraInfo(p, true); };
    table[Opcode::SMSG_SET_EXTRA_AURA_INFO_OBSOLETE] = [this](network::Packet& p) { handleExtraAuraInfo(p, false); };
    table[Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE] = [this](network::Packet& p) { handleExtraAuraInfo(p, false); };
    table[Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE_OBSOLETE] = [this](network::Packet& p) { handleExtraAuraInfo(p, false); };
    table[Opcode::SMSG_SPELLDISPELLOG] = [this](network::Packet& p) { handleSpellDispelLog(p); };
    table[Opcode::SMSG_SPELLSTEALLOG] = [this](network::Packet& p) { handleSpellStealLog(p); };
    table[Opcode::SMSG_SPELLBREAKLOG] = [this](network::Packet& p) { handleSpellBreakLog(p); };
    table[Opcode::SMSG_SPELL_CHANCE_PROC_LOG] = [this](network::Packet& p) { handleSpellChanceProcLog(p); };
    table[Opcode::SMSG_SPELLINSTAKILLLOG] = [this](network::Packet& p) { handleSpellInstaKillLog(p); };
    table[Opcode::SMSG_SPELLLOGEXECUTE] = [this](network::Packet& p) { handleSpellLogExecute(p); };
    table[Opcode::SMSG_CLEAR_EXTRA_AURA_INFO] = [this](network::Packet& p) { handleClearExtraAuraInfo(p); };
    table[Opcode::SMSG_CLEAR_EXTRA_AURA_INFO_OBSOLETE] = [this](network::Packet& p) { handleClearExtraAuraInfo(p); };
    table[Opcode::SMSG_ITEM_ENCHANT_TIME_UPDATE] = [this](network::Packet& p) { handleItemEnchantTimeUpdate(p); };
    table[Opcode::SMSG_RESUME_CAST_BAR] = [this](network::Packet& p) { handleResumeCastBar(p); };
    table[Opcode::MSG_CHANNEL_START] = [this](network::Packet& p) { handleChannelStart(p); };
    table[Opcode::MSG_CHANNEL_UPDATE] = [this](network::Packet& p) { handleChannelUpdate(p); };
}

// ============================================================
// Public API
// ============================================================

bool SpellHandler::isGameObjectInteractionCasting() const {
    return casting_ && currentCastSpellId_ == 0 && owner_.pendingGameObjectInteractGuidRef() != 0;
}

bool SpellHandler::isTargetCasting() const {
    return getUnitCastState(owner_.getTargetGuid()) != nullptr;
}

uint32_t SpellHandler::getTargetCastSpellId() const {
    auto* s = getUnitCastState(owner_.getTargetGuid());
    return s ? s->spellId : 0;
}

float SpellHandler::getTargetCastProgress() const {
    auto* s = getUnitCastState(owner_.getTargetGuid());
    return (s && s->timeTotal > 0.0f)
        ? (s->timeTotal - s->timeRemaining) / s->timeTotal : 0.0f;
}

float SpellHandler::getTargetCastTimeRemaining() const {
    auto* s = getUnitCastState(owner_.getTargetGuid());
    return s ? s->timeRemaining : 0.0f;
}

bool SpellHandler::isTargetCastInterruptible() const {
    auto* s = getUnitCastState(owner_.getTargetGuid());
    return s ? s->interruptible : true;
}

bool SpellHandler::armGroundTarget(uint32_t spellId, uint64_t caster) {
    // A spell that wants a place on the ground (Spell.dbc Targets 0x20/0x40,
    // 0x0080c790) is not sent: the cursor holds it until the world is
    // clicked (0x0080c340) or it is put down (SpellStopTargeting). A second
    // such spell replaces the first.
    if (spellId == 0) return false;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return false;
    // TARGET_DEST_TRAJ's exception reads the spell's SpellMissile
    // row, its Flags (+4) bit 1 (0x0080c790 case 0x59).
    bool trajectory = false;
    if (it->second.implicitTargetsA[0] == 89 && it->second.spellMissileId != 0) {
        auto* am = core::Application::getInstance().getAssetManager();
        auto missiles = am ? am->loadDBCOptional("SpellMissile.dbc") : nullptr;
        const int32_t row = missiles && missiles->getFieldCount() > 1
                                ? missiles->findRecordById(it->second.spellMissileId) : -1;
        trajectory = row >= 0 && (missiles->getUInt32(static_cast<uint32_t>(row), 1) & 1u) != 0;
    }
    const uint32_t required = ground_target::requiredTargets(
        it->second.targetFlags, it->second.implicitTargetsA[0], trajectory);
    if (!ground_target::wantsLocation(required)) return false;
    if (owner_.isAwaitingItemTarget()) owner_.cancelItemTargeting();
    owner_.cancelGlyphTargeting();
    unitTarget_ = UnitTarget{};
    groundTarget_ = GroundTarget{};
    groundTarget_.spellId = spellId;
    groundTarget_.caster = caster;
    groundTarget_.required = required &
        (ground_target::kTargetFlagSourceLocation | ground_target::kTargetFlagDestLocation);
    // The first effect that summons a game object shows its
    // model at the place (0x0080cce0).
    for (int k = 0; k < 3; ++k) {
        if (!ground_target::summonsObject(it->second.effectIds[k])) continue;
        groundTarget_.objectEntry = it->second.effectMiscValues[k];
        groundTarget_.objectFixedFacing = it->second.effectIds[k] == ground_target::kEffectObjectFixedFacing;
        if (groundTarget_.objectEntry != 0) owner_.queryGameObjectInfo(groundTarget_.objectEntry, 0);
        break;
    }
    LOG_INFO("Ground targeting: spell ", spellId, " waits for a place (targets 0x",
             std::hex, it->second.targetFlags, std::dec, ")");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURSOR_UPDATE", {});
    return true;
}

namespace {

uint64_t entityGuidField(const Entity& e, UF low) {
    const uint16_t idx = fieldIndex(low);
    if (idx == 0xFFFF) return 0;
    return e.getField(idx) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32);
}

/// Spell.dbc as 0x00739650 reads it (cast_check::Spell).
cast_check::Spell checkSpellOf(const GameHandler::SpellNameEntry& e) {
    cast_check::Spell s;
    s.attr = e.attr;
    s.attrEx = e.attrEx;
    s.attrEx2 = e.attrEx2;
    s.attrEx3 = e.attrEx3;
    s.attrEx4 = e.attrEx4;
    s.attrEx5 = e.attrEx5;
    s.attrEx6 = e.attrEx6;
    s.attrEx7 = e.attrEx7;
    s.dispel = e.dispelType;
    s.mechanic = e.mechanic;
    s.schoolMask = e.schoolMask;
    for (int w = 0; w < 2; ++w) {
        s.stances[w] = e.stances[w];
        s.stancesNot[w] = e.stancesNot[w];
    }
    s.targets = e.targetFlags;
    s.casterAuraState = e.casterAuraState;
    s.casterAuraStateNot = e.casterAuraStateNot;
    s.casterAuraSpell = e.casterAuraSpell;
    s.excludeCasterAuraSpell = e.excludeCasterAuraSpell;
    s.recoveryTime = static_cast<int32_t>(e.recoveryMs);
    s.categoryRecoveryTime = static_cast<int32_t>(e.categoryRecoveryMs);
    s.interruptFlags = e.interruptFlags;
    s.auraInterruptFlags = e.auraInterruptFlags;
    s.channelInterruptFlags = e.channelInterruptFlags;
    for (int k = 0; k < 3; ++k) {
        s.effect[k] = e.effectIds[k];
        s.effectMechanic[k] = e.effectMechanic[k];
        s.implicitA[k] = e.implicitTargetsA[k];
        s.implicitB[k] = e.implicitTargetsB[k];
        s.aura[k] = e.effectAuraIds[k];
        s.misc[k] = e.effectMiscValues[k];
        s.trigger[k] = e.effectTriggerSpell[k];
    }
    s.preventionType = e.preventionType;
    s.minFactionId = e.minFactionId;
    s.minReputation = e.minReputation;
    s.areaGroupId = e.areaGroupId;
    return s;
}

uint32_t entityField(const Entity& e, UF f) {
    const uint16_t idx = fieldIndex(f);
    return idx != 0xFFFF ? e.getField(idx) : 0u;
}

/// UNIT_MOD_CAST_SPEED (+0x128) as a factor; 1 where none was sent.
float castSpeedOf(const Entity& e) {
    const uint32_t raw = entityField(e, UF::UNIT_MOD_CAST_SPEED);
    return raw != 0 ? std::bit_cast<float>(raw) : 1.0f;
}

/// 0x00718b70: the player behind a unit - itself, else its charmer (else
/// creator), else that one's.
std::shared_ptr<Entity> controllingPlayer(GameHandler& gh, const std::shared_ptr<Entity>& e) {
    const auto up = [](const Entity& x) {
        const uint64_t c = entityGuidField(x, UF::UNIT_FIELD_CHARMEDBY);
        return c != 0 ? c : entityGuidField(x, UF::UNIT_FIELD_CREATEDBY);
    };
    std::shared_ptr<Entity> u = e;
    if (const uint64_t g = up(*e); g != 0) u = gh.getEntityManager().getEntity(g);
    if (!u) return nullptr;
    if (u->getType() == ObjectType::PLAYER) return u;
    const uint64_t g2 = up(*u);
    auto v = g2 != 0 ? gh.getEntityManager().getEntity(g2) : nullptr;
    return v && v->getType() == ObjectType::PLAYER ? v : nullptr;
}

/// 0x0052c680: the player or one of their party - in a raid, their
/// subgroup; 0x00512a00 adds the raid's members.
bool inPlayersParty(GameHandler& gh, uint64_t guid, bool orRaid) {
    if (guid == 0) return false;
    if (guid == gh.getPlayerGuid()) return true;
    const auto& pd = gh.getPartyData();
    const bool raid = pd.groupType == 1;
    for (const auto& m : pd.members) {
        if (m.guid == guid && (!raid || orRaid || m.subGroup == pd.subGroup)) return true;
    }
    return false;
}

unit_target::Side relationSide(GameHandler& gh, const Entity& e) {
    unit_target::Side s;
    s.isPlayer = e.getType() == ObjectType::PLAYER;
    s.unitFlags = entityField(e, UF::UNIT_FIELD_FLAGS);
    s.unitFlags2 = entityField(e, UF::UNIT_FIELD_FLAGS_2);
    s.pvpFlags = unitPvpFlags(!isPreWotlk() && fieldIndex(UF::UNIT_FIELD_BYTES_2) != 0xFFFF,
                              entityField(e, UF::UNIT_FIELD_BYTES_2), s.unitFlags);
    if (s.isPlayer) {
        s.playerFlags = entityField(e, UF::PLAYER_FLAGS);
    } else if (e.isUnit()) {
        const auto& cache = gh.getCreatureInfoCache();
        if (auto it = cache.find(static_cast<const Unit&>(e).getEntry()); it != cache.end())
            s.creatureTypeFlags = it->second.typeFlags;
    }
    if (const auto* row = gh.vehicleRowOf(e.getGuid()); row && (row->flags & 0x20000000u) != 0) {
        const auto known = [&gh](uint64_t g) { return gh.getEntityManager().getEntity(g) != nullptr; };
        s.vehicleOutOfReach = gh.vehicleRoster().rootVehicle(e.getGuid(), 0, known) == e.getGuid();
    }
    return s;
}

/// The caster's relations to the unit as 0x0080bc80 asks them.
unit_target::Relations relationsOf(GameHandler& gh, const std::shared_ptr<Entity>& caster,
                                   const std::shared_ptr<Entity>& unit, uint32_t attrEx6) {
    unit_target::Relations r;
    if (!unit->isUnit()) return r;
    const auto cs = relationSide(gh, *caster);
    const auto us = relationSide(gh, *unit);
    unit_target::Controllers c;
    const auto c1 = controllingPlayer(gh, caster);
    const auto c2 = controllingPlayer(gh, unit);
    if (c1 && c2) {
        c.found = true;
        c.casterArbiter = entityGuidField(*c1, UF::PLAYER_DUEL_ARBITER);
        c.unitArbiter = entityGuidField(*c2, UF::PLAYER_DUEL_ARBITER);
        c.casterTeam = entityField(*c1, UF::PLAYER_DUEL_TEAM);
        c.unitTeam = entityField(*c2, UF::PLAYER_DUEL_TEAM);
    }
    // 0x007251c0 both ways round.
    const int reaction = gh.unitReaction(*caster, *unit);
    const int back = gh.unitReaction(*unit, *caster);
    r.mayAssist = unit_target::mayAssist(cs, us, reaction, false, c);
    r.mayAssistEx6 = unit_target::mayAssist(cs, us, reaction, (attrEx6 & 0x8u) != 0, c);
    r.mayAttack = unit_target::mayAttack(cs, us, reaction, back, c);
    // 0x00718ca0 / 0x00718d70: itself; between two the player controls,
    // their players in the player's party (or raid); for the raid also a
    // creature flagged 0x4000000.
    const bool same = unit->getGuid() == caster->getGuid();
    const bool bothPc = (cs.unitFlags & unit_target::kFlagPlayerControlled) != 0 &&
                        (us.unitFlags & unit_target::kFlagPlayerControlled) != 0;
    const auto grouped = [&](bool orRaid) {
        if (!bothPc || !c1 || !c2) return false;
        const uint64_t me = gh.getPlayerGuid();
        return (c1->getGuid() == me && inPlayersParty(gh, c2->getGuid(), orRaid)) ||
               (c2->getGuid() == me && inPlayersParty(gh, c1->getGuid(), orRaid));
    };
    r.inParty = same || grouped(false);
    r.inRaid = same || (us.creatureTypeFlags & 0x4000000u) != 0 || grouped(true);
    return r;
}

/// 0x007ff480's moving unit: forward, sideways or falling (0x100d) and
/// faster than twice its walk speed (0x00716fa0, 0x00987570). The mover's
/// are the player's own; another's those its last packet or block brought.
bool movingForReach(GameHandler& gh, const Entity& e) {
    if (e.getGuid() == gh.activeMover()) {
        const uint32_t flags = gh.getMovementInfo().flags;
        const MovementSpeeds speeds = gh.moverSpeeds();
        return (flags & 0x100du) != 0 && Entity::speedForFlags(speeds, flags) > 2.0f * speeds.walk;
    }
    return (e.getReportedMoveFlags() & 0x100du) != 0 && e.currentSpeed() > 2.0f * e.getMovementSpeeds().walk;
}

/// 0x0071f300: a unit's creature type - its form's, else its creature's,
/// else its race's (ChrRaces CreatureType, +0x20).
uint32_t creatureTypeOf(GameHandler& gh, const Entity& e) {
    const uint32_t bytes2 = entityField(e, UF::UNIT_FIELD_BYTES_2);
    if (const auto* form = !isPreWotlk() ? gh.shapeshiftFormRow(static_cast<uint8_t>(bytes2 >> 24)) : nullptr;
        form && static_cast<int32_t>(form->creatureType) > 0)
        return form->creatureType;
    if (e.getType() != ObjectType::PLAYER) {
        return e.isUnit() ? gh.getCreatureType(static_cast<const Unit&>(e).getEntry()) : 0u;
    }
    const uint32_t race = entityField(e, UF::UNIT_FIELD_BYTES_0) & 0xFFu;
    auto* am = core::Application::getInstance().getAssetManager();
    auto races = am ? am->loadDBCOptional("ChrRaces.dbc") : nullptr;
    const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ChrRaces") : nullptr;
    const uint32_t field = layout ? layout->tryField("CreatureType") : 0xFFFFFFFF;
    const int32_t row = races && field < races->getFieldCount() ? races->findRecordById(race) : -1;
    return row >= 0 ? races->getUInt32(static_cast<uint32_t>(row), field) : 0u;
}

}  // namespace

bool SpellHandler::unitMayAttack(uint64_t attacker, uint64_t victim) const {
    auto& em = owner_.getEntityManager();
    const auto a = em.getEntity(attacker);
    const auto b = em.getEntity(victim);
    return a && b && a->isUnit() && b->isUnit() && relationsOf(owner_, a, b, 0).mayAttack;
}

bool SpellHandler::unitMayAssist(uint64_t helper, uint64_t unit) const {
    auto& em = owner_.getEntityManager();
    const auto a = em.getEntity(helper);
    const auto b = em.getEntity(unit);
    return a && b && a->isUnit() && b->isUnit() && relationsOf(owner_, a, b, 0).mayAssist;
}

SpellHandler::UnitTake SpellHandler::takeUnit(uint32_t spellId, uint64_t casterGuid, uint64_t unitGuid,
                                              uint32_t required, bool say) const {
    UnitTake out{false, required, 0, false};
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end() || unitGuid == 0) return out;
    auto& em = owner_.getEntityManager();
    const auto caster = em.getEntity(casterGuid);
    auto unit = em.getEntity(unitGuid);
    if (!caster || !unit) return out;
    const auto& e = it->second;
    if ((e.attrEx5 & 0x800u) != 0 && unit->isUnit()) {
        // 0x007ffc40: the unit, or its own target in its place.
        const uint64_t itsGuid = entityGuidField(*unit, UF::UNIT_FIELD_TARGET_LO);
        auto its = itsGuid != 0 ? em.getEntity(itsGuid) : nullptr;
        if (its && !its->isUnit()) its = nullptr;
        const auto pick = unit_target::retarget(required, relationsOf(owner_, caster, unit, e.attrEx6), its != nullptr,
                                                its ? relationsOf(owner_, caster, its, e.attrEx6)
                                                    : unit_target::Relations{});
        if (pick == unit_target::Retarget::None) return out;
        if (pick == unit_target::Retarget::ItsTarget) {
            unit = its;
            unitGuid = itsGuid;
        }
        required = unit_target::retargetRequired(required);
    }
    const unit_target::Spell s{e.attrEx, e.attrEx2, e.attrEx3, e.attrEx5, e.attrEx6, e.targetCreatureType};
    unit_target::Unit u;
    switch (unit->getType()) {
        case ObjectType::PLAYER: u.type = unit_target::kTypeUnit | unit_target::kTypePlayer; break;
        case ObjectType::UNIT: u.type = unit_target::kTypeUnit; break;
        case ObjectType::GAMEOBJECT: u.type = unit_target::kTypeGameObject; break;
        default: break;
    }
    u.isCaster = unitGuid == casterGuid;
    if (unit->isUnit()) {
        const auto& un = static_cast<const Unit&>(*unit);
        u.health = static_cast<int32_t>(un.getHealth());
        u.unitFlags = entityField(*unit, UF::UNIT_FIELD_FLAGS);
        u.dynamicFlags = un.getDynamicFlags();
        u.petNumber = entityField(*unit, UF::UNIT_FIELD_PETNUMBER);
        u.creatureType = creatureTypeOf(owner_, *unit);
        u.charmed = entityGuidField(*unit, UF::UNIT_FIELD_CHARMEDBY) != 0;
    }
    const auto r = relationsOf(owner_, caster, unit, e.attrEx6);
    const auto t = unit_target::takeUnit(s, u, r, required);
    if (!t.taken) return out;
    if (unit_target::reachChecked(t.mask) && caster->isUnit() && unit->isUnit()) {
        // 0x00801650: the friendly reach for a unit it may help, else the
        // hostile one.
        const int k = r.mayAssist ? 1 : 0;
        unit_target::RangeInput in;
        in.attr = e.attr;
        in.hasRow = e.hasRangeRow;
        in.rowFlags = e.rangeFlags;
        in.rowMin = std::max(0.0f, e.rangeMin[k]);
        in.rowMax = std::max(0.0f, e.rangeMax[k]);
        in.casterReach = static_cast<const Unit&>(*caster).getCombatReach();
        in.hasUnit = true;
        in.unitReach = static_cast<const Unit&>(*unit).getCombatReach();
        in.unitIsPlayer = unit->getType() == ObjectType::PLAYER;
        in.casterMoving = movingForReach(owner_, *caster);
        in.unitMoving = movingForReach(owner_, *unit);
        const auto range = unit_target::unitRange(in, [&](float m) {
            return spell_mods::apply(m, spellModifiers(spellId, spell_mods::kOpRange));
        });
        const glm::vec3 d(unit->getX() - caster->getX(), unit->getY() - caster->getY(), unit->getZ() - caster->getZ());
        if (const uint8_t fail = unit_target::reachFailure(glm::dot(d, d), range)) {
            if (say) {
                // The player's cast reports it as 0x00808200 does; a pet's
                // is only shown.
                const std::string text = castFailureMessage(owner_, spellId, fail, -1, 0, 0);
                if (casterGuid == owner_.getPlayerGuid()) showPlayerCastFailure(spellId, fail, text);
                else owner_.addUIError(text);
            }
            out.outOfReach = true;
            return out;
        }
    }
    return {true, t.required, unitGuid, false};
}

bool SpellHandler::auraLetsSpell(uint64_t unit, uint32_t spellId) const {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return false;
    const auto& spell = it->second;
    uint64_t root = unit;
    for (auto e = owner_.getEntityManager().getEntity(unit); e;) {
        const uint64_t up = entityGuidField(*e, UF::UNIT_FIELD_CREATEDBY);
        if (up == 0) break;
        root = up;
        e = owner_.getEntityManager().getEntity(up);
        if (!e) root = 0;
    }
    const auto* auras = root == owner_.getPlayerGuid() ? &playerAuras_ : getUnitAuras(root);
    if (!auras) return false;
    for (const auto& a : *auras) {
        if (a.spellId == 0) continue;
        const auto as = owner_.spellNameCacheRef().find(a.spellId);
        if (as == owner_.spellNameCacheRef().end()) continue;
        if (pet::auraIgnoresAuraState(as->second.spellFamily, as->second.effectAuraIds, as->second.effectClassMask,
                                      a.flags, spell.spellFamily, spell.spellFamilyFlags))
            return true;
    }
    return false;
}

void SpellHandler::reportLocalCastFailure(uint32_t spellId, uint8_t result) {
    showPlayerCastFailure(spellId, result, castFailureMessage(owner_, spellId, result, -1, 0, 0));
}

void SpellHandler::sayCastFailure(uint32_t spellId, uint8_t result) const {
    owner_.playGameErrorSound(cast_failure::gameError(result, spellRowOf(owner_, spellId)));
}

void SpellHandler::sayPetCastFailure(uint32_t spellId, uint8_t result) const {
    owner_.playGameErrorSound(cast_failure::petGameError(result, spellRowOf(owner_, spellId)));
}

void SpellHandler::showPlayerCastFailure(uint32_t spellId, uint8_t result, const std::string& text) const {
    // OsGetAsyncTimeMs's milliseconds (0x0086ae20); only their differences count.
    const auto nowMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
    if (!failureHold_.report(spellId, result, nowMs) || text.empty()) return;
    owner_.addUIError(text);
    sayCastFailure(spellId, result);
}

void SpellHandler::armUnitTarget(uint32_t spellId, uint64_t caster, uint32_t required) {
    // 0x0080cce0: the cursor holds it (0x00d3f4e4) until a unit is clicked
    // or it is put down; it takes the place of a spell held for a place.
    if (owner_.isAwaitingItemTarget()) owner_.cancelItemTargeting();
    owner_.cancelGlyphTargeting();
    groundTarget_ = GroundTarget{};
    unitTarget_ = UnitTarget{spellId, caster, required};
    LOG_INFO("Unit targeting: spell ", spellId, " waits for a unit (wants 0x", std::hex, required, std::dec, ")");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURSOR_UPDATE", {});
}

void SpellHandler::spellTargetUnit(uint64_t unitGuid) {
    if (unitTarget_.spellId == 0) return;
    if (!owner_.getEntityManager().getEntity(unitGuid)) {
        // 0x0080dc00: no unit there.
        const std::string text =
            castFailureMessage(owner_, unitTarget_.spellId, unit_target::kFailedOutOfRange, -1, 0, 0);
        if (unitTarget_.caster == owner_.getPlayerGuid())
            showPlayerCastFailure(unitTarget_.spellId, unit_target::kFailedOutOfRange, text);
        else owner_.addUIError(text);
        return;
    }
    const auto t = takeUnit(unitTarget_.spellId, unitTarget_.caster, unitGuid, unitTarget_.required, true);
    if (!t.taken) return;
    unitTarget_.required = t.required;
    if (t.required != 0) return;
    const UnitTarget held = unitTarget_;
    unitTarget_ = UnitTarget{};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURSOR_UPDATE", {});
    sendCastBy(held.spellId, held.caster, t.unit);
}

bool SpellHandler::spellCanTargetUnit(uint64_t unitGuid) const {
    return unitTarget_.spellId != 0 &&
           takeUnit(unitTarget_.spellId, unitTarget_.caster, unitGuid, unitTarget_.required, false).taken;
}

void SpellHandler::sendPetCast(uint32_t spellId, uint64_t caster, uint64_t target) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const auto cast = owner_.getPacketParsers() ? owner_.getPacketParsers()->buildCastSpell(spellId, target, ++castCount_)
                                                : CastSpellPacket::build(spellId, target, ++castCount_);
    owner_.getSocket()->send(CastSpellPacket::asPetCast(caster, cast));
    LOG_INFO("Pet 0x", std::hex, caster, " casting spell: ", std::dec, spellId, " on 0x", std::hex, target, std::dec);
    castSent(spellId, caster);
}

void SpellHandler::castPetSpell(uint64_t caster, uint32_t spellId, uint64_t targetGuid) {
    // 0x005d4210 -> 0x0080cce0: a pet's spell cast as the player's are, by
    // the pet - the vehicle the player drives among them.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || caster == 0 || spellId == 0) return;
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    const auto& spell = it->second;
    if ((spell.attr & 0x40u) != 0) return;  // a passive is not cast
    // AttributesEx6 0x40000: its charmer casts it, the player as its own
    // casts go (0x0080cce0 on, 0x0080ac90 sending CMSG_CAST_SPELL).
    if ((spell.attrEx6 & 0x40000u) != 0) {
        auto pet = owner_.getEntityManager().getEntity(caster);
        if (!pet) return;
        caster = pet::castingUnit(caster, spell.attrEx6, entityGuidField(*pet, UF::UNIT_FIELD_CHARMEDBY),
                                  entityGuidField(*pet, UF::UNIT_FIELD_CREATEDBY));
        if (caster == 0 || !owner_.getEntityManager().getEntity(caster)) return;
        if (caster == owner_.getPlayerGuid()) {
            castSpell(spellId, targetGuid);
            return;
        }
    }
    if (owner_.heldBySeat(caster)) return;  // 0x0074ba40
    auto casterEntity = owner_.getEntityManager().getEntity(caster);
    // A dead caster (CGUnit_C +0x128, 0x00716490), unless Attributes
    // 0x800000.
    if (casterEntity && casterEntity->isUnit() &&
        pet::casterDeadRefuses(static_cast<const Unit&>(*casterEntity).getHealth(), spell.attr)) {
        constexpr uint8_t kSpellFailedCasterDead = 0x17;
        owner_.addUIError(castFailureMessage(owner_, spellId, kSpellFailedCasterDead, -1, 0, 0));
        return;
    }
    // AttributesEx7 0x8 wants a caster with UNIT_FIELD_FLAGS_2 0x40000.
    if (casterEntity) {
        if (!pet::casterMayCast(spell.attrEx7, entityField(*casterEntity, UF::UNIT_FIELD_FLAGS_2))) {
            constexpr uint8_t kSpellFailedSpellUnavailable = 0x6b;
            owner_.addUIError(castFailureMessage(owner_, spellId, kSpellFailedSpellUnavailable, -1, 0, 0));
            return;
        }
        // Attributes 0x2: a ranged weapon in its third virtual slot, not
        // disarmed of it (CGUnit_C +0x12c -> 0x0071f440).
        const uint16_t slots = fieldIndex(UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
        const uint32_t ranged = slots != 0xFFFF ? casterEntity->getField(static_cast<uint16_t>(slots + 2)) : 0u;
        if (pet::rangedWeaponRefuses(spell.attr, ranged, entityField(*casterEntity, UF::UNIT_FIELD_FLAGS_2))) {
            constexpr uint8_t kSpellFailedEquippedItem = 0x1c;
            owner_.addUIError(castFailureMessage(owner_, spellId, kSpellFailedEquippedItem, -1, 0, 0));
            return;
        }
        // AttributesEx 0x500000 from the pet: its combo points on the unit
        // (0x005d3520), else an aura 262 for the spell on the unit that
        // made it, up its creators (0x0072aaf0 -> 0x007283a0).
        if ((spell.attrEx & 0x500000u) != 0 && caster == owner_.getPetGuid()) {
            const bool auraLetsIt = auraLetsSpell(caster, spellId);
            const uint8_t casterClass =
                static_cast<uint8_t>((entityField(*casterEntity, UF::UNIT_FIELD_BYTES_0) >> 8) & 0xFFu);
            if (const uint8_t fail = pet::comboPointsFailure(spell.attrEx, owner_.petComboPointsOn(targetGuid),
                                                             auraLetsIt, casterClass)) {
                owner_.addUIError(castFailureMessage(owner_, spellId, fail, -1, 0, 0));
                return;
            }
        }
    }
    cancelGroundTargeting();  // 0x00809a60
    if (armGroundTarget(spellId, caster)) return;
    castAtUnits(spellId, caster, targetGuid);
}

int64_t SpellHandler::historyNowMs() {
    return static_cast<int64_t>(core::appTimeSeconds() * 1000.0);
}

uint32_t SpellHandler::spellCategoryFlags(uint32_t category) const {
    if (!spellCategoriesLoaded_) {
        spellCategoriesLoaded_ = true;
        auto* am = owner_.services().assetManager;
        // SpellCategory.dbc: the id and Flags.
        if (auto dbc = am && am->isInitialized() ? am->loadDBC("SpellCategory.dbc") : nullptr;
            dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                spellCategoryFlags_[dbc->getUInt32(i, 0)] = dbc->getUInt32(i, 1);
        }
    }
    const auto it = spellCategoryFlags_.find(category);
    return it != spellCategoryFlags_.end() ? it->second : 0u;
}

int32_t SpellHandler::categoryCooldown(uint32_t spellId, bool pet) const {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return 0;
    const auto& e = it->second;
    auto ms = static_cast<int32_t>(e.categoryRecoveryMs);
    if (!pet) {
        // A category flagged 0x1 lasts as long as the main hand's swing
        // makes it, in thousandths (item template +0xf4).
        if ((spellCategoryFlags(e.category) & 0x1u) != 0) {
            const uint64_t guid = owner_.getEquipSlotGuid(static_cast<int>(EquipSlot::MAIN_HAND));
            const auto item = owner_.onlineItemsRef().find(guid);
            if (const auto* info = item != owner_.onlineItemsRef().end() ? owner_.getItemInfo(item->second.entry)
                                                                          : nullptr)
                ms = static_cast<int32_t>(info->delayMs) * ms / 1000;
        }
        if ((e.attr & 0x2u) != 0 && (e.attrEx2 & 0x20000u) == 0) {
            const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
            if (player) ms += static_cast<int32_t>(entityField(*player, UF::UNIT_FIELD_RANGEDATTACKTIME));
        }
    }
    return (e.attrEx6 & 0x80000000u) == 0 ? moddedCooldown(spellId, ms) : ms;
}

void SpellHandler::playerCooldownsChanged() {
    if (!owner_.addonEventCallbackRef()) return;
    owner_.addonEventCallbackRef()("ACTIONBAR_UPDATE_COOLDOWN", {});
    owner_.addonEventCallbackRef()("SPELL_UPDATE_COOLDOWN", {});
}

const std::array<uint32_t, 3>* SpellHandler::runeCostRow(uint32_t id) const {
    if (!runeCostRowsLoaded_) {
        runeCostRowsLoaded_ = true;
        auto* am = owner_.services().assetManager;
        // SpellRuneCost.dbc: the id, blood, unholy, frost, runic power.
        if (auto dbc = am && am->isInitialized() ? am->loadDBC("SpellRuneCost.dbc") : nullptr;
            dbc && dbc->isLoaded() && dbc->getFieldCount() >= 4) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                runeCostRows_[dbc->getUInt32(i, 0)] = {dbc->getUInt32(i, 1), dbc->getUInt32(i, 2), dbc->getUInt32(i, 3)};
        }
    }
    const auto it = runeCostRows_.find(id);
    return id != 0 && it != runeCostRows_.end() ? &it->second : nullptr;
}

SpellHandler::RuneCost SpellHandler::runeCost(uint32_t spellId) const {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end() || it->second.powerType != 5) return {};
    const auto* row = runeCostRow(it->second.runeCostId);
    const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!row || !player) return {};
    // The least of the spell's schools' cost modifier and multiplier
    // (0x005ee050, 0x005ee0b0) on 100, through the cost modifiers.
    int32_t mod = 0;
    float mult = 0.0f;
    bool any = false;
    for (uint32_t school = 0; school < 7; ++school) {
        if ((it->second.schoolMask & (1u << school)) == 0) continue;
        const uint16_t mi = fieldIndex(UF::UNIT_FIELD_POWER_COST_MODIFIER);
        const uint16_t xi = fieldIndex(UF::UNIT_FIELD_POWER_COST_MULTIPLIER);
        const auto m = static_cast<int32_t>(mi != 0xFFFF ? player->getField(static_cast<uint16_t>(mi + school)) : 0u);
        const float x = xi != 0xFFFF ? std::bit_cast<float>(player->getField(static_cast<uint16_t>(xi + school))) : 0.0f;
        if (!any || m < mod) mod = m;
        if (!any || x < mult) mult = x;
        any = true;
    }
    auto pct = static_cast<int32_t>(std::nearbyint(static_cast<float>(mod + 100) * (mult + 1.0f)));
    if (const auto mods = spellModifiers(spellId, 14); mods.any) pct = (pct + mods.flat) * mods.pct / 100;
    if (pct == 0) return {};
    const uint32_t cost[3] = {(*row)[0], (*row)[1], (*row)[2]};
    const auto& r = owner_.getPlayerRunes();
    if (runes::costMet(r, cost, pct)) return {};
    return {false, runes::readyFor(r, cost, pct)};
}

spell_history::History::Cooldown SpellHandler::itemCooldown(uint32_t itemId) const {
    const auto* info = owner_.getItemInfo(itemId);
    if (!info || !info->valid) return {};
    for (const auto& sp : info->spells)
        if (sp.spellId != 0 && sp.spellTrigger == 0) return historyCooldown(sp.spellId, itemId, false);
    return {};
}

spell_history::History::Cooldown SpellHandler::historyCooldown(uint32_t spellId, uint32_t itemId, bool pet) const {
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return {};
    // 0x00807980: none for a spell whose first effect is 78 or 47
    // (TRADE_SKILL).
    if (it->second.effectIds[0] == 78 || it->second.effectIds[0] == 47) return {};
    // With a rune cost (RuneCostID), the runes it waits on (0x005ee7e0), for
    // an entry of its own or of a spell sharing a rune type with it.
    std::optional<int64_t> runesReady;
    const auto* row = runeCostRow(it->second.runeCostId);
    if (row) {
        if (const auto cost = runeCost(spellId); !cost.paid) runesReady = cost.readyAt;
    }
    const auto sharesRunes = [&](uint32_t other) {
        const auto o = owner_.spellNameCacheRef().find(other);
        const auto* orow = o != owner_.spellNameCacheRef().end() ? runeCostRow(o->second.runeCostId) : nullptr;
        if (!row || !orow) return false;
        for (int t = 0; t < 3; ++t)
            if ((*row)[t] != 0 && (*orow)[t] != 0) return true;
        return false;
    };
    // An item's template gives the spell its category (ItemSpellCategory
    // over 0, the last of its slots naming the spell).
    uint32_t category = it->second.category;
    if (const auto* info = itemId != 0 ? owner_.getItemInfo(itemId) : nullptr; info && info->valid) {
        for (const auto& sp : info->spells)
            if (sp.spellId == spellId && static_cast<int32_t>(sp.category) > 0) category = sp.category;
    }
    return (pet ? petHistory_ : playerHistory_)
        .query(spellId, category, it->second.startRecoveryCategory, historyNowMs(),
               [this](uint32_t c) { return (spellCategoryFlags(c) & 0x2u) != 0; }, runesReady, sharesRunes,
               itemId);
}

int32_t SpellHandler::moddedCooldown(uint32_t spellId, int32_t ms) const {
    // 0x007fd970 with op 11, Cooldown.
    return static_cast<int32_t>(spell_mods::apply(static_cast<float>(ms), spellModifiers(spellId, 11)));
}

bool SpellHandler::petToggleHeld(uint32_t spellId) const {
    // 0x00809100: the cvar, then the pet's list (0x00d3f5ac + 24*1).
    return addons::storedCVarValue("secureAbilityToggle", "0") != "0" &&
           petHistory_.startedRecently(spellId, historyNowMs());
}

void SpellHandler::notePetActionCast(uint32_t spellId, uint64_t pet) {
    noteGlobalCooldown(spellId, pet, petHistory_);
}

void SpellHandler::noteGlobalCooldown(uint32_t spellId, uint64_t caster, spell_history::History& history) {
    // 0x00805d70: StartRecoveryCategory or StartRecoveryTime - the global
    // cooldown through its modifiers (op 21), begun now.
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    const auto& e = it->second;
    if (e.startRecoveryCategory == 0 && e.startRecoveryMs == 0) return;
    auto unit = owner_.getEntityManager().getEntity(caster);
    const float speed = unit ? castSpeedOf(*unit) : 1.0f;
    const auto mods = spellModifiers(spellId, 21);
    const int32_t gcd = spell_history::globalCooldown(
        e.startRecoveryCategory, static_cast<int32_t>(spell_mods::apply(static_cast<float>(e.startRecoveryMs), mods)),
        static_cast<int32_t>(e.startRecoveryMs), e.dmgClass, e.attr, speed);
    if (gcd == 0) return;
    const int64_t now = historyNowMs();
    spell_history::Entry h;
    h.spellId = spellId;
    h.start = now;
    h.categoryStart = now;
    h.onHold = (e.attr & 0x2000000u) != 0;  // 0x00801220
    h.gcdCategory = e.startRecoveryCategory;
    h.gcd = gcd;
    history.add(h);
}

void SpellHandler::notePetCastFailed(uint32_t spellId) {
    petHistory_.dropGlobalCooldown(spellId, historyNowMs());
}

void SpellHandler::castSent(uint32_t spellId, uint64_t caster) {
    // 0x0080ac90: AttributesEx7 0x80 from the active mover - the player's
    // seat change is asked (0x00747fa0, the player seated), then the mover
    // stops, its keys as they are (0x0071ae10).
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    if (vehicle::castEndsVehicleControl(it->second.attrEx7, caster, owner_.activeMover())) {
        owner_.askVehicleSeatChange();
        owner_.stopMover();
    }
    // Then the global cooldown begins (0x00805d70) - the player's here, the
    // pet's where its bar sends it (notePetActionCast).
    if (caster == owner_.getPlayerGuid()) {
        noteGlobalCooldown(spellId, caster, playerHistory_);
        playerCooldownsChanged();
    }
}

void SpellHandler::castSpell(uint32_t spellId, uint64_t targetGuid) {
    LOG_DEBUG("castSpell: spellId=", spellId, " target=0x", std::hex, targetGuid, std::dec);
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // 0x0080cce0: nothing is cast by a caster its seat holds (0x0074ba40) -
    // on its way into or out of one, or its move held for the vehicle's
    // animation.
    if (owner_.heldBySeat(owner_.getPlayerGuid())) return;

    // Auto Shot and the wand's Shoot keep going once started, and the action
    // button flashes for as long as they do. The stop half comes from the
    // server on SMSG_CANCEL_AUTO_REPEAT; this is the start.
    constexpr uint32_t kAutoShot = 75, kShoot = 5019;
    // A spell with AttributesEx2 0x20 repeats: +0xa38 0x200 while it does
    // (0x007fa2e0), which the ranged readies read (0x00714f90).
    if ((owner_.getSpellAttributesEx2(spellId) & 0x20u) != 0) owner_.setAutoRepeating(true);
    if ((spellId == kAutoShot || spellId == kShoot) && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("START_AUTOREPEAT_SPELL", {});
    }

    // Food and water are server auras, but using any action interrupts them.
    // Cancel the aura and stand first, then allow the requested action to proceed.
    if (restorationActive_) cancelCast();

    // Attack routes to auto-attack instead of cast
    if (spellId == SPELL_ID_ATTACK) {
        uint64_t target = targetGuid != 0 ? targetGuid : owner_.getTargetGuid();
        if (target != 0) {
            if (owner_.isAutoAttacking()) {
                // Ability Toggle, which the panel describes exactly: protection
                // from turning an ability off by hitting its button twice in a
                // short space of time. Offered, and read by nothing - so a
                // double-tap on Attack has always stopped the swing that the
                // first tap started.
                //
                // The guard is on the second press rather than on the state:
                // stopping deliberately a second later is what the button is
                // for, and only the accident within the window is refused.
                const auto now = std::chrono::steady_clock::now();
                const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - autoAttackToggledAt_).count();
                constexpr long kToggleGuardMs = 500;
                if (autoAttackToggledAt_.time_since_epoch().count() != 0 &&
                    since < kToggleGuardMs &&
                    addons::storedCVarValue("secureAbilityToggle", "0") != "0") {
                    return;
                }
                owner_.stopAutoAttack();
                autoAttackToggledAt_ = now;
            } else {
                owner_.startAutoAttack(target);
                autoAttackToggledAt_ = std::chrono::steady_clock::now();
            }
        }
        return;
    }

    // Action bars restored from the server can still hold a rank that a higher rank
    // has since superseded. The server drops casts of superseded ranks without
    // sending any error, so swap in the highest rank we actually know.
    spellId = resolveHighestKnownRank(spellId);

    // Fishing places a bobber in front of the caster using the facing the server has
    // on record. The client only sends MSG_MOVE_SET_FACING when the aim changes by >3°
    // (throttled to 10 Hz), so a small final aim adjustment before pressing cast may not
    // have reached the server - leaving it with a slightly stale facing that drops the
    // bobber off to the side or on land ("Face open water"). Push the exact current
    // facing right before the cast so the bobber lands where the player is aiming.
    if (spellclass::isFishingCast(spellId) && owner_.getSocket()) {
        // Snap the character to face where the camera is looking, then push that facing to
        // the server, so the bobber lands in front of the player's view. Standing still the
        // character yaw doesn't follow the free-look camera, so without this the bobber
        // dropped toward a stale heading (off to the side / on land → "Face open water").
        const float canonO = owner_.faceCameraDirection();
        owner_.setOrientation(canonO);
        owner_.sendMovement(Opcode::MSG_MOVE_SET_FACING);
    }

    // Profession spells (Cooking, First Aid, Alchemy, ...) open the crafting
    // window client-side instead of being sent as casts - matching the real
    // client, where these spells just open the tradeskill UI.
    if (uint32_t craftSkillLine = tradeskillOpenerSkillLine(spellId)) {
        // At warning, with the three refusals above it, so one press says which
        // of the four things happened.
        LOG_WARNING("Profession: spell ", spellId, " (", owner_.getSpellName(spellId),
                    ") opens the trade skill window for skill line ", craftSkillLine);
        openCraftingWindow(craftSkillLine);
        return;
    }

    // Pressing the companion you already have out puts it away. A companion has
    // no aura, so there is nothing in the buff bar to right-click and this is
    // the only way to send one back - without it the button could only ever
    // summon, which is why pressing it again just produced the same critter.
    if (spellId != 0 && spellId == owner_.getActiveCritterSpellId() &&
        owner_.getActiveCritterGuid() != 0) {
        owner_.dismissCritter();
        return;
    }

    // A spell cast while mounted: refused as NOT_MOUNTED where the player
    // would not get down (0x00739650, castChecksPass), the mount left as
    // it is sent where it would (0x0080ac90, sendPlayerCast).
    if (owner_.isMounted()) {
        // Pressing the mount you are already riding dismounts you and stops
        // there. Falling through to the cast would put the player straight back
        // on the same mount, so the button appeared to do nothing.
        //
        // Asked of the player's own auras as well as of mountAuraSpellId_. The
        // latter is what the client worked out when the mount appeared, and it
        // is only as good as what it had to go on: with no cast of the player's
        // own to point at - logging in already mounted, or an aura the server
        // applied - the scan behind it keeps whichever indefinite aura it saw
        // last, which is as easily a racial or a tracking buff. The mount you
        // are on is carrying its own aura, and no other mount's, so that is the
        // question with an answer.
        const bool ridingThisMount =
            spellId != 0 && (spellId == owner_.getMountAuraSpellId() ||
                             owner_.playerCarriesAura(spellId));
        if (ridingThisMount) {
            owner_.dismount();
            LOG_INFO("Dismount via mount action: spell=", spellId);
            return;
        }
    }

    // 0x0080cce0 -> 0x00805f60: the same spell already pending, dropped.
    if (casting_ && currentCastSpellId_ == spellId) {
        const auto it = owner_.spellNameCacheRef().find(spellId);
        if (cast_check::dropsPendingPress(true, playerCooldown(spellId).running,
                                          it != owner_.spellNameCacheRef().end() ? it->second.attr : 0u))
            return;
    }

    // 0x0080cce0: what the cursor holds is put down (0x00809a60), then the
    // unit asked is held to the reach (0x00809610) and the caster to
    // 0x00739650's rules.
    cancelGroundTargeting();
    if (!castChecksPass(spellId, targetGuid)) return;

    // A spell that wants a place on the ground waits for it (0x0080c790).
    if (armGroundTarget(spellId, 0)) return;

    // Another spell pressed while a cast runs is not held back: neither
    // 0x0080cce0 nor 0x0080ac90 asks after the running cast, so it is sent,
    // and the server answers SPELL_FAILED_SPELL_IN_PROGRESS (or, for one it
    // lets through, interrupts the channel itself). Nothing is kept for when
    // the cast ends: 3.3.5 has no spell queue.

    // Spells cast at an item - Disenchant, Prospecting, Milling, the enchant
    // formulas - carry TARGET_FLAG_ITEM in Spell.dbc's Targets. Sending one with
    // no item in SpellCastTargets is what made the server answer "can't be
    // disenchanted": it evaluated the question against nothing. Arm the same
    // item-picking cursor an enchanting scroll uses and send the cast once the
    // player chooses.
    constexpr uint32_t kSpellTargetFlagItem = 0x10;
    if ((getSpellTargetFlags(spellId) & kSpellTargetFlagItem) != 0) {
        // 0x0080c790: a temporary enchantment of the player's (Attributes
        // 0x200) goes on the weapon in hand without asking; with none in the
        // main hand to fall back on, MAINHAND_EMPTY (0x0080cce0).
        if (const uint64_t weapon = enchantWeapon(spellId); weapon != 0) {
            sendPlayerCast(spellId, 0, weapon);
            return;
        }
        if (const auto it = owner_.spellNameCacheRef().find(spellId);
            it != owner_.spellNameCacheRef().end() &&
            unit_target::noUnitFailure(it->second.attr, it->second.attrEx5, 0, true,
                                       owner_.getEquipSlotGuid(static_cast<int>(EquipSlot::MAIN_HAND)) != 0,
                                       false) == unit_target::kFailedMainhand) {
            showPlayerCastFailure(spellId, unit_target::kFailedMainhand,
                                  castFailureMessage(owner_, spellId, unit_target::kFailedMainhand, -1, 0, 0));
            return;
        }
        if (owner_.isAwaitingItemTarget()) {
            // Not said: the interface has no such line.
            return;
        }
        owner_.beginSpellItemTargeting(spellId, getSpellName(spellId));
        return;
    }

    // 0x0080c790: the unit it goes at - none, the one asked, the player by
    // autoSelfCast - or the cursor holds it for one (0x0080cce0). Without
    // Spell.dbc, at what is selected.
    if (owner_.spellNameCacheRef().count(spellId) == 0) {
        sendPlayerCast(spellId, targetGuid != 0 ? targetGuid : owner_.getTargetGuid());
        return;
    }
    castAtUnits(spellId, owner_.getPlayerGuid(), targetGuid);
}

uint64_t SpellHandler::enchantWeapon(uint32_t spellId) const {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end() || (it->second.attr & 0x200u) == 0) return 0;
    const auto& e = it->second;
    // The equipped weapons (0x00754390 with 15, 16), each with its
    // temporary enchantment and the time it has left.
    const auto weapon = [&](EquipSlot slot, uint32_t timer, uint64_t& guid) {
        unit_target::HandWeapon w;
        guid = owner_.getEquipSlotGuid(static_cast<int>(slot));
        const auto item = owner_.onlineItemsRef().find(guid);
        if (guid == 0 || item == owner_.onlineItemsRef().end()) return w;
        w.present = true;
        if (const auto* info = owner_.getItemInfo(item->second.entry)) {
            w.itemClass = info->itemClass;
            w.subClass = info->subClass;
        }
        w.tempEnchant = item->second.temporaryEnchantId;
        w.remaining = static_cast<int32_t>(owner_.getTempEnchantRemainingMs(timer));
        return w;
    };
    uint64_t mainGuid = 0, offGuid = 0;
    const auto main = weapon(EquipSlot::MAIN_HAND, 0, mainGuid);
    const auto off = weapon(EquipSlot::OFF_HAND, 1, offGuid);
    // The first ENCHANT_ITEM_TEMPORARY (54) effect a weapon takes.
    for (int k = 0; k < 3; ++k) {
        if (e.effectIds[k] != 54) continue;
        switch (unit_target::enchantHand(static_cast<uint32_t>(e.equippedItemClass), e.equippedItemSubClassMask,
                                         e.effectMiscValues[k], main, off)) {
            case unit_target::Hand::Main: return mainGuid;
            case unit_target::Hand::Off: return offGuid;
            case unit_target::Hand::None: break;
        }
    }
    return 0;
}

void SpellHandler::sendPlayerCast(uint32_t spellId, uint64_t target, uint64_t item) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // 0x0080ac90 before it sends: the player stands (0x0071a360) unless
    // Attributes 0x8000000, gets down (0x007412e0) unless Attributes
    // 0x1000000, and leaves a form the spell may not be cast in
    // (0x00726ce0) - what 0x00739650 let go on autoStand, autoDismount and
    // autoUnshift.
    if (const auto it = owner_.spellNameCacheRef().find(spellId); it != owner_.spellNameCacheRef().end()) {
        const auto& e = it->second;
        const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
        if (player && player->isUnit()) {
            const uint16_t b1 = fieldIndex(UF::UNIT_FIELD_BYTES_1);
            if ((e.attr & 0x8000000u) == 0 && b1 != 0xFFFF && (player->getField(b1) & 0xFFu) != 0)
                owner_.setStandState(0);
            if ((e.attr & 0x1000000u) == 0 && owner_.isMounted()) owner_.dismount();
            const auto c = casterState(spellId, *player);
            if (cast_check::formFailure(checkSpellOf(e), c) == cast_check::kNotShapeshift) {
                // The form's aura: one that is SHAPESHIFT (36) to the form.
                for (const auto& a : playerAuras_) {
                    const auto f = a.spellId != 0 ? owner_.spellNameCacheRef().find(a.spellId)
                                                  : owner_.spellNameCacheRef().end();
                    if (f == owner_.spellNameCacheRef().end()) continue;
                    for (int k = 0; k < 3; ++k) {
                        if (f->second.effectAuraIds[k] == 36 && f->second.effectMiscValues[k] == c.form) {
                            cancelAura(a.spellId);
                            break;
                        }
                    }
                }
            }
        }
    }
    // Remembered so that, if this cast turns out to be what mounted the player,
    // the mount aura can be identified exactly rather than guessed at.
    if (!owner_.isMounted()) lastGroundCastSpellId_ = spellId;
    const bool fishingCast = spellclass::isFishingCast(spellId);
    // A spell that lands on you never turns you. Facing is worked out from the
    // vector between you and your target, and when the target is you that
    // vector is the gap between the position this client has moved you to and
    // the one the server last confirmed - which, while running, points
    // backwards along your own path.
    //
    // Face the target before casting any targeted spell (server checks facing arc).
    // Only with the Combat page's debug setting on:
    // the original client never turns the player to cast, and a target behind
    // them is refused as not in front.
    if (target != 0 && target != owner_.getPlayerGuid() && owner_.isAutoFaceTarget()) {
        auto entity = owner_.getEntityManager().getEntity(target);
        if (entity) {
            float dx = entity->getX() - owner_.movementInfoRef().x;
            float dy = entity->getY() - owner_.movementInfoRef().y;
            float lenSq = dx * dx + dy * dy;
            if (lenSq > 0.01f) {
                owner_.faceCanonicalYaw(std::atan2(-dy, dx));
            }
        }
    }
    // Heartbeat ensures the server has the updated orientation before the cast packet.
    if (target != 0 || fishingCast) {
        owner_.sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    auto packet = item != 0
        ? (owner_.getPacketParsers() ? owner_.getPacketParsers()->buildCastSpellOnItem(spellId, item)
                                     : CastSpellPacket::buildItemTarget(spellId, item, ++castCount_))
        : owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildCastSpell(spellId, target, ++castCount_)
        : CastSpellPacket::build(spellId, target, ++castCount_);
    LOG_DEBUG("CMSG_CAST_SPELL: spellId=", spellId, " target=0x", std::hex, target, std::dec,
              " castCount=", static_cast<int>(castCount_), " packetSize=", packet.getSize());
    owner_.getSocket()->send(packet);
    LOG_INFO("Casting spell: ", spellId, " on 0x", std::hex, item != 0 ? item : target, std::dec);
    castSent(spellId, owner_.getPlayerGuid());

    // Fire UNIT_SPELLCAST_SENT for cast bar addons
    if (owner_.addonEventCallbackRef()) {
        std::string targetName;
        if (target != 0) targetName = owner_.lookupName(target);
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_SENT", {"player", targetName, std::to_string(spellId)});
    }
}

bool SpellHandler::castChecksPass(uint32_t spellId, uint64_t targetGuid) {
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return true;
    const auto& e = it->second;
    // 0x00809610: the unit asked - the cast's, else the target - where the
    // spell may take it (Targets with each effect's ImplicitTargetA, the pet
    // for TARGET_UNIT_PET), held to the spell's reach (0x007ff480,
    // 0x00803850) and refused, said, outside it.
    const uint64_t asked = targetGuid != 0 ? targetGuid : owner_.getTargetGuid();
    const auto kinds = unit_target::reachKinds(e.targetFlags, e.implicitTargetsA);
    const uint64_t unit = kinds.pet ? owner_.getPetGuid() : asked;
    if (kinds.mask != 0 && unit != 0 && takeUnit(spellId, owner_.getPlayerGuid(), unit, kinds.mask, true).outOfReach)
        return false;
    // 0x00739650.
    const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!player || !player->isUnit()) return true;
    const auto r = cast_check::check(checkSpellOf(e), casterState(spellId, *player));
    switch (r.error) {
        case cast_check::GameError::SpellAlreadyKnown:
            // ERR_SPELL_ALREADY_KNOWN_S with the spell's name (0x005216f0 0x36).
            owner_.addUIError(interfaceFormat("ERR_SPELL_ALREADY_KNOWN_S", "You already know %s.",
                                              {getSpellName(r.errorSpell)}));
            owner_.playGameErrorSound("ERR_SPELL_ALREADY_KNOWN_S");
            return false;
        case cast_check::GameError::TameFailedTooMany:
            // 0x00800390(2): ERR_TAME_FAILED with PETTAME_TOOMANY.
            owner_.addUIError(interfaceFormat("ERR_TAME_FAILED", "%s",
                                              {interfaceString("PETTAME_TOOMANY", "You have too many pets.")}));
            return false;
        case cast_check::GameError::None: break;
    }
    if (r.code == 0) return true;
    showPlayerCastFailure(spellId, r.code,
                          castFailureMessage(owner_, spellId, r.code, static_cast<int32_t>(e.powerType), r.arg));
    return false;
}

uint32_t SpellHandler::classSpellFamily() const {
    // The class's spell family, ChrClasses SpellClassSet (+0x20, WotLK
    // column 56; 22 in TBC's file, 14 in Classic's), set when the player
    // enters the world (0x008007a0, 0x00d397b4).
    const uint8_t classId = owner_.getPlayerClass();
    if (classSpellFamilyFor_ != classId) {
        classSpellFamilyFor_ = classId;
        classSpellFamily_ = 0;
        auto* am = core::Application::getInstance().getAssetManager();
        auto classes = am ? am->loadDBCOptional("ChrClasses.dbc") : nullptr;
        const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ChrClasses") : nullptr;
        const uint32_t field = layout ? layout->tryField("SpellClassSet") : 0xFFFFFFFF;
        const int32_t row = classes && field < classes->getFieldCount() ? classes->findRecordById(classId) : -1;
        if (row >= 0) classSpellFamily_ = classes->getUInt32(static_cast<uint32_t>(row), field);
    }
    return classSpellFamily_;
}

bool SpellHandler::historyHeld(uint32_t spellId, uint32_t itemId, uint32_t category) const {
    // 0x00807da0: an entry of the spell's from the item (none: 0) with a
    // cooldown, or of the category the spell is in (the item's own where
    // it gives one), its cooldown held until an event.
    for (const auto& e : playerHistory_.entries()) {
        if (!e.onHold) continue;
        if (e.spellId == spellId && e.itemId == itemId) return true;
        if (e.category == category && e.categoryDuration != 0) return true;
    }
    return false;
}

bool SpellHandler::toolsCarried(uint32_t spellId, const Entity& player) const {
    const auto& e = owner_.spellNameCacheRef().find(spellId)->second;
    const auto& inv = owner_.getInventory();
    // 0x008091d0: each Totem carried (0x00754a20) ...
    for (const uint32_t totem : e.totems)
        if (totem != 0 && inv.countCarried(totem) == 0) return false;
    // ... each TotemCategory met by what is carried: items whose own
    // category is of its type, their masks together covering its mask
    // (0x007548f0 -> 0x007541f0; TotemCategory.dbc type 18, mask 19).
    auto* am = core::Application::getInstance().getAssetManager();
    auto cats = am ? am->loadDBCOptional("TotemCategory.dbc") : nullptr;
    for (const uint32_t cat : e.totemCategories) {
        if (cat == 0) continue;
        const int32_t row = cats && cats->getFieldCount() > 19 ? cats->findRecordById(cat) : -1;
        if (row < 0) return false;
        const uint32_t type = cats->getUInt32(static_cast<uint32_t>(row), 18);
        uint32_t need = cats->getUInt32(static_cast<uint32_t>(row), 19);
        inv.forEachCarried([&](const ItemDef& d) {
            const auto* info = owner_.getItemInfo(d.itemId);
            if (!info || info->totemCategory == 0 || need == 0) return;
            const int32_t r = cats->findRecordById(info->totemCategory);
            if (r >= 0 && cats->getUInt32(static_cast<uint32_t>(r), 18) == type)
                need &= ~cats->getUInt32(static_cast<uint32_t>(r), 19);
        });
        if (need != 0) return false;
    }
    // 0x00800d60: no reagents prepared (UNIT_FLAG 0x20 with AttributesEx5
    // 0x2), or for a spell of the class's family PLAYER_NO_REAGENT_COST
    // names.
    const uint16_t flagsIdx = fieldIndex(UF::UNIT_FIELD_FLAGS);
    const uint32_t unitFlags = flagsIdx != 0xFFFF ? player.getField(flagsIdx) : 0u;
    bool free = (unitFlags & 0x20u) != 0 && (e.attrEx5 & 0x2u) != 0;
    if (!free && e.spellFamily != 0 && e.spellFamily == classSpellFamily()) {
        const uint16_t mask = fieldIndex(UF::PLAYER_NO_REAGENT_COST_1);
        for (uint16_t w = 0; mask != 0xFFFF && w < 3 && !free; ++w)
            free = (player.getField(static_cast<uint16_t>(mask + w)) & e.spellFamilyFlags[w]) != 0;
    }
    if (free) return true;
    // Each reagent carried in its count - one more where the reagent is an
    // item whose own spell this is, with charges.
    for (const auto& r : e.reagents) {
        if (r.itemId == 0) continue;
        uint32_t need = r.count;
        if (const auto* info = owner_.getItemInfo(r.itemId)) {
            for (const auto& sp : info->spells)
                if (sp.spellId == spellId && sp.charges != 0) ++need;
        }
        if (inv.countCarried(r.itemId) < need) return false;
    }
    return true;
}

bool SpellHandler::weaponCarried(uint32_t spellId, const Entity& player) const {
    const auto& e = owner_.spellNameCacheRef().find(spellId)->second;
    // 0x008093d0: no item wanted (Targets 0x10, no EquippedItemClass or
    // subclasses) ...
    if ((e.targetFlags & 0x10u) != 0 || e.equippedItemClass < 0 || e.equippedItemSubClassMask == 0) return true;
    const auto field = [&](UF f) {
        const uint16_t i = fieldIndex(f);
        return i != 0xFFFF ? player.getField(i) : 0u;
    };
    // ... else one in the slots it looks in (0x006de230): the main hand for
    // AttributesEx3 0x400, the off hand for 0x1000000, the ranged slot for
    // a ranged spell (Attributes 0x2), else any - not a hand disarmed, nor a
    // broken item.
    const auto found = [&](uint32_t slots) -> const ItemQueryResponseData* {
        const uint32_t flags = field(UF::UNIT_FIELD_FLAGS), flags2 = field(UF::UNIT_FIELD_FLAGS_2);
        if ((flags & 0x200000u) != 0) slots &= ~0x8000u;
        if ((flags2 & 0x80u) != 0) slots &= ~0x10000u;
        if ((flags2 & 0x400u) != 0) slots &= ~0x20000u;
        for (int slot = 0; slot < 0x17; ++slot) {
            if ((slots & (1u << slot)) == 0) continue;
            const auto it = owner_.onlineItemsRef().find(owner_.getEquipSlotGuid(slot));
            if (it == owner_.onlineItemsRef().end()) continue;
            if (it->second.maxDurability != 0 && it->second.curDurability == 0) continue;
            const auto* info = owner_.getItemInfo(it->second.entry);
            if (info && static_cast<int32_t>(info->itemClass) == e.equippedItemClass &&
                (e.equippedItemSubClassMask & (1u << (info->subClass & 31))) != 0)
                return info;
        }
        return nullptr;
    };
    uint32_t slots = 0xFFFFFFFFu;
    if ((e.attrEx3 & 0x400u) != 0) slots = 0x8000u;
    else if ((e.attrEx3 & 0x1000000u) != 0) slots = 0x10000u;
    else if ((e.attr & 0x2u) != 0) slots = 0x20000u;
    const auto* weapon = found(slots);
    if (!weapon) return false;
    // Both hands wanted (AttributesEx3 0x1000400): the off hand too.
    if ((e.attrEx3 & 0x1000400u) == 0x1000400u && !found(0x10000u)) return false;
    // Its ammunition, where its ItemSubClass row (Flags 0x10: thrown) does
    // not do without: PLAYER_AMMO_ID put in, and some carried.
    auto* am = core::Application::getInstance().getAssetManager();
    if (auto sub = am ? am->loadDBCOptional("ItemSubClass.dbc") : nullptr; sub && sub->getFieldCount() > 4) {
        for (uint32_t r = 0; r < sub->getRecordCount(); ++r) {
            if (sub->getUInt32(r, 0) != weapon->itemClass || sub->getUInt32(r, 1) != weapon->subClass) continue;
            if ((sub->getUInt32(r, 4) & 0x10u) != 0) return true;
            break;
        }
    }
    if (weapon->ammoType == 0) return true;
    const uint32_t ammo = field(UF::PLAYER_AMMO_ID);
    return ammo != 0 && owner_.getInventory().countCarried(ammo) != 0;
}

bool SpellHandler::spellUsable(uint32_t spellId, const ItemQueryResponseData* item) {
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return false;
    const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!player || !player->isUnit()) return false;
    const auto& e = it->second;
    namespace cc = cast_check;
    const cc::Caster c = casterState(spellId, *player);
    cc::Usable u;
    const uint16_t pf = fieldIndex(UF::PLAYER_FLAGS);
    u.playerFlags = pf != 0xFFFF ? player->getField(pf) : 0u;
    // vtable +0x128: dead, or a ghost (PLAYER_FLAGS 0x10).
    u.deadOrGhost = static_cast<const Unit&>(*player).getHealth() == 0 || (u.playerFlags & 0x10u) != 0;
    u.control = owner_.isServerMovementAllowed();
    // An item flagged 0x10000000 is used without the spell's tools.
    u.tools = (item && (item->itemFlags & 0x10000000u) != 0) || toolsCarried(spellId, *player);
    u.equipped = weaponCarried(spellId, *player);
    // 0x00513920: the combo points are on the target.
    u.comboPoints = owner_.getComboPoints() != 0 && owner_.getComboTarget() == owner_.getTargetGuid() &&
                    owner_.getTargetGuid() != 0;
    u.held = historyHeld(spellId, 0, e.category);
    // The target's aura states and spells (TargetAuraState [21], its Not
    // [23], TargetAuraSpell [25], ExcludeTargetAuraSpell [27]) and the side
    // its first effect wants. With autoSelfCast, no target, or a friend
    // wanted and the target no friend, the player stands for it.
    const auto spellKnown = [&](uint32_t id) { return id != 0 && owner_.spellNameCacheRef().count(id) != 0; };
    if (e.targetAuraState != 0 || e.targetAuraStateNot != 0 || spellKnown(e.targetAuraSpell) ||
        spellKnown(e.excludeTargetAuraSpell)) {
        auto& em = owner_.getEntityManager();
        auto target = owner_.getTargetGuid() != 0 ? em.getEntity(owner_.getTargetGuid()) : nullptr;
        if (target && !target->isUnit()) target = nullptr;
        const uint32_t a0 = e.implicitTargetsA[0];
        const bool friendWanted = a0 == 21 || a0 == 35 || a0 == 57 || a0 == 61;
        if (owner_.isAutoSelfCast() &&
            (!target || (friendWanted && !relationsOf(owner_, player, target, 0).mayAssist)))
            target = player;
        if (!target) {
            if (e.targetAuraState != 0 || spellKnown(e.targetAuraSpell)) u.target = false;
        } else {
            const uint16_t st = fieldIndex(UF::UNIT_FIELD_AURASTATE);
            const uint32_t states = st != 0xFFFF ? target->getField(st) : 0u;
            const auto hasState = [&](uint32_t s) { return (states & (1u << ((s - 1) & 31))) != 0; };
            const auto* auras = target->getGuid() == owner_.getPlayerGuid() ? &playerAuras_
                                                                            : getUnitAuras(target->getGuid());
            const auto carries = [&](uint32_t id) {
                if (!auras) return false;
                for (const auto& a : *auras)
                    if (a.spellId == id) return true;
                return false;
            };
            const bool lets = c.ignoresAuraState;
            if (e.targetAuraState != 0 && !hasState(e.targetAuraState) && !lets) u.target = false;
            if (e.targetAuraStateNot != 0 && hasState(e.targetAuraStateNot) && !lets) u.target = false;
            if (spellKnown(e.targetAuraSpell) && !carries(e.targetAuraSpell) && !lets) u.target = false;
            if (spellKnown(e.excludeTargetAuraSpell) && carries(e.excludeTargetAuraSpell) && !lets) u.target = false;
            const auto r = relationsOf(owner_, player, target, 0);
            if (a0 == 6 && !r.mayAttack) u.target = false;
            if (a0 == 21 && !r.mayAssist) u.target = false;
            if (a0 == 35 && (!r.inParty || !r.mayAssist)) u.target = false;
            if ((a0 == 57 || a0 == 61) && (!r.inRaid || !r.mayAssist)) u.target = false;
        }
    }
    return cc::usable(checkSpellOf(e), c, u);
}

unit_target::Range SpellHandler::spellReach(uint32_t spellId, const Entity& caster, const Entity* unit,
                                            bool friendly) const {
    const auto& e = owner_.spellNameCacheRef().find(spellId)->second;
    const int k = friendly ? 1 : 0;
    unit_target::RangeInput in;
    in.attr = e.attr;
    in.hasRow = e.hasRangeRow;
    in.rowFlags = e.rangeFlags;
    in.rowMin = std::max(0.0f, e.rangeMin[k]);
    in.rowMax = std::max(0.0f, e.rangeMax[k]);
    in.casterReach = caster.isUnit() ? static_cast<const Unit&>(caster).getCombatReach() : 0.0f;
    if (unit && unit->isUnit()) {
        in.hasUnit = true;
        in.unitReach = static_cast<const Unit&>(*unit).getCombatReach();
        in.unitIsPlayer = unit->getType() == ObjectType::PLAYER;
        in.casterMoving = movingForReach(owner_, caster);
        in.unitMoving = movingForReach(owner_, *unit);
    }
    return unit_target::unitRange(in, [&](float m) {
        return spell_mods::apply(m, spellModifiers(spellId, spell_mods::kOpRange));
    });
}

std::optional<unit_target::Range> SpellHandler::spellRange(uint32_t spellId, uint64_t casterGuid) const {
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    const auto caster = owner_.getEntityManager().getEntity(casterGuid);
    if (it == owner_.spellNameCacheRef().end() || !caster) return std::nullopt;
    // 0x00801650 with no unit: the friendly row for a spell aimed at
    // friends (0x007fe1b0 answering 1), else the hostile one.
    return spellReach(spellId, *caster, nullptr, owner_.getSpellTargetKind(spellId) == 1);
}

std::optional<bool> SpellHandler::reachAnswer(uint32_t spellId, uint64_t casterGuid, uint64_t unitGuid) const {
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    auto& em = owner_.getEntityManager();
    const auto caster = em.getEntity(casterGuid);
    // 0x00809610: the unit asked, else the target.
    if (unitGuid == 0) unitGuid = owner_.getTargetGuid();
    auto unit = unitGuid != 0 ? em.getEntity(unitGuid) : nullptr;
    if (it == owner_.spellNameCacheRef().end() || !caster || !unit) return std::nullopt;
    const auto& e = it->second;
    // Targets (low half) and what the effects' ImplicitTargetA add; a pet
    // wanted (5) and no plain unit puts the pet in its place.
    uint32_t m = e.targetFlags & 0xFFFFu;
    bool pet = false;
    for (const uint32_t a : e.implicitTargetsA) {
        switch (a) {
            case 1: m &= ~0x400u; break;
            case 5: pet = true; m &= ~0x8000u; break;
            case 6: case 53: m |= 0x80u; break;
            case 21: case 45: m |= 0x100u; break;
            case 23: m |= 0x800u; break;
            case 25: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 74:
            case 75: m |= 0x2u; break;
            case 26: m |= 0x4000u; break;
            case 35: m |= 0x8u; break;
            case 57: case 61: m |= 0x4u; break;
            default: break;
        }
    }
    if (pet && (m & 0x2u) == 0) {
        unit = owner_.getPetGuid() != 0 ? em.getEntity(owner_.getPetGuid()) : nullptr;
        if (!unit) return std::nullopt;
        m |= 0x2u;
    }
    const auto inReach = [&](const Entity& u, bool friendly) {
        const auto range = spellReach(spellId, *caster, &u, friendly);
        const glm::vec3 d(u.getX() - caster->getX(), u.getY() - caster->getY(), u.getZ() - caster->getZ());
        return unit_target::reachFailure(glm::dot(d, d), range) == 0;  // 0x00803850
    };
    if (unit->getType() == ObjectType::GAMEOBJECT) {
        if ((m & 0x4800u) == 0) return std::nullopt;
        return inReach(*unit, owner_.getSpellTargetKind(spellId) == 1);
    }
    // A corpse is not asked about here.
    if (!unit->isUnit()) return std::nullopt;
    if ((e.attrEx5 & 0x800u) != 0) {
        // 0x007ffc40: the unit, or its own target standing for it.
        const uint64_t itsGuid = entityGuidField(*unit, UF::UNIT_FIELD_TARGET_LO);
        auto its = itsGuid != 0 ? em.getEntity(itsGuid) : nullptr;
        if (its && !its->isUnit()) its = nullptr;
        const auto pick = unit_target::retarget(m, relationsOf(owner_, caster, unit, e.attrEx6), its != nullptr,
                                                its ? relationsOf(owner_, caster, its, e.attrEx6)
                                                    : unit_target::Relations{});
        if (pick == unit_target::Retarget::None) return std::nullopt;
        if (pick == unit_target::Retarget::ItsTarget) unit = its;
    }
    const uint32_t flags = entityField(*unit, UF::UNIT_FIELD_FLAGS);
    if ((flags & 0x10000u) != 0 && (e.attrEx6 & 0x1000000u) == 0) return std::nullopt;
    if (e.targetCreatureType != 0) {
        const uint32_t type = creatureTypeOf(owner_, *unit);
        if (type == 0 || (e.targetCreatureType & (1u << ((type - 1) & 31))) == 0) return std::nullopt;
    }
    const auto& un = static_cast<const Unit&>(*unit);
    const bool dead = un.getHealth() == 0;
    if (dead) {
        if ((un.getDynamicFlags() & 0x20u) == 0 && (m & 0x8600u) == 0 && (e.attrEx2 & 0x1u) == 0) return std::nullopt;
    } else if ((m & 0x400u) != 0) {
        return std::nullopt;
    }
    const auto r = relationsOf(owner_, caster, unit, e.attrEx6);
    const bool taken = ((m & 0x8u) != 0 && r.inParty && r.mayAssist) || ((m & 0x4u) != 0 && r.inRaid && r.mayAssist) ||
                       ((m & 0x100u) != 0 && r.mayAssistEx6) || ((m & 0x80u) != 0 && r.mayAttack) ||
                       ((m & 0x2u) != 0 && (flags & 0x100u) == 0);
    const bool isPlayer = unit->getType() == ObjectType::PLAYER;
    if (!taken) {
        // An ally's corpse (0x8000): a dead player or pet the caster may
        // help; an enemy's (0x200): a dead player it may not.
        if ((m & 0x8000u) != 0 && dead && (isPlayer || entityField(*unit, UF::UNIT_FIELD_PETNUMBER) != 0)) {
            if (!r.mayAssist) return std::nullopt;
        } else if ((m & 0x200u) == 0 || !dead || !isPlayer || r.mayAssist) {
            return std::nullopt;
        }
    }
    // 0x00803850 at the friendly row for a unit the caster may help
    // (0x00801650).
    return inReach(*unit, r.mayAssist);
}

bool SpellHandler::playerAuraAffects(uint32_t auraType, uint32_t spellId) const {
    const auto own = owner_.spellNameCacheRef().find(spellId);
    if (own == owner_.spellNameCacheRef().end()) return false;
    const auto& spell = own->second;
    // 0x007283a0: an aura of the player's of the type, its effect on, whose
    // class mask meets the spell's (0x007fdec0).
    for (const auto& a : playerAuras_) {
        if (a.spellId == 0) continue;
        const auto it = owner_.spellNameCacheRef().find(a.spellId);
        if (it == owner_.spellNameCacheRef().end() || it->second.spellFamily != spell.spellFamily) continue;
        for (int k = 0; k < 3; ++k) {
            if (it->second.effectAuraIds[k] != auraType || (a.flags & (1u << k)) == 0) continue;
            for (int w = 0; w < 3; ++w) {
                if ((it->second.effectClassMask[k][w] & spell.spellFamilyFlags[w]) != 0) return true;
            }
        }
    }
    return false;
}

bool SpellHandler::unitHasAuraType(uint64_t guid, uint32_t auraType) const {
    const auto* auras = guid == owner_.getPlayerGuid() ? &playerAuras_ : getUnitAuras(guid);
    if (!auras) return false;
    for (const auto& a : *auras) {
        if (a.spellId == 0) continue;
        const auto it = owner_.spellNameCacheRef().find(a.spellId);
        if (it == owner_.spellNameCacheRef().end()) continue;
        for (int k = 0; k < 3; ++k)
            if ((a.flags & (1u << k)) != 0 && it->second.effectAuraIds[k] == auraType) return true;
    }
    return false;
}

bool SpellHandler::channelCostsMana(const Unit& unit) const {
    const uint16_t ch = fieldIndex(UF::UNIT_CHANNEL_SPELL);
    const uint32_t spell = ch != 0xFFFF ? unit.getField(ch) : 0u;
    if (spell == 0) return false;
    const auto it = owner_.spellNameCacheRef().find(spell);
    return it != owner_.spellNameCacheRef().end() && (it->second.attrEx2 & 0x2000000u) == 0 &&
           it->second.powerType == 0 && powerCost(spell, unit, 0) > 0;
}

bool SpellHandler::enoughPower(uint32_t spellId, const Unit& unit) const {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return true;
    // 0x008017e0: the cost against the power as it is predicted
    // (0x0071c2e0) - health under it, runes by 0x005ee5b0.
    const int32_t type = static_cast<int32_t>(it->second.powerType);
    const int32_t cost = powerCost(spellId, unit, 0);
    if (type == -2) return cost < static_cast<int32_t>(unit.getHealth());
    if (type == 5) return runeCost(spellId).paid;
    if (type >= 0 && type < 7) return owner_.unitPower(unit, type) >= cost;
    return true;
}

int32_t SpellHandler::powerCost(uint32_t spellId, const Entity& player, int depth) const {
    const auto own = owner_.spellNameCacheRef().find(spellId);
    if (own == owner_.spellNameCacheRef().end()) return 0;
    const auto& e = own->second;
    // 0x008012f0 for the player: ManaCost, plus ManaCostPerLevel for each
    // level past BaseLevel (to MaxLevel), plus ManaCostPercentage of the
    // base power (0x0071b770).
    const auto field = [&](UF f, uint32_t offset = 0) {
        const uint16_t i = fieldIndex(f);
        return i != 0xFFFF ? player.getField(static_cast<uint16_t>(i + offset)) : 0u;
    };
    int32_t level = static_cast<int32_t>(field(UF::UNIT_FIELD_LEVEL));
    if (e.maxLevel > 0 && e.maxLevel < level) level = e.maxLevel;
    if (level < 0) level = 0;
    int32_t cost = (level - e.baseLevel) * e.manaCostPerLevel + e.manaCost;
    if (e.manaCostPct != 0) {
        int32_t base = 0;
        switch (static_cast<int32_t>(e.powerType)) {
            case 0: base = static_cast<int32_t>(field(UF::UNIT_FIELD_BASE_MANA)); break;
            case -2: base = static_cast<int32_t>(field(UF::UNIT_FIELD_BASE_HEALTH)); break;
            case 1: case 6: base = 1000; break;
            case 2: case 3: base = 100; break;
            case 5: base = 1; break;
            default: break;
        }
        cost += base * e.manaCostPct / 100;
    }
    // Attributes 0x400: a hundredth of the form's swing, else the weapon's
    // (the off hand's with AttributesEx3 0x1000000), 2000 where it has none
    // to use.
    if ((e.attr & 0x400u) != 0) {
        uint32_t swing = 0;
        const uint8_t form = static_cast<uint8_t>(field(UF::UNIT_FIELD_BYTES_2) >> 24);
        if (const auto* row = owner_.shapeshiftFormRow(form)) swing = row->combatRoundTime;
        if (swing == 0) {
            const bool offhand = (e.attrEx3 & 0x1000000u) != 0;
            const uint64_t guid = owner_.getEquipSlotGuid(static_cast<int>(offhand ? EquipSlot::OFF_HAND : EquipSlot::MAIN_HAND));
            const auto it = owner_.onlineItemsRef().find(guid);
            const bool disarmed = offhand ? (field(UF::UNIT_FIELD_FLAGS_2) & 0x80u) != 0
                                          : (field(UF::UNIT_FIELD_FLAGS) & 0x200000u) != 0;
            const auto* info = it != owner_.onlineItemsRef().end() ? owner_.getItemInfo(it->second.entry) : nullptr;
            // 0x006d71a0: not broken.
            const bool usable = it != owner_.onlineItemsRef().end() &&
                                (it->second.maxDurability == 0 || it->second.curDurability != 0);
            swing = info && usable && !disarmed ? info->delayMs : 2000u;
        }
        cost += static_cast<int32_t>(swing / 100);
    }
    // The school's cost modifier and multiplier, the least of its schools
    // (0x005ee050, 0x005ee0b0).
    int32_t mod = 0;
    float mult = 0.0f;
    bool any = false;
    for (uint32_t school = 0; school < 7; ++school) {
        if ((e.schoolMask & (1u << school)) == 0) continue;
        const int32_t m = static_cast<int32_t>(field(UF::UNIT_FIELD_POWER_COST_MODIFIER, school));
        float x = 0.0f;
        const uint32_t raw = field(UF::UNIT_FIELD_POWER_COST_MULTIPLIER, school);
        std::memcpy(&x, &raw, sizeof x);
        if (!any || m < mod) mod = m;
        if (!any || x < mult) mult = x;
        any = true;
    }
    cost = static_cast<int32_t>(std::nearbyint(static_cast<float>(mod + cost) * (mult + 1.0f)));
    // Cost modifiers (op 14).
    const auto mods = spellModifiers(spellId, 14);
    if (mods.any) cost = (cost + mods.flat) * mods.pct / 100;
    // SUMMON_ALL_TOTEMS (97): with it the totems on its action buttons
    // (from 132 on, flagged AttributesEx7 0x20).
    if (depth == 0) {
        for (int k = 0; k < 3; ++k) {
            if (e.effectIds[k] != 97) continue;
            const uint32_t first = e.effectMiscValues[k], count = e.effectMiscValuesB[k];
            if (first >= 12 || first + count >= 13) continue;
            for (uint32_t n = 0; n < count; ++n) {
                const auto& slot = owner_.getActionBar()[132 + first + n];
                if (slot.type != ActionBarSlot::SPELL || slot.id == 0) continue;
                const auto t = owner_.spellNameCacheRef().find(slot.id);
                if (t != owner_.spellNameCacheRef().end() && (t->second.attrEx7 & 0x20u) != 0)
                    cost += powerCost(slot.id, player, depth + 1);
            }
        }
    }
    return cost < 1 ? 0 : cost;
}

bool SpellHandler::hasCastTime(uint32_t spellId, const Entity& player) const {
    const auto own = owner_.spellNameCacheRef().find(spellId);
    if (own == owner_.spellNameCacheRef().end()) return false;
    const auto& e = own->second;
    // 0x007ff180: SpellCastTimes' base and per level past BaseLevel, at
    // least its minimum, through the cast time modifiers (op 10), the
    // casting speed unless an ability, a trade spell or AttributesEx3
    // 0x20000000, half a second more for a ranged one.
    if (!castTimesLoaded_) {
        castTimesLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (auto dbc = am && am->isInitialized() ? am->loadDBC("SpellCastTimes.dbc") : nullptr;
            dbc && dbc->isLoaded() && dbc->getFieldCount() >= 4) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                castTimes_[dbc->getUInt32(i, 0)] = {static_cast<int32_t>(dbc->getUInt32(i, 1)),
                                                    static_cast<int32_t>(dbc->getUInt32(i, 2)),
                                                    static_cast<int32_t>(dbc->getUInt32(i, 3))};
            }
        }
    }
    const auto row = castTimes_.find(e.castingTimeIndex);
    if (row == castTimes_.end()) return false;
    const uint16_t levelIdx = fieldIndex(UF::UNIT_FIELD_LEVEL);
    int32_t level = levelIdx != 0xFFFF ? static_cast<int32_t>(player.getField(levelIdx)) : 0;
    if (e.maxLevel > 0 && e.maxLevel < level) level = e.maxLevel;
    int32_t t = (level - e.baseLevel) * row->second[1] + row->second[0];
    if (t < row->second[2]) t = row->second[2];
    const auto mods = spellModifiers(spellId, 10);
    if (mods.any) t = (t + mods.flat) * mods.pct / 100;
    if ((e.attr & 0x30u) == 0 && (e.attrEx3 & 0x20000000u) == 0 && t > 0) {
        const float speed = castSpeedOf(player);
        if (speed != 1.0f) t = static_cast<int32_t>(std::nearbyint(static_cast<float>(t) * speed));
    }
    if ((e.attr & 0x2u) != 0) t += 500;
    return t > 0;
}

cast_check::Caster SpellHandler::casterState(uint32_t spellId, const Entity& player) {
    const auto& e = owner_.spellNameCacheRef().find(spellId)->second;
    namespace cc = cast_check;
    const auto field = [&](UF f) {
        const uint16_t i = fieldIndex(f);
        return i != 0xFFFF ? player.getField(i) : 0u;
    };
    const auto& unit = static_cast<const Unit&>(player);
    cc::Caster c;
    c.mapType = owner_.currentMapInstanceType();
    // 0x00809000: the spell's history - its cooldown, its category's, its
    // global cooldown.
    c.notReady = playerCooldown(spellId).running;
    c.enoughPower = enoughPower(spellId, unit);
    // LEARN_SPELL's spell known, or one SkillLineAbility supersedes it with
    // (0x007260e0, 0x00726160).
    for (int k = 0; k < 3; ++k) {
        const uint32_t learn = e.effectTriggerSpell[k];
        if (e.effectIds[k] != 36 || learn == 0) continue;
        c.learnKnown[k] = knowsSpellOrSuccessor(learn);
    }
    c.hasCharm = entityGuidField(player, UF::UNIT_FIELD_CHARM) != 0;
    c.hasSummon = entityGuidField(player, UF::UNIT_FIELD_SUMMON) != 0;
    c.alive = unit.getHealth() > 0;
    c.playerBytes = static_cast<uint8_t>(field(UF::PLAYER_FIELD_BYTES) & 0xFFu);
    c.charmedByOther = entityGuidField(player, UF::UNIT_FIELD_CHARMEDBY) != 0;
    c.unitFlags = field(UF::UNIT_FIELD_FLAGS);
    // The holds' aura walks (0x00804430) over the player's auras.
    const cc::Spell cast = checkSpellOf(e);
    std::vector<cc::Spell> auraSpells;
    for (const auto& a : playerAuras_) {
        if (a.spellId == 0) continue;
        const auto it = owner_.spellNameCacheRef().find(a.spellId);
        if (it != owner_.spellNameCacheRef().end()) auraSpells.push_back(checkSpellOf(it->second));
    }
    const auto auras = [&](auto&& fn) {
        for (const auto& a : auraSpells)
            if (!fn(a)) return;
    };
    c.charm = cc::hold(cast, auras, {6u, 177u, 2u});
    c.stun = cc::hold(cast, auras, {12u});
    c.silence = cc::hold(cast, auras, {27u, 60u});
    c.pacify = cc::hold(cast, auras, {25u, 60u});
    c.fear = cc::hold(cast, auras, {7u});
    c.confuse = cc::hold(cast, auras, {5u});
    const uint32_t bytes2 = field(UF::UNIT_FIELD_BYTES_2);
    c.sheath = static_cast<uint8_t>(bytes2 & 0xFFu);
    c.mounted = owner_.isMounted();
    const bool flying = (owner_.movementInfoRef().flags & 0x2000000u) != 0;
    c.wouldDismount = unit_target::wouldDismount(c.unitFlags, flying,
                                                 addons::storedCVarValue("autoDismount", "1") != "0",
                                                 addons::storedCVarValue("autoDismountFlying", "0") != "0");
    const uint32_t bytes1 = field(UF::UNIT_FIELD_BYTES_1);
    c.standState = static_cast<uint8_t>(bytes1 & 0xFFu);
    c.wouldStand = addons::storedCVarValue("autoStand", "1") != "0";
    // 0x004c9850: 05:30 to 21:00 of the game's clock, in whole minutes.
    if (const float h = owner_.getGameTime(); h >= 0.0f) {
        const int minute = static_cast<int>(h * 60.0f) % (24 * 60);
        c.daytime = minute >= 5 * 60 + 30 && minute < 21 * 60 ? 1 : 0;
    }
    c.moveFlags = owner_.movementInfoRef().flags;
    c.hasCastTime = hasCastTime(spellId, player);
    c.form = static_cast<uint8_t>(bytes2 >> 24);
    if (const auto* row = owner_.shapeshiftFormRow(c.form)) {
        c.formRow = true;
        c.formFlags = row->flags;
    }
    c.ignoresForm = playerAuraAffects(275, spellId);
    // 0x0071b130: autoUnshift, off a taxi, not flying unless
    // autoDismountFlying, from a form not flagged 0x100.
    c.wouldUnshift = (c.unitFlags & 0x100000u) == 0 && addons::storedCVarValue("autoUnshift", "1") != "0" &&
                     (!flying || addons::storedCVarValue("autoDismountFlying", "0") != "0") &&
                     (!c.formRow || (c.formFlags & 0x100u) == 0);
    c.visFlags = static_cast<uint8_t>((bytes1 >> 16) & 0xFFu);
    c.ignoresAuraState = auraLetsSpell(owner_.getPlayerGuid(), spellId);
    c.auraState = field(UF::UNIT_FIELD_AURASTATE);
    const auto carries = [&](uint32_t id) {
        if (id == 0) return false;
        for (const auto& a : playerAuras_)
            if (a.spellId == id) return true;
        return false;
    };
    c.hasCasterAuraSpell = carries(e.casterAuraSpell);
    c.hasExcludedAuraSpell = carries(e.excludeCasterAuraSpell);
    // 0x005d05b0: the standing in MinFactionId, 0 where it has none.
    if (e.minFactionId != 0) {
        const uint32_t i = owner_.getRepListIdByFactionId(e.minFactionId);
        const auto& all = owner_.getInitialFactions();
        c.reputation = i < 0x80 && i < all.size() ? all[i].standing : 0;
    }
    // The area (0x00519ca0: the subzone, else the zone) in AreaGroupId's
    // chain (0x007199e0): its id or its parent's among a group's six.
    const auto* renderer = core::Application::getInstance().getRenderer();
    const auto* texts = renderer ? renderer->getZoneTexts() : nullptr;
    const uint32_t area = texts ? (texts->subzoneId != 0 ? texts->subzoneId : texts->zoneId) : 0;
    const uint32_t parent = texts && texts->subzoneId != 0 ? texts->zoneId : 0;
    if (static_cast<int32_t>(e.areaGroupId) > 0 && texts) {
        if (area == 0) {
            c.area = 1;
        } else {
            loadAreaGroups();
            int listed = 0;
            uint32_t firstArea = 0;
            bool in = false;
            for (uint32_t g = e.areaGroupId, n = 0; g != 0 && !in && n < 64; ++n) {
                const auto it = areaGroups_.find(g);
                if (it == areaGroups_.end()) break;
                if (n == 0) firstArea = it->second[0];
                for (int k = 0; k < 6 && it->second[k] != 0; ++k) {
                    ++listed;
                    if (it->second[k] == area || it->second[k] == parent) in = true;
                }
                g = it->second[6];
            }
            c.area = in ? 0 : (listed == 1 && firstArea != 0 ? 2 : 1);
            c.areaArg = c.area == 2 ? firstArea : 0;
        }
    }
    // 0x0071b7f0: indoors or out where the player stands (0x0077fbf0).
    if (renderer && renderer->getPlayerPlace().known) {
        const auto& place = renderer->getPlayerPlace();
        c.outdoors = outdoors::outdoors(place, owner_.getAreaFlags(place.groundArea)) ? 1 : 0;
    }
    // 0x007261b0: an area flagged for flying (0x400, not 0x20000000); in
    // Northrend (571) also Cold Weather Flying (54197), unless
    // AttributesEx7 0x400000.
    if ((e.attrEx4 & 0x4000000u) != 0 && texts) {
        const uint32_t flags = owner_.getAreaFlags(area);
        bool fly = area != 0 && (flags & 0x400u) != 0 && (flags & 0x20000000u) == 0;
        if (fly && (e.attrEx7 & 0x400000u) == 0 && owner_.getCurrentMapId() == 571 && knownSpells_.count(54197) == 0)
            fly = false;
        c.flyable = fly ? 1 : 0;
    }
    return c;
}

bool SpellHandler::knowsSpellOrSuccessor(uint32_t spellId) const {
    bool known = knownSpells_.count(spellId) != 0;
    for (uint32_t next = supersededBy(spellId), n = 0; !known && next != 0 && n < 32; next = supersededBy(next), ++n)
        known = knownSpells_.count(next) != 0;
    return known;
}

uint32_t SpellHandler::supersededBy(uint32_t spellId) const {
    // 0x00812410: the SkillLineAbility row for the player's race and class,
    // its SupercededBySpell (+0x20).
    if (!supersedeLoaded_) {
        supersedeLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (auto dbc = am && am->isInitialized() ? am->loadDBC("SkillLineAbility.dbc") : nullptr;
            dbc && dbc->isLoaded() && dbc->getFieldCount() > 8) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t next = dbc->getUInt32(i, 8);
                if (next != 0)
                    supersedes_.emplace(dbc->getUInt32(i, 2),
                                        std::array<uint32_t, 3>{dbc->getUInt32(i, 3), dbc->getUInt32(i, 4), next});
            }
        }
    }
    const uint32_t bytes0 = [&] {
        const auto p = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
        return p ? entityField(*p, UF::UNIT_FIELD_BYTES_0) : 0u;
    }();
    const uint32_t race = bytes0 & 0xFFu, cls = (bytes0 >> 8) & 0xFFu;
    const auto [lo, hi] = supersedes_.equal_range(spellId);
    for (auto it = lo; it != hi; ++it) {
        const auto& [raceMask, classMask, next] = it->second;
        if ((raceMask == 0 || (race != 0 && (raceMask & (1u << (race - 1))) != 0)) &&
            (classMask == 0 || (cls != 0 && (classMask & (1u << (cls - 1))) != 0)))
            return next;
    }
    return 0;
}

void SpellHandler::loadAreaGroups() const {
    if (areaGroupsLoaded_) return;
    areaGroupsLoaded_ = true;
    auto* am = owner_.services().assetManager;
    // AreaGroup.dbc: the id, six AreaIDs and NextAreaID.
    if (auto dbc = am && am->isInitialized() ? am->loadDBC("AreaGroup.dbc") : nullptr;
        dbc && dbc->isLoaded() && dbc->getFieldCount() >= 8) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            std::array<uint32_t, 7> row{};
            for (uint32_t k = 0; k < 7; ++k) row[k] = dbc->getUInt32(i, k + 1);
            areaGroups_[dbc->getUInt32(i, 0)] = row;
        }
    }
}

void SpellHandler::sendCastBy(uint32_t spellId, uint64_t caster, uint64_t target) {
    if (caster == owner_.getPlayerGuid()) sendPlayerCast(spellId, target);
    else sendPetCast(spellId, caster, target);
}

void SpellHandler::castAtUnits(uint32_t spellId, uint64_t caster, uint64_t targetGuid) {
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    const auto& spell = it->second;
    const uint64_t player = owner_.getPlayerGuid();
    // 0x0080c790: the targets it needs.
    const uint32_t required = ground_target::requiredTargets(spell.targetFlags, spell.implicitTargetsA[0], false);
    // The unit asked: the cast's, else the player's target.
    const uint64_t asked = targetGuid != 0 ? targetGuid : owner_.getTargetGuid();
    auto& em = owner_.getEntityManager();
    const auto unit = asked != 0 ? em.getEntity(asked) : nullptr;
    // One out of view goes as it is, for a spell reaching past 100 yards
    // (0x00802c30 with no unit).
    bool farReach = false;
    if (required != 0 && asked != 0 && !unit) {
        const auto casterEntity = em.getEntity(caster);
        unit_target::RangeInput in;
        in.attr = spell.attr;
        in.hasRow = spell.hasRangeRow;
        in.rowFlags = spell.rangeFlags;
        const int k = spellTargetKind(spell.targetFlags,
                                      {spell.implicitTargetsA[0], spell.implicitTargetsA[1], spell.implicitTargetsA[2]},
                                      {spell.implicitTargetsB[0], spell.implicitTargetsB[1], spell.implicitTargetsB[2]},
                                      {spell.effectAuraIds[0], spell.effectAuraIds[1], spell.effectAuraIds[2]}) == 1
                          ? 1 : 0;
        in.rowMin = std::max(0.0f, spell.rangeMin[k]);
        in.rowMax = std::max(0.0f, spell.rangeMax[k]);
        in.casterReach = casterEntity && casterEntity->isUnit()
                             ? static_cast<const Unit&>(*casterEntity).getCombatReach() : 0.0f;
        const auto range = unit_target::unitRange(in, [&](float m) {
            return spell_mods::apply(m, spellModifiers(spellId, spell_mods::kOpRange));
        });
        farReach = range.max > 100.0f;
    }
    // Auto self-cast with none asked, or a unit the caster may not help.
    bool helpless = asked == 0;
    if (!helpless && unit && unit->isUnit()) {
        const auto c = em.getEntity(caster);
        helpless = !c || !relationsOf(owner_, c, unit, 0).mayAssist;
    }
    const auto chosen = unit_target::chooseUnit(
        required, ground_target::takesNoUnit(spell.implicitTargetsA[0]), asked, farReach, owner_.isAutoSelfCast(),
        helpless, player, caster, [&](uint64_t guid) { return takeUnit(spellId, caster, guid, required, true); });
    switch (chosen.what) {
        case unit_target::Choice::AtNone: sendCastBy(spellId, caster, 0); return;
        case unit_target::Choice::AtUnit: sendCastBy(spellId, caster, chosen.unit); return;
        case unit_target::Choice::Partly: return;
        case unit_target::Choice::NoUnit: break;
    }
    // 0x0080cce0 with no unit: the cursor holds it, or it is refused - a
    // main hand looked for from the player alone.
    const bool byPlayer = caster == player;
    const bool mainhand = byPlayer && owner_.getEquipSlotGuid(static_cast<int>(EquipSlot::MAIN_HAND)) != 0;
    if (const uint8_t fail = unit_target::noUnitFailure(spell.attr, spell.attrEx5, required, byPlayer, mainhand,
                                                        owner_.getTargetGuid() != 0)) {
        const std::string text = castFailureMessage(owner_, spellId, fail, -1, 0, 0);
        if (byPlayer) showPlayerCastFailure(spellId, fail, text);
        else owner_.addUIError(text);
        return;
    }
    armUnitTarget(spellId, caster, required);
}

bool SpellHandler::cancelGroundTargeting() {
    if (groundTarget_.spellId == 0 && unitTarget_.spellId == 0) return false;
    LOG_INFO("Targeting: spell ", groundTarget_.spellId | unitTarget_.spellId, " put down");
    groundTarget_ = GroundTarget{};
    unitTarget_ = UnitTarget{};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURSOR_UPDATE", {});
    return true;
}

bool SpellHandler::turnOrCancelGroundTargeting() {
    // 0x0051fb00: one held for a unit is put down (0x00809a60).
    if (unitTarget_.spellId != 0) return cancelGroundTargeting();
    if (groundTarget_.spellId == 0) return false;
    if (!groundTarget_.objectFixedFacing) return cancelGroundTargeting();
    groundTarget_.objectFacing = ground_target::quarterTurn(groundTarget_.objectFacing);
    return true;
}

void SpellHandler::placeGroundTarget(const ground_target::Place& place) {
    if (groundTarget_.spellId == 0) return;
    // 0x0080c340: the source location first when both are wanted.
    const uint32_t filled = ground_target::locationFilledByClick(groundTarget_.required);
    if (filled == ground_target::kTargetFlagSourceLocation) groundTarget_.source = place.canonical;
    groundTarget_.required &= ~filled;
    if (groundTarget_.required != 0) return;
    const uint32_t spellId = groundTarget_.spellId;
    const uint64_t caster = groundTarget_.caster;
    groundTarget_ = GroundTarget{};
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURSOR_UPDATE", {});
    castAtLocation(spellId, place, caster);
}

float SpellHandler::groundTargetSpellArea() const {
    if (groundTarget_.spellId == 0) return 0.0f;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(groundTarget_.spellId);
    if (it == owner_.spellNameCacheRef().end()) return 0.0f;
    // 0x008019c0: the first two effects, grown by the caster's level, then
    // the spell's Radius modifiers.
    const float area = ground_target::spellRadius(it->second.effectRadius, it->second.effectRadiusPerLevel,
                                                  owner_.getPlayerLevel());
    return spell_mods::apply(area, spellModifiers(groundTarget_.spellId, spell_mods::kOpRadius));
}

spell_mods::Sum SpellHandler::spellModifiers(uint32_t spellId, uint8_t op) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return {};
    const uint32_t family = classSpellFamily();
    const auto& flat = owner_.spellFlatModsRef();
    const auto& pct = owner_.spellPctModsRef();
    const auto& e = it->second;
    return spell_mods::modifiers(e.spellFamily, e.spellFamilyFlags, e.attrEx3, family, op,
                                 [&](uint8_t bit, uint8_t o) {
                                     const GameHandler::SpellModKey key{.op = static_cast<GameHandler::SpellModOp>(o),
                                                                        .group = bit};
                                     const auto f = flat.find(key);
                                     const auto p = pct.find(key);
                                     return std::pair<int32_t, int32_t>{f != flat.end() ? f->second : 0,
                                                                        p != pct.end() ? p->second : 0};
                                 });
}

int SpellHandler::groundTargetPlacement(const glm::vec3& canonical) const {
    if (groundTarget_.spellId == 0) return static_cast<int>(ground_target::Placement::Unacceptable);
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(groundTarget_.spellId);
    if (it == owner_.spellNameCacheRef().end() || it->second.rangeMax[0] < 0.0f) {
        return static_cast<int>(ground_target::Placement::Acceptable);
    }
    const auto& e = it->second;
    // 0x00801650: the friendly range for a spell aimed at friends
    // (0x007fe1b0 answering 1), else the hostile one. A place is no unit, so
    // no combat reach is added (0x007ff480).
    const int k = spellTargetKind(e.targetFlags, {e.implicitTargetsA[0], e.implicitTargetsA[1], e.implicitTargetsA[2]},
                                  {e.implicitTargetsB[0], e.implicitTargetsB[1], e.implicitTargetsB[2]},
                                  {e.effectAuraIds[0], e.effectAuraIds[1], e.effectAuraIds[2]}) == 1 ? 1 : 0;
    // 0x007ff480: Attributes 0x404 is 0 to 100; else the table's, the
    // maximum through the spell's Range modifiers.
    float minRange = e.rangeMin[k];
    float maxRange = spell_mods::apply(e.rangeMax[k], spellModifiers(groundTarget_.spellId, spell_mods::kOpRange));
    if ((e.attr & 0x404u) != 0) {
        minRange = 0.0f;
        maxRange = 100.0f;
    }
    const auto& me = owner_.movementInfoRef();
    const glm::vec3 d = canonical - glm::vec3(me.x, me.y, me.z);
    return static_cast<int>(ground_target::placement(glm::dot(d, d), minRange, maxRange));
}

void SpellHandler::castAtLocation(uint32_t spellId, const ground_target::Place& place, uint64_t caster) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Sent where the click landed, whatever the range: the client only paints
    // the circle by it (0x00803ee0), and the server answers out of range. On
    // a transport, its guid and the place in its frame (0x009ab8b0); before
    // WotLK there is no transport to name, and the place is the world's.
    const bool onTransport = place.transportGuid != 0 && !isPreWotlk();
    const glm::vec3 wire = onTransport ? place.transportOffset : core::coords::canonicalToServer(place.canonical);
    const uint64_t transportGuid = onTransport ? place.transportGuid : 0;
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildCastSpellAtLocation(spellId, wire.x, wire.y, wire.z, ++castCount_,
                                                              transportGuid)
        : CastSpellPacket::buildDestination(spellId, wire.x, wire.y, wire.z, ++castCount_, transportGuid);
    // A pet's, by the pet (0x0080ac90's 0x1f0).
    owner_.getSocket()->send(caster != 0 ? CastSpellPacket::asPetCast(caster, packet) : packet);
    castSent(spellId, caster != 0 ? caster : owner_.getPlayerGuid());
    LOG_INFO("Casting spell: ", spellId, " at (", wire.x, ", ", wire.y, ", ", wire.z, ") transport 0x", std::hex,
             transportGuid, std::dec);
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_SENT", {"player", "", std::to_string(spellId)});
}

void SpellHandler::cancelChannelOnMove() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const auto player = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(owner_.getPlayerGuid()));
    const uint16_t ch = fieldIndex(UF::UNIT_CHANNEL_SPELL);
    const uint32_t spell = player && ch != 0xFFFF ? player->getField(ch) : 0u;
    if (spell == 0) return;
    const auto it = owner_.spellNameCacheRef().find(spell);
    if (it == owner_.spellNameCacheRef().end() || (it->second.channelInterruptFlags & 0x8u) == 0) return;
    if (wireOpcode(Opcode::CMSG_CANCEL_CHANNELLING) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_CHANNELLING));
    packet.writeUInt32(spell);
    owner_.getSocket()->send(packet);
}

void SpellHandler::cancelChannelOnTurn() {
    // 0x005fb0b0: a turn key going down (left or right, not both) starts a
    // turn through 0x0072e7e0, which ends the active player's channel when its
    // spell has ChannelInterruptFlags 0x10 (turning) and AttributesEx 0x4000
    // (Spell.dbc words 33 and 5): CMSG_CANCEL_CHANNELLING (0x008008d0).
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const auto player = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(owner_.getPlayerGuid()));
    const uint16_t ch = fieldIndex(UF::UNIT_CHANNEL_SPELL);
    const uint32_t spell = player && ch != 0xFFFF ? player->getField(ch) : 0u;
    if (spell == 0) return;
    const auto it = owner_.spellNameCacheRef().find(spell);
    if (it == owner_.spellNameCacheRef().end()) return;
    if ((it->second.channelInterruptFlags & 0x10u) == 0 || (it->second.attrEx & 0x4000u) == 0) return;
    if (wireOpcode(Opcode::CMSG_CANCEL_CHANNELLING) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_CHANNELLING));
    packet.writeUInt32(spell);
    owner_.getSocket()->send(packet);
}

void SpellHandler::cancelCast() {
    if (restorationActive_) {
        const uint32_t auraSpell = restorationSpellId_;
        cancelAura(auraSpell);
        owner_.setStandState(0);
        stopRestorationPresentation();
        LOG_INFO("Cancelled restoration aura: spellId=", auraSpell);
        return;
    }
    if (!casting_) return;
    // GameObject interaction cast is client-side timing only.
    if (owner_.pendingGameObjectInteractGuidRef() == 0 &&
        owner_.getState() == WorldState::IN_WORLD && owner_.getSocket() &&
        currentCastSpellId_ != 0) {
        auto packet = CancelCastPacket::build(currentCastSpellId_);
        owner_.getSocket()->send(packet);
    }
    owner_.pendingGameObjectInteractGuidRef() = 0;
    owner_.lastInteractedGoGuidRef() = 0;
    casting_ = false;
    castIsChannel_ = false;
    currentCastSpellId_ = 0;
    castTimeRemaining_ = 0.0f;
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_STOP", {"player"});
    // Remove lingering precast visual effects
    if (auto* renderer = owner_.services().renderer) {
        if (auto* svs = renderer->getSpellVisualSystem())
            svs->cancelAllPrecastVisuals();
    }
}

void SpellHandler::startCraftQueue(uint32_t spellId, int count) {
    // castSpell() dismounts a ground mount and then casts, so crafting while
    // mounted just works. But while airborne on a flying mount it blocks the
    // cast - guard that here too so we don't populate the queue with a cast
    // that will never fire (which would freeze the crafting UI on
    // "Crafting... N remaining").
    //
    // The same setting as the cast itself: with Auto Dismount in Flight on the
    // cast does fire, so refusing to queue it here would leave crafting the one
    // thing still blocked in the air.
    if (owner_.isMounted() && owner_.isPlayerFlying() &&
        addons::storedCVarValue("autoDismountFlying", "0") == "0") {
        // The cast's own refusal, SPELL_FAILED_NOT_MOUNTED (see castSpell).
        owner_.addUIError(castFailureMessage(owner_, spellId, 0x40, -1));
        return;
    }
    craftQueueSpellId_ = spellId;
    craftQueueRemaining_ = count;
    // castSpell has several silent refusals - already casting, a cast-time
    // spell while moving, a mount - and each of them leaves the queue set with
    // nothing sent, which reads as the Create button doing nothing.
    LOG_WARNING("Craft: queued spell ", spellId, " x", count, ", casting now");
    castSpell(spellId, 0);
}

void SpellHandler::cancelCraftQueue() {
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;
}

void SpellHandler::cancelAura(uint32_t spellId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // 0x00802f80: the aura of a form the player is in. The form is left at
    // once (0x00721cf0: +0x9f4 set, UPDATE_SHAPESHIFT_FORM) and stays left
    // until the server changes the form field (0x006da130).
    if (!isPreWotlk()) {
        switch (owner_.formAuraCancelFor(spellId)) {
        case FormAuraCancel::Refused:
            return;
        case FormAuraCancel::SendFormPending:
            owner_.setShapeshiftFormPending();
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("UPDATE_SHAPESHIFT_FORM", {});
            break;
        case FormAuraCancel::Send:
            break;
        }
    }
    auto packet = CancelAuraPacket::build(spellId);
    owner_.getSocket()->send(packet);
}

void SpellHandler::learnTalent(uint32_t talentId, uint32_t requestedRank) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        LOG_WARNING("learnTalent: Not in world or no socket connection");
        return;
    }

    LOG_INFO("Requesting to learn talent: id=", talentId, " rank=", requestedRank);

    auto packet = LearnTalentPacket::build(talentId, requestedRank);
    owner_.getSocket()->send(packet);
}

void SpellHandler::learnPreviewTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talentRanks) {
    if (talentRanks.empty() || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    owner_.getSocket()->send(LearnPreviewTalentsPacket::build(talentRanks));
}

void SpellHandler::openCraftingWindow(uint32_t skillLine) {
    craftingWindowOpen_ = true;
    craftingSkillLine_ = skillLine;
    owner_.fireAddonEvent("TRADE_SKILL_SHOW", {});
}

void SpellHandler::closeCraftingWindow() {
    if (!craftingWindowOpen_) return;
    craftingWindowOpen_ = false;
    owner_.fireAddonEvent("TRADE_SKILL_CLOSE", {});
}

void SpellHandler::switchTalentSpec(uint8_t newSpec) {
    if (newSpec > 1) {
        LOG_WARNING("Invalid talent spec: ", (int)newSpec);
        return;
    }
    if (newSpec == activeTalentSpec_) {
        LOG_INFO("Already on spec ", (int)newSpec);
        return;
    }
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    // Switching spec is a spell cast, not a message. AzerothCore lists
    // CMSG_SET_ACTIVE_TALENT_GROUP_OBSOLETE as Handle_NULL - it reads the
    // packet and does nothing - and the effect that actually moves a player
    // between specs is SPELL_EFFECT_TALENT_SPEC_SELECT, cast at themselves.
    //
    // This sent that dead opcode and then set the active spec locally anyway.
    // So the client believed it was on the second spec while the server had
    // never left the first, and everything the talent frame reads came from
    // the second spec's slots - which the server never fills, because it keeps
    // reporting the first. That is both halves of what was seen: a spec that
    // switched itself, and a first spec that never received the points earned
    // by levelling.
    //
    // Named the way Spell.dbc names them, which is the opposite way round from
    // how the ids read: 63645 is the primary and 63644 is the secondary.
    constexpr uint32_t kActivatePrimarySpec = 63645;
    constexpr uint32_t kActivateSecondarySpec = 63644;
    const uint32_t spellId = (newSpec == 0) ? kActivatePrimarySpec
                                            : kActivateSecondarySpec;

    // Both are taught when dual specialisation is bought, so not knowing one
    // means the character has only the single spec.
    if (!knownSpells_.count(spellId)) {
        // Silent, as SetActiveTalentGroup (0x005c5e70) is for a group the
        // character does not have. This said so in English, twice over.
        return;
    }

    castSpell(spellId, owner_.getPlayerGuid());
    LOG_INFO("Activating talent spec ", (int)newSpec, " by casting ", spellId);
    // Deliberately not set here. The server answers a successful switch with
    // SMSG_TALENTS_INFO naming the group it moved to, and that is the only
    // thing that should move this - a local guess is what caused the drift.
}

void SpellHandler::confirmTalentWipe() {
    if (!talentWipePending_) return;
    talentWipePending_ = false;

    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    network::Packet pkt(wireOpcode(Opcode::MSG_TALENT_WIPE_CONFIRM));
    pkt.writeUInt64(talentWipeNpcGuid_);
    owner_.getSocket()->send(pkt);

    LOG_INFO("confirmTalentWipe: sent confirm for npc=0x", std::hex, talentWipeNpcGuid_, std::dec);
    // No line: ConfirmTalentWipe (0x0051bf50) only sends.
    talentWipeNpcGuid_ = 0;
    talentWipeCost_ = 0;
}

void SpellHandler::confirmPetUnlearn() {
    if (!petUnlearnPending_) return;
    petUnlearnPending_ = false;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;

    // Sent, and this server throws it away: CMSG_PET_UNLEARN_TALENTS is
    // Handle_NULL in AzerothCore's opcode table, and pet talents are only ever
    // reset there through the AT_LOGIN_RESET_PET_TALENTS flag at the next
    // login. Kept rather than dropped, because the request is correct and a
    // server that implements it would act on it - but the line that followed
    // said "Pet talent reset confirmed", which is a claim about what happened
    // rather than about what was asked, and nothing was going to happen.
    //
    // The talent reset beside this one is the contrast worth keeping: it goes
    // out on MSG_TALENT_WIPE_CONFIRM, which the server does implement, and its
    // line already says the server will update the talents rather than that it
    // has.
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_UNLEARN_TALENTS));
    owner_.getSocket()->send(pkt);
    LOG_INFO("confirmPetUnlearn: sent CMSG_PET_UNLEARN_TALENTS");
    // No line: the 3.3.5 interface has no pet unlearn confirmation at all,
    // let alone words for one.
    petUnlearnCost_ = 0;
}

const std::vector<SpellHandler::SpellBookTab>& SpellHandler::getSpellBookTabs() {
    // Must be an instance member, not static - a static is shared across all
    // SpellHandler instances, so switching characters with the same spell count
    // would skip the rebuild and return the previous character's tabs.
    if (lastSpellCount_ == knownSpells_.size() && !spellBookTabsDirty_)
        return spellBookTabs_;
    lastSpellCount_ = knownSpells_.size();
    spellBookTabsDirty_ = false;
    spellBookTabs_.clear();

    // Which SkillLine.dbc categories earn a tab of their own.
    //
    // Only the class one did, so everything else fell into General - and what
    // falls into General is not a handful of odds and ends but every recipe
    // the character knows. A miner's General tab listed Smelt Copper, Smelt
    // Tin, Smelt Silver and the rest beside Hearthstone, and a tailor's ran to
    // a hundred entries. The recipes belong under the skill that makes them.
    static constexpr uint32_t SKILLLINE_CATEGORY_CLASS     = 7;
    static constexpr uint32_t SKILLLINE_CATEGORY_SECONDARY = 9;   // Cooking, First Aid, Fishing
    static constexpr uint32_t SKILLLINE_CATEGORY_PROFESSION = 11; // Mining, Tailoring, ...
    // Category 9 is not only Cooking, First Aid and Fishing: it also holds
    // every racial and every riding skill - Dwarven Racial, Horse Riding, Kodo
    // Riding, twenty-four lines in all. Tabbing the category wholesale would
    // put a tab named "Kodo Riding" beside Mining. So a secondary line earns
    // one only when the character knows something in it that is actually made:
    // a spell with reagents or a created item. That is the same question the
    // General tab was answering wrongly, asked once here instead.
    const auto lineHasRecipe = [this](uint32_t skillLine) {
        for (uint32_t known : knownSpells_) {
            auto kslIt = owner_.spellToSkillLineRef().find(known);
            if (kslIt == owner_.spellToSkillLineRef().end() ||
                kslIt->second != skillLine) continue;
            auto cacheIt = owner_.spellNameCacheRef().find(known);
            if (cacheIt == owner_.spellNameCacheRef().end()) continue;
            if (cacheIt->second.createdItemId != 0) return true;
            for (const auto& reagent : cacheIt->second.reagents) {
                if (reagent.itemId != 0) return true;
            }
        }
        return false;
    };
    const auto earnsOwnTab = [&](uint32_t category, uint32_t skillLine) {
        if (category == SKILLLINE_CATEGORY_CLASS) return true;
        if (category == SKILLLINE_CATEGORY_PROFESSION) return true;
        if (category == SKILLLINE_CATEGORY_SECONDARY) return lineHasRecipe(skillLine);
        return false;
    };

    std::map<uint32_t, std::vector<uint32_t>> bySkillLine;
    std::vector<uint32_t> general;

    for (uint32_t spellId : knownSpells_) {
        auto slIt = owner_.spellToSkillLineRef().find(spellId);
        if (slIt != owner_.spellToSkillLineRef().end()) {
            uint32_t skillLineId = slIt->second;
            auto catIt = owner_.skillLineCategoriesRef().find(skillLineId);
            if (catIt != owner_.skillLineCategoriesRef().end() &&
                earnsOwnTab(catIt->second, skillLineId)) {
                bySkillLine[skillLineId].push_back(spellId);
                continue;
            }
        }
        general.push_back(spellId);
    }

    auto byName = [this](uint32_t a, uint32_t b) {
        return owner_.getSpellName(a) < owner_.getSpellName(b);
    };

    if (!general.empty()) {
        std::sort(general.begin(), general.end(), byName);
        spellBookTabs_.push_back({.name = "General", .texture = "Interface\\Icons\\INV_Misc_Book_09", .spellIds = std::move(general)});
    }

    // Each tab's own picture, from SkillLine.dbc's icon column. Every tab used
    // to carry the same book, so the row of tabs down the side of the
    // spellbook was a column of identical squares that said nothing about
    // which was which - the tooltip was the only way to tell them apart.
    //
    // The book stays as the fallback: a skill line with no icon, or an icon
    // this install cannot resolve, keeps what it had rather than going blank.
    static constexpr const char* kDefaultTabIcon = "Interface\\Icons\\INV_Misc_Book_09";
    auto tabIcon = [this](uint32_t skillLineId) -> std::string {
        auto it = owner_.skillLineIconsRef().find(skillLineId);
        if (it == owner_.skillLineIconsRef().end()) return kDefaultTabIcon;
        std::string path = owner_.getIconPath(it->second);
        return path.empty() ? kDefaultTabIcon : path;
    };

    // Down the side of the book in the order the kinds belong in, not in one
    // alphabetical run: General, then the class's own lines, then the crafting
    // ones. Sorting every tab by name alone dealt them together - Affliction,
    // Alchemy, Arms, Blacksmithing - so a warlock's specialisations and their
    // professions read as one undifferentiated column.
    //
    // Alphabetical within each kind, which is what the sort was right about.
    const auto tabRank = [&](uint32_t skillLineId) {
        auto catIt = owner_.skillLineCategoriesRef().find(skillLineId);
        if (catIt == owner_.skillLineCategoriesRef().end()) return 3;
        switch (catIt->second) {
            case SKILLLINE_CATEGORY_CLASS:      return 0;
            case SKILLLINE_CATEGORY_PROFESSION: return 1;
            case SKILLLINE_CATEGORY_SECONDARY:  return 2;
            default:                            return 3;
        }
    };

    struct NamedTab {
        int rank;
        std::string name;
        std::string icon;
        std::vector<uint32_t> spells;
    };
    std::vector<NamedTab> named;
    for (auto& [skillLineId, spells] : bySkillLine) {
        auto nameIt = owner_.skillLineNamesRef().find(skillLineId);
        std::string tabName = (nameIt != owner_.skillLineNamesRef().end()) ? nameIt->second : "Unknown";
        std::sort(spells.begin(), spells.end(), byName);
        named.push_back({.rank = tabRank(skillLineId), .name = std::move(tabName), .icon = tabIcon(skillLineId),
                         .spells = std::move(spells)});
    }
    std::sort(named.begin(), named.end(), [](const NamedTab& a, const NamedTab& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.name < b.name;
    });

    for (auto& tab : named) {
        spellBookTabs_.push_back({.name = std::move(tab.name), .texture = std::move(tab.icon),
                                  .spellIds = std::move(tab.spells)});
    }

    return spellBookTabs_;
}

void SpellHandler::loadTalentDbc() {
    if (talentDbcLoaded_) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    talentDbcLoaded_ = true;

    // Load Talent.dbc
    auto talentDbc = am->loadDBC("Talent.dbc");
    if (talentDbc && talentDbc->isLoaded()) {
        const auto* talL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Talent") : nullptr;
        const uint32_t tID = talL ? (*talL)["ID"] : 0;
        const uint32_t tTabID = talL ? (*talL)["TabID"] : 1;
        const uint32_t tRow = talL ? (*talL)["Row"] : 2;
        const uint32_t tCol = talL ? (*talL)["Column"] : 3;
        const uint32_t tRank0 = talL ? (*talL)["RankSpell0"] : 4;
        const uint32_t tPrereq0 = talL ? (*talL)["PrereqTalent0"] : 9;
        const uint32_t tPrereqR0 = talL ? (*talL)["PrereqRank0"] : 12;

        uint32_t count = talentDbc->getRecordCount();
        for (uint32_t i = 0; i < count; ++i) {
            TalentEntry entry;
            entry.talentId = talentDbc->getUInt32(i, tID);
            if (entry.talentId == 0) continue;

            entry.tabId = talentDbc->getUInt32(i, tTabID);
            entry.row = static_cast<uint8_t>(talentDbc->getUInt32(i, tRow));
            entry.column = static_cast<uint8_t>(talentDbc->getUInt32(i, tCol));

            for (int r = 0; r < 5; ++r) {
                entry.rankSpells[r] = talentDbc->getUInt32(i, tRank0 + r);
            }

            for (int p = 0; p < 3; ++p) {
                entry.prereqTalent[p] = talentDbc->getUInt32(i, tPrereq0 + p);
                entry.prereqRank[p] = static_cast<uint8_t>(talentDbc->getUInt32(i, tPrereqR0 + p));
            }

            entry.maxRank = 0;
            for (int r = 0; r < 5; ++r) {
                if (entry.rankSpells[r] != 0) {
                    entry.maxRank = r + 1;
                }
            }

            talentCache_[entry.talentId] = entry;
        }
        LOG_INFO("Loaded ", talentCache_.size(), " talents from Talent.dbc");
    } else {
        LOG_WARNING("Could not load Talent.dbc");
    }

    // Load TalentTab.dbc
    auto tabDbc = am->loadDBC("TalentTab.dbc");
    if (tabDbc && tabDbc->isLoaded()) {
        const auto* ttL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TalentTab") : nullptr;
        // Cache field indices before the loop
        const uint32_t ttIdField    = ttL ? (*ttL)["ID"]             : 0;
        const uint32_t ttNameField  = ttL ? (*ttL)["Name"]           : 1;
        const uint32_t ttClassField = ttL ? (*ttL)["ClassMask"]      : 20;
        const uint32_t ttOrderField = ttL ? (*ttL)["OrderIndex"]     : 22;
        const uint32_t ttBgField    = ttL ? (*ttL)["BackgroundFile"] : 23;

        uint32_t count = tabDbc->getRecordCount();
        for (uint32_t i = 0; i < count; ++i) {
            TalentTabEntry entry;
            entry.tabId = tabDbc->getUInt32(i, ttIdField);
            if (entry.tabId == 0) continue;

            entry.name = tabDbc->getString(i, ttNameField);
            entry.classMask = tabDbc->getUInt32(i, ttClassField);
            entry.orderIndex = static_cast<uint8_t>(tabDbc->getUInt32(i, ttOrderField));
            entry.backgroundFile = tabDbc->getString(i, ttBgField);

            talentTabCache_[entry.tabId] = entry;

            if (talentTabCache_.size() <= 10) {
                LOG_INFO("  Tab ", entry.tabId, ": ", entry.name, " (classMask=0x", std::hex, entry.classMask, std::dec, ")");
            }
        }
        LOG_INFO("Loaded ", talentTabCache_.size(), " talent tabs from TalentTab.dbc");
    } else {
        LOG_WARNING("Could not load TalentTab.dbc");
    }

    syncPreWotlkTalentsFromKnownSpells();
}

void SpellHandler::syncPreWotlkTalentsFromKnownSpells() {
    if (!isPreWotlk() || talentCache_.empty() || knownSpells_.empty()) return;

    std::unordered_map<uint32_t, uint8_t> derived;
    uint32_t spentPoints = 0;
    for (const auto& [talentId, talent] : talentCache_) {
        uint8_t rankKnown = 0;
        for (int rank = 0; rank < 5; ++rank) {
            uint32_t rankSpell = talent.rankSpells[rank];
            if (rankSpell != 0 && knownSpells_.count(rankSpell) > 0) {
                rankKnown = static_cast<uint8_t>(rank + 1);
            }
        }
        if (rankKnown > 0) {
            derived[talentId] = rankKnown;
            spentPoints += rankKnown;
        }
    }

    const uint32_t playerLevel = owner_.getPlayerLevel();
    const uint32_t earnedPoints = (playerLevel > 9)
        ? std::min<uint32_t>(playerLevel - 9, 61u)
        : 0u;
    const uint8_t unspent = static_cast<uint8_t>(
        earnedPoints > spentPoints ? std::min<uint32_t>(earnedPoints - spentPoints, 255u) : 0u);

    if (learnedTalents_[0] == derived && activeTalentSpec_ == 0 &&
        unspentTalentPoints_[0] == unspent) {
        return;
    }

    activeTalentSpec_ = 0;
    learnedTalents_[0] = std::move(derived);
    learnedTalents_[1].clear();
    learnedGlyphs_[0].fill(0);
    learnedGlyphs_[1].fill(0);
    unspentTalentPoints_[0] = unspent;
    unspentTalentPoints_[1] = 0;

    LOG_INFO("[pre-WotLK] Derived ", learnedTalents_[0].size(),
             " learned talent(s) from known spells; spent=", spentPoints,
             " unspent=", static_cast<int>(unspent));

    if (owner_.addonEventCallbackRef()) {
        // Two arguments, both zero: the interface reads the second as a count
        // of skill points just gained and compares it against zero, which is an
        // error against nothing. These fire on a full refresh rather than an
        // increment, so nothing was gained and nothing is announced.
        owner_.addonEventCallbackRef()("CHARACTER_POINTS_CHANGED", {"0", "0"});
        owner_.addonEventCallbackRef()("PLAYER_TALENT_UPDATE", {});
    }
}

void SpellHandler::updateTimers(float dt) {
    // 0x00804c10: the missiles that have arrived leave the list.
    missilesInFlight_.expire(historyNowMs());
    // Tick down cast bar
    if (casting_ && castTimeRemaining_ > 0.0f) {
        castTimeRemaining_ -= dt;
        if (castTimeRemaining_ < 0.0f) castTimeRemaining_ = 0.0f;
    }
    // Eating/drinking munch-gulp repeats for the whole restoration aura,
    // matching the real client's looping consume sound.
    if (restorationActive_) {
        if (restorationTimeRemaining_ > 0.0f) {
            restorationTimeRemaining_ = std::max(0.0f, restorationTimeRemaining_ - dt);
        }
        restorationSoundTimer_ -= dt;
        if (restorationSoundTimer_ <= 0.0f) {
            restorationSoundTimer_ = 1.0f;
            if (auto* ac = owner_.services().audioCoordinator) {
                if (auto* sfx = ac->getUiSoundManager()) {
                    if (restorationIsFood_) sfx->playEating();
                    else                    sfx->playDrinking();
                }
            }
        }
    }
    // Tick down unit cast states
    for (auto it = unitCastStates_.begin(); it != unitCastStates_.end(); ) {
        if (it->second.casting && it->second.timeRemaining > 0.0f) {
            it->second.timeRemaining -= dt;
            if (it->second.timeRemaining <= 0.0f) {
                it->second.timeRemaining = 0.0f;
                it->second.casting = false;
                it = unitCastStates_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void SpellHandler::stopRestorationPresentation() {
    if (!restorationActive_) return;
    const uint32_t stoppedSpell = restorationSpellId_;
    if (owner_.spellCastAnimCallbackRef()) {
        owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), false, true,
                                          SpellCastType::OMNI);
    }
    restorationActive_ = false;
    restorationSpellId_ = 0;
    restorationTimeRemaining_ = 0.0f;
    restorationTimeTotal_ = 0.0f;
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_CHANNEL_STOP",
                                       {"player", std::to_string(stoppedSpell)});
    }
}

void SpellHandler::refreshRestorationFromPlayerAuras() {
    owner_.loadSpellNameCache();
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    const AuraSlot* restorationAura = nullptr;
    bool restorationIsFood = false;
    for (const auto& aura : playerAuras_) {
        if (aura.isEmpty()) continue;
        auto nameIt = owner_.spellNameCacheRef().find(aura.spellId);
        if (nameIt == owner_.spellNameCacheRef().end()) continue;
        const auto kind = spellclass::classifyRestChannel(nameIt->second.name);
        if (kind == spellclass::RestChannelKind::FOOD ||
            kind == spellclass::RestChannelKind::DRINK) {
            restorationAura = &aura;
            restorationIsFood = kind == spellclass::RestChannelKind::FOOD;
            break;
        }
    }

    if (!restorationAura) {
        stopRestorationPresentation();
        return;
    }

    const uint32_t spellId = restorationAura->spellId;
    const bool newlyActive = !restorationActive_ || restorationSpellId_ != spellId;
    if (restorationActive_ && restorationSpellId_ != spellId) {
        stopRestorationPresentation();
    }

    int32_t totalMs = restorationAura->maxDurationMs;
    if (totalMs <= 0) totalMs = restorationAura->durationMs;
    if (totalMs <= 0) {
        const float dbcSeconds = getSpellDuration(spellId);
        totalMs = dbcSeconds > 0.0f ? static_cast<int32_t>(dbcSeconds * 1000.0f) : 30000;
    }
    int32_t remainingMs = restorationAura->getRemainingMs(nowMs);
    if (remainingMs < 0) {
        remainingMs = (!newlyActive && restorationTimeRemaining_ > 0.0f)
            ? static_cast<int32_t>(restorationTimeRemaining_ * 1000.0f)
            : totalMs;
    }

    restorationActive_ = true;
    restorationSpellId_ = spellId;
    restorationIsFood_ = restorationIsFood;
    restorationTimeTotal_ = static_cast<float>(totalMs) / 1000.0f;
    restorationTimeRemaining_ = static_cast<float>(remainingMs) / 1000.0f;

    if (newlyActive) {
        restorationSoundTimer_ = 0.0f;
        LOG_INFO("Restoration aura started: spellId=", spellId,
                 " durationMs=", totalMs);
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), true, true,
                                              SpellCastType::OMNI);
        }
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("UNIT_SPELLCAST_CHANNEL_START",
                                           {"player", std::to_string(spellId)});
        }
    }
}

// ============================================================
// Packet handlers
// ============================================================

void SpellHandler::handleInitialSpells(network::Packet& packet) {
    InitialSpellsData data;
    if (!owner_.getPacketParsers()->parseInitialSpells(packet, data)) return;

    knownSpells_ = {data.spellIds.begin(), data.spellIds.end()};

    LOG_DEBUG("Initial spells include: 527=", knownSpells_.count(527u),
              " 988=", knownSpells_.count(988u), " 1180=", knownSpells_.count(1180u));

    // Ensure Attack and Hearthstone are always present
    knownSpells_.insert(SPELL_ID_ATTACK);
    knownSpells_.insert(SPELL_ID_HEARTHSTONE);
    if (isPreWotlk()) {
        loadTalentDbc();
        syncPreWotlkTalentsFromKnownSpells();
    }

    // SMSG_INITIAL_SPELLS (0x006df050 -> 0x00805b10): each cooldown into the
    // player's history, begun as long ago as the spell's own (through the
    // modifiers) is longer than what is left; the category's top bit holds
    // it until an event.
    playerHistory_.clear();
    {
        loadSpellNameCache();
        const int64_t now = historyNowMs();
        for (const auto& cd : data.cooldowns) {
            const auto it = owner_.spellNameCacheRef().find(cd.spellId);
            if (it == owner_.spellNameCacheRef().end()) continue;
            auto h = spell_history::fromRemaining(
                cd.spellId, cd.categoryId, static_cast<int32_t>(cd.cooldownMs),
                static_cast<int32_t>(cd.categoryCooldownMs & 0x7FFFFFFFu), (cd.categoryCooldownMs & 0x80000000u) != 0,
                static_cast<int32_t>(it->second.recoveryMs), categoryCooldown(cd.spellId, false), now,
                [&](int32_t ms) { return moddedCooldown(cd.spellId, ms); });
            h.itemId = cd.itemId;
            playerHistory_.add(h);
        }
    }

    // Load saved action bar or use defaults
    owner_.actionBarRef()[0].type = ActionBarSlot::SPELL;
    owner_.actionBarRef()[0].id = SPELL_ID_ATTACK;
    owner_.actionBarRef()[11].type = ActionBarSlot::SPELL;
    owner_.actionBarRef()[11].id = 8690;  // Hearthstone
    owner_.loadCharacterConfig();

    // Pre-load skill line DBCs
    owner_.loadSkillLineDbc();
    owner_.loadSkillLineAbilityDbc();

    LOG_INFO("Learned ", knownSpells_.size(), " spells");

    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("SPELLS_CHANGED", {});
        // The stance bar's list, built from the spellbook (event 0x178).
        if (!knownShapeshiftForms(owner_.getPlayerClass(), knownSpells_).empty()) {
            owner_.addonEventCallbackRef()("UPDATE_SHAPESHIFT_FORMS", {});
        }
        // A crafting window's recipes are the spells that just changed, and
        // it refreshes on its own event rather than on that one.
        if (craftingWindowOpen_) owner_.addonEventCallbackRef()("TRADE_SKILL_UPDATE", {});
    }
}

void SpellHandler::handleCastFailed(network::Packet& packet) {
    CastFailedData data;
    bool ok = owner_.getPacketParsers() ? owner_.getPacketParsers()->parseCastFailed(packet, data)
                                    : CastFailedParser::parse(packet, data);
    if (!ok) return;

    if (spellclass::isFishingCast(data.spellId)) {
        const auto& movement = owner_.movementInfoRef();
        LOG_WARNING("Fishing cast failed: spell=", data.spellId,
                    " result=", static_cast<int>(data.result),
                    " pos=(", movement.x, ",", movement.y, ",", movement.z, ")",
                    " facing=", movement.orientation,
                    " selectedTarget=0x", std::hex, owner_.getTargetGuid(), std::dec);
    }

    const uint64_t gatherGoGuid = owner_.lastInteractedGoGuidRef();
    const bool gatherCast = gatherGoGuid != 0 && isGatherSpellId(data.spellId);

    // 0x00809af0 fails the pending cast the count names (0x008051d0), not
    // whatever is casting: a spell sent during a cast and refused
    // SPELL_IN_PROGRESS leaves the running cast as it was.
    const bool otherCast =
        !cast_check::failureEndsRunningCast(casting_, data.result, data.spellId == currentCastSpellId_);
    if (!otherCast) {
        casting_ = false;
        castIsChannel_ = false;
        currentCastSpellId_ = 0;
        castTimeRemaining_ = 0.0f;
        owner_.lastInteractedGoGuidRef() = 0;
        owner_.pendingGameObjectInteractGuidRef() = 0;
        craftQueueSpellId_ = 0;
        craftQueueRemaining_ = 0;
        // Remove lingering precast visual effects
        if (auto* renderer = owner_.services().renderer) {
            if (auto* svs = renderer->getSpellVisualSystem())
                svs->cancelAllPrecastVisuals();
        }

        // Stop precast sound
        if (auto* ac = owner_.services().audioCoordinator) {
            if (auto* ssm = ac->getSpellSoundManager()) {
                ssm->stopPrecast();
            }
        }
    }

    // Show failure reason
    int powerType = -1;
    auto playerEntity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (auto playerUnit = std::dynamic_pointer_cast<Unit>(playerEntity)) {
        powerType = playerUnit->getPowerType();
    }
    // 0x00809af0: the cast's global cooldown is taken off (0x00808200 ->
    // 0x00804720), and a spell held until an event (Attributes 0x2000000)
    // leaves the history, unless refused as NOT_READY (0x00802970).
    {
        const int64_t now = historyNowMs();
        if (data.result != kSpellFailedNotReady) {
            loadSpellNameCache();
            const auto it = owner_.spellNameCacheRef().find(data.spellId);
            if (it != owner_.spellNameCacheRef().end() && (it->second.attr & 0x2000000u) != 0)
                playerHistory_.event(data.spellId, now, true);
        }
        playerHistory_.dropGlobalCooldown(data.spellId, now);
        playerCooldownsChanged();
    }
    // Totem failures name tool item ids; request their info so a retry of the
    // craft can show the tool's name even if it wasn't cached yet.
    if (data.result == kCastResultTotems) {
        if (data.miscArg != 0) owner_.ensureItemInfo(data.miscArg);
        if (data.miscArg2 != 0) owner_.ensureItemInfo(data.miscArg2);
    }
    std::string errMsg = castFailureMessage(owner_, data.spellId, data.result, powerType,
                                            data.miscArg, data.miscArg2);
    // A gathering cast fails in the same words as any other: the lines this
    // put in their place ("Failed.", "Already in use.", "Requires Mining
    // skill N.") are not the interface's.
    if (gatherCast && shouldDespawnGatherTarget(data.result)) {
        owner_.despawnGameObjectLocally(gatherGoGuid);
    }
    // Not at all while it repeats the last failure within three seconds
    // (cast_failure::RepeatHold): no line, no speech, no log.
    const auto holdNowMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());
    if (!failureHold_.report(data.spellId, data.result, holdNowMs)) errMsg.clear();
    if (!errMsg.empty()) owner_.addUIError(errMsg);
    // And the combat log, in the words the screen was given (0x00808200 ->
    // 0x00751ad0).
    if (!errMsg.empty() && isActiveExpansion("wotlk")) owner_.combatLogFeed().castFailed(data.spellId, errMsg);

    // On screen and nowhere else. A cast failure went to the chat log as well
    // from here and from handleCastResult both, so the fix that took it out of
    // GameHandler::raiseUiError changed nothing that was being reported.
    const bool routine = isRoutineCastRejection(data.result);

    if (!routine && !errMsg.empty()) {
        if (auto* ac = owner_.services().audioCoordinator) {
            if (auto* sfx = ac->getUiSoundManager())
                sfx->playError();
        }
    }

    // And the character's line for the failure's game error ("Not enough
    // mana", "That ability isn't ready yet"), the routine ones included -
    // the client speaks those most of all, one line at a time (0x00808200 ->
    // 0x005216f0 -> 0x00986d00).
    if (!errMsg.empty()) sayCastFailure(data.spellId, data.result);

    // Not for a cast refused beside the running one: its bar goes on.
    if (otherCast) return;
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_FAILED", spellcastArgs("player", data.spellId));
        owner_.addonEventCallbackRef()("UNIT_SPELLCAST_STOP", spellcastArgs("player", data.spellId));
    }
    if (owner_.spellCastFailedCallbackRef()) owner_.spellCastFailedCallbackRef()(data.spellId);
}

void SpellHandler::castPowerSaid(uint64_t casterUnit, uint32_t spellId, uint32_t power, bool go) {
    // 0x00806700, 0x0080e1b0: cast flag 0x800 says the caster's power of the
    // spell's type, set where UNIT_FIELD_FLAGS_2 0x100 does not hold its
    // stats (health and runes are kept apart); a mana cost gone holds mana
    // regeneration for five seconds (0x0071c390).
    Unit* unit = owner_.getUnitByGuid(casterUnit);
    loadSpellNameCache();
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (!unit || it == owner_.spellNameCacheRef().end()) return;
    const auto type = static_cast<int32_t>(it->second.powerType);
    if (type == -2 || type == 5) return;
    if ((entityField(*unit, UF::UNIT_FIELD_FLAGS_2) & 0x100u) == 0)
        owner_.setPredictedPower(*unit, type, static_cast<int32_t>(power));
    if (go && type == 0) owner_.startFiveSecondRule(*unit);
}

void SpellHandler::handleSpellStart(network::Packet& packet) {
    SpellStartData data;
    if (!owner_.getPacketParsers()->parseSpellStart(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_SPELL_START, size=", packet.getSize());
        return;
    }
    LOG_DEBUG("SMSG_SPELL_START: caster=0x", std::hex, data.casterUnit, std::dec,
              " spell=", data.spellId, " castTime=", data.castTime,
              " target=0x", std::hex, data.targetGuid, std::dec);
    if (data.hasPower) castPowerSaid(data.casterUnit, data.spellId, data.power, false);
    if (isActiveExpansion("wotlk")) owner_.combatLogFeed().castStart(data.casterUnit, data.spellId, data.castFlags);

    // Classify spell targeting for animation selection:
    //   DIRECTED - targets a specific other unit (Frostbolt, Heal)
    //   OMNI     - self-cast or no explicit target (Arcane Explosion, buffs)
    //   AREA     - ground-targeted AoE with no unit target (Blizzard, Rain of Fire)
    auto classifyCast = [](uint64_t targetGuid, uint64_t casterGuid) -> SpellCastType {
        if (targetGuid == 0)            return SpellCastType::AREA;
        if (targetGuid == casterGuid)   return SpellCastType::OMNI;
        return SpellCastType::DIRECTED;
    };
    const SpellCastType castType = classifyCast(data.targetGuid, data.casterUnit);
    const bool rangedWeaponAttack = spellclass::isRangedWeaponAutoAttack(data.spellId);
    // 0x00805330: the cast's beginning on the caster (0x007fa2e0).
    if (owner_.spellCastBeginCallbackRef()) owner_.spellCastBeginCallbackRef()(data.casterUnit, data.spellId);

    // Track cast bar for any non-player caster
    if (data.casterUnit != owner_.getPlayerGuid() && data.castTime > 0 && !rangedWeaponAttack) {
        auto& s = unitCastStates_[data.casterUnit];
        s.casting        = true;
        s.isChannel      = false;
        s.spellId        = data.spellId;
        s.timeTotal      = data.castTime / 1000.0f;
        s.timeRemaining  = s.timeTotal;
        s.interruptible  = owner_.isSpellInterruptible(data.spellId);
        s.castType       = castType;
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(data.casterUnit, true, false, castType);
        }
    }

    // Player's own cast
    if (data.casterUnit == owner_.getPlayerGuid() && data.castTime > 0 && !rangedWeaponAttack) {
        // Cancel pending GO retries
        owner_.pendingGameObjectLootRetriesRef().erase(
            std::remove_if(owner_.pendingGameObjectLootRetriesRef().begin(), owner_.pendingGameObjectLootRetriesRef().end(),
                [](const GameHandler::PendingLootRetry&) { return true; }),
            owner_.pendingGameObjectLootRetriesRef().end());

        casting_ = true;
        castIsChannel_ = false;
        currentCastSpellId_ = data.spellId;
        // Cast from an item, the caster SMSG_SPELL_START names is the item.
        castItemGuid_ = data.casterGuid != data.casterUnit ? data.casterGuid : 0;
        castTimeTotal_ = data.castTime / 1000.0f;
        castTimeRemaining_ = castTimeTotal_;
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CURRENT_SPELL_CAST_CHANGED", {});

        // Play precast sound - skip profession/tradeskill spells
        if (!owner_.isProfessionSpell(data.spellId)) {
            if (auto* ac = owner_.services().audioCoordinator) {
                if (auto* ssm = ac->getSpellSoundManager()) {
                    owner_.loadSpellNameCache();
                    auto it = owner_.spellNameCacheRef().find(data.spellId);
                    auto school = (it != owner_.spellNameCacheRef().end() && it->second.schoolMask)
                        ? schoolMaskToMagicSchool(it->second.schoolMask)
                        : audio::SpellSoundManager::MagicSchool::ARCANE;
                    ssm->playPrecast(school, audio::SpellSoundManager::SpellPower::MEDIUM);
                }
            }
        }

        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), true, false, castType);
        }

        // Hearthstone: pre-load terrain at bind point
        const bool isHearthstone = (data.spellId == ITEM_ID_HEARTHSTONE ||
                                    data.spellId == SPELL_ID_HEARTHSTONE);
        if (isHearthstone && owner_.hasHomeBindRef() && owner_.hearthstonePreloadCallbackRef()) {
            owner_.hearthstonePreloadCallbackRef()(owner_.homeBindMapIdRef(), owner_.homeBindPosRef().x, owner_.homeBindPosRef().y, owner_.homeBindPosRef().z);
        }
    }

    // Fire UNIT_SPELLCAST_START
    if (owner_.addonEventCallbackRef() && !rangedWeaponAttack) {
        std::string unitId = owner_.guidToUnitId(data.casterUnit);
        if (!unitId.empty()) {
            owner_.addonEventCallbackRef()("UNIT_SPELLCAST_START", spellcastArgs(unitId, data.spellId));
            // Whether the cast can be kicked, which the cast bar draws as a
            // shield around itself. This client already worked it out for the
            // cast it is tracking and kept it - so a boss casting something
            // uninterruptible looked exactly like one that could be stopped.
            //
            // After START, not before: the bar sets its shield up when the cast
            // begins and these only change the border, so arriving first would
            // be undone by the start it is describing.
            owner_.addonEventCallbackRef()(
                owner_.isSpellInterruptible(data.spellId)
                    ? "UNIT_SPELLCAST_INTERRUPTIBLE"
                    : "UNIT_SPELLCAST_NOT_INTERRUPTIBLE",
                {unitId});
        }
    }

    // Trigger cast visual effect (precast/cast kit M2) at the caster's position:
    // every spell's, a profession's too - its kit hangs the tool in the hand
    // (0x0073a6c0).
    if (!rangedWeaponAttack) {
        triggerCastVisual(data.spellId, data.casterUnit, data.castTime);
    }
}

void SpellHandler::noteMissileInFlight(const SpellGoData& data) {
    owner_.loadSpellNameCache();
    const auto spellIt = owner_.spellNameCacheRef().find(data.spellId);
    if (spellIt == owner_.spellNameCacheRef().end() || spellIt->second.missileSpeed == 0.0f) return;
    const float speed = spellIt->second.missileSpeed;
    auto& entities = owner_.getEntityManager();
    const auto caster = entities.getEntity(data.casterUnit);
    if (!caster) return;
    const glm::vec3 from(caster->getX(), caster->getY(), caster->getZ());
    const auto add = [&](const glm::vec3& to) {
        missilesInFlight_.add({.caster = data.casterUnit, .spellId = data.spellId, .castTime = data.castTime,
                               .arrivesAt = historyNowMs() + spell_missiles::flightMs(glm::length(to - from), speed)});
    };
    // To the destination where the targets have one, else to the first unit
    // hit, if the client knows it; otherwise none for these. The destination
    // is the wire's, in its transport's frame (0x009ab810).
    if ((data.targetFlags & 0x40u) != 0 && data.hasDestLocation) {
        add(targetPointInWorld(data.destTransportGuid, data.destX, data.destY, data.destZ));
    } else if (!data.hitTargets.empty()) {
        if (const auto target = entities.getEntity(data.hitTargets.front()))
            add(glm::vec3(target->getX(), target->getY(), target->getZ()));
    }
    // Target flag 0x80000: one more for each further place - one in a
    // transport's frame put in the world by it, and one whose transport is
    // not known left out (0x0080e1b0 -> 0x0074b4c0, 0x00804b20).
    for (const SpellGoExtraTarget& extra : data.extraTargets) {
        const glm::vec3 place(extra.x, extra.y, extra.z);
        if (extra.transportGuid == 0) {
            add(core::coords::serverToCanonical(place));
        } else if (const auto placed = pointInObjectFrame(extra.transportGuid, place)) {
            add(*placed);
        }
    }
}

void SpellHandler::handleSpellGo(network::Packet& packet) {
    SpellGoData data;
    if (!owner_.getPacketParsers()->parseSpellGo(packet, data)) return;
    noteMissileInFlight(data);
    if (isActiveExpansion("wotlk")) owner_.combatLogFeed().castGo(data);
    if ((data.targetFlags & 0x40u) && data.destLocationCounter != 0)
        rememberDestLocationCount(data.casterUnit, data.destLocationCounter);
    const bool rangedWeaponAttack = spellclass::isRangedWeaponAutoAttack(data.spellId);
    // Any cast going off, bar a pending one, lets the next failure be
    // reported (0x0080e1b0 clears 0x00d397c4).
    if ((data.castFlags & 0x1u) == 0) failureHold_.forget();
    if (data.hasPower) castPowerSaid(data.casterUnit, data.spellId, data.power, true);
    // 0x00805330 for a cast that had no start (instant), as for one that had.
    if (owner_.spellCastBeginCallbackRef()) owner_.spellCastBeginCallbackRef()(data.casterUnit, data.spellId);

    // 0x0080e1b0: a unit the player charmed (else summoned) - the pet - puts
    // the cast in the pet's history: its cooldown and category's, through
    // the modifiers, and its global cooldown; every 30 s the history is
    // pruned (0x00d3f608).
    {
        const int64_t now = historyNowMs();
        auto caster = owner_.getEntityManager().getEntity(data.casterUnit);
        if (caster && caster->isUnit() && data.casterUnit != owner_.getPlayerGuid()) {
            uint64_t by = entityGuidField(*caster, UF::UNIT_FIELD_CHARMEDBY);
            if (by == 0) {
                const uint16_t lo = fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_LO);
                const uint16_t hi = fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_HI);
                by = (lo != 0xFFFF ? caster->getField(lo) : 0u) |
                     (static_cast<uint64_t>(hi != 0xFFFF ? caster->getField(hi) : 0u) << 32);
            }
            loadSpellNameCache();
            const auto it = owner_.spellNameCacheRef().find(data.spellId);
            if (by == owner_.getPlayerGuid() && it != owner_.spellNameCacheRef().end()) {
                const auto& e = it->second;
                spell_history::Entry h;
                h.spellId = data.spellId;
                h.start = now;
                h.duration = moddedCooldown(data.spellId, static_cast<int32_t>(e.recoveryMs));
                h.category = e.category;
                h.categoryStart = now;
                h.categoryDuration = (e.attrEx6 & 0x80000000u) == 0
                                         ? moddedCooldown(data.spellId, static_cast<int32_t>(e.categoryRecoveryMs))
                                         : static_cast<int32_t>(e.categoryRecoveryMs);
                h.onHold = (e.attr & 0x2000000u) != 0;  // 0x00801220
                h.gcdCategory = e.startRecoveryCategory;
                const auto gcdMods = spellModifiers(data.spellId, 21);
                h.gcd = spell_history::globalCooldown(
                    e.startRecoveryCategory,
                    static_cast<int32_t>(spell_mods::apply(static_cast<float>(e.startRecoveryMs), gcdMods)),
                    static_cast<int32_t>(e.startRecoveryMs), e.dmgClass, e.attr,
                    castSpeedOf(*caster));
                petHistory_.add(h);
            }
        }
        // The player's own cast, not from an item: its cooldown and its
        // category's (0x007fef60), begun now, held for Attributes 0x2000000.
        if (data.casterUnit == owner_.getPlayerGuid() && data.casterGuid == data.casterUnit) {
            loadSpellNameCache();
            if (const auto it = owner_.spellNameCacheRef().find(data.spellId);
                it != owner_.spellNameCacheRef().end()) {
                spell_history::Entry h;
                h.spellId = data.spellId;
                h.start = now;
                h.duration = moddedCooldown(data.spellId, static_cast<int32_t>(it->second.recoveryMs));
                h.category = it->second.category;
                h.categoryStart = now;
                h.categoryDuration = categoryCooldown(data.spellId, false);
                h.onHold = (it->second.attr & 0x2000000u) != 0;  // 0x00801220
                playerHistory_.add(h);
                playerCooldownsChanged();
            }
        }
        // Cast from an item of the player's whose use spell it is: the item
        // template's cooldown, category and category cooldown (0x00707e90 ->
        // 0x00706c50), the spell's own where under zero (the category under
        // one), begun now, as cast from that item.
        if (data.casterUnit == owner_.getPlayerGuid() && data.casterGuid != data.casterUnit) {
            loadSpellNameCache();
            const auto item = owner_.onlineItemsRef().find(data.casterGuid);
            const auto it = owner_.spellNameCacheRef().find(data.spellId);
            const auto* info = item != owner_.onlineItemsRef().end() ? owner_.getItemInfo(item->second.entry) : nullptr;
            const ItemQueryResponseData::ItemSpell* use = nullptr;
            for (size_t k = 0; info && info->valid && k < info->spells.size() && !use; ++k)
                if (info->spells[k].spellId != 0 && info->spells[k].spellTrigger == 0) use = &info->spells[k];
            if (it != owner_.spellNameCacheRef().end() && use && use->spellId == data.spellId) {
                spell_history::Entry h;
                h.spellId = data.spellId;
                h.itemId = item->second.entry;
                h.start = now;
                h.duration = use->cooldown < 0 ? static_cast<int32_t>(it->second.recoveryMs) : use->cooldown;
                h.category = static_cast<int32_t>(use->category) < 1 ? it->second.category : use->category;
                h.categoryStart = now;
                h.categoryDuration = use->categoryCooldown < 0 ? static_cast<int32_t>(it->second.categoryRecoveryMs)
                                                               : use->categoryCooldown;
                h.onHold = (it->second.attr & 0x2000000u) != 0;
                playerHistory_.add(h);
                playerCooldownsChanged();
                if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("BAG_UPDATE_COOLDOWN", {});
            }
        }
        // A rune spell (PowerType 5) of the player's says which runes it
        // used (0x0080e1b0 -> 0x005ee240).
        if (data.hasRuneList && data.casterUnit == owner_.getPlayerGuid()) {
            if (const auto it = owner_.spellNameCacheRef().find(data.spellId);
                it != owner_.spellNameCacheRef().end() && it->second.powerType == 5)
                owner_.runeListChanged(data.runesBefore, data.runesAfter, data.runeProgress);
        }
        if (now - historyPruneAt_ >= 0) {
            historyPruneAt_ = now + 30000;
            playerHistory_.prune(now);
            petHistory_.prune(now);
        }
    }

    if (data.casterUnit == owner_.getPlayerGuid()) {
        owner_.loadSpellNameCache();
        spellclass::RestChannelKind restKind = spellclass::RestChannelKind::NONE;
        auto spellNameIt = owner_.spellNameCacheRef().find(data.spellId);
        if (spellNameIt != owner_.spellNameCacheRef().end()) {
            restKind = spellclass::classifyRestChannel(spellNameIt->second.name);
            if (spellclass::hasInebriateEffect(spellNameIt->second.effectIds, 3)) {
                restKind = spellclass::RestChannelKind::ALCOHOL;
            }
        }
        const bool restorationLoop = restKind == spellclass::RestChannelKind::FOOD ||
                                     restKind == spellclass::RestChannelKind::DRINK;
        // The real client sits itself when consuming food/water. Servers apply
        // the regen aura with AURA_INTERRUPT_FLAG_NOT_SEATED and remove it
        // immediately unless the client sends CMSG_STANDSTATECHANGE(SIT) -
        // without this the whole eat/drink presentation never starts.
        if (restorationLoop) {
            owner_.setStandState(1); // UNIT_STAND_STATE_SIT
        }
        // Play cast-complete sound
        if (!owner_.isProfessionSpell(data.spellId) && !rangedWeaponAttack)
            playSpellCastSound(data.spellId);

        // Ranged auto-attack spells (Auto Shot, Shoot, Throw) complete as timed
        // casts and are NOT classified as instant melee abilities, so trigger the
        // ranged shot animation explicitly here.
        uint32_t sid = data.spellId;
        if (spellclass::isRangedWeaponAutoAttack(sid)) {
            if (owner_.meleeSwingCallbackRef()) owner_.meleeSwingCallbackRef()(sid);
            owner_.suppressNextMeleeSwingAnim();
            uint64_t projectileTarget = data.targetGuid;
            if (projectileTarget == 0 && !data.hitTargets.empty())
                projectileTarget = data.hitTargets.front();
            if (projectileTarget == 0 && !data.missTargets.empty())
                projectileTarget = data.missTargets.front().targetGuid;
            launchRangedWeaponProjectile(sid, projectileTarget);
        }

        // Instant melee abilities → trigger attack animation. Range decides this, not
        // school: physical abilities cast at range (Steady Shot, Taunt) would otherwise
        // play a melee swing.
        bool isMeleeAbility = false;
        if (!owner_.isProfessionSpell(sid) && !spellclass::isRangedWeaponAutoAttack(sid)) {
            if (spellclass::isMeleeRange(getSpellMaxRange(sid))) {
                isMeleeAbility = (currentCastSpellId_ != sid);
            }
        }
        // The ability's swing sounds by its animation's own events and the
        // hit the server reports for it (rendering/melee_sound.hpp).
        if (isMeleeAbility) {
            if (owner_.meleeSwingCallbackRef()) owner_.meleeSwingCallbackRef()(sid);
        }

        const bool wasInTimedCast = casting_ && (data.spellId == currentCastSpellId_);

        // Instant spell cast animation - if this wasn't a timed cast and isn't a
        // melee ability, play a brief spell cast animation (one-shot)
        if (!wasInTimedCast && !isMeleeAbility && !rangedWeaponAttack && !restorationLoop &&
            !owner_.isProfessionSpell(data.spellId)) {
            // Classify instant spell from SPELL_GO packet target info
            SpellCastType goType = SpellCastType::OMNI;
            if (data.targetGuid != 0 && data.targetGuid != data.casterUnit)
                goType = SpellCastType::DIRECTED;
            else if (data.targetGuid == 0 && data.hitCount > 1)
                goType = SpellCastType::AREA;
            if (owner_.spellCastAnimCallbackRef()) {
                // Instant item spells have no SMSG_SPELL_START, so publish the
                // SPELL_GO id just long enough for the animation callback to
                // distinguish potions from generic instant magic.
                currentCastSpellId_ = data.spellId;
                owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), true, false, goType);
            }
        }

        LOG_DEBUG("[GO-DIAG] SPELL_GO: spellId=", data.spellId,
                    " casting=", casting_, " currentCast=", currentCastSpellId_,
                    " wasInTimedCast=", wasInTimedCast,
                    " lastGoGuid=0x", std::hex, owner_.lastInteractedGoGuidRef(),
                    " pendingGoGuid=0x", owner_.pendingGameObjectInteractGuidRef(), std::dec);

        casting_ = false;
        castIsChannel_ = false;
        currentCastSpellId_ = 0;
        castTimeRemaining_ = 0.0f;
        castTimeTotal_ = 0.0f;

        // Gather node looting: re-send CMSG_LOOT now that the cast completed.
        if (wasInTimedCast && owner_.lastInteractedGoGuidRef() != 0) {
            LOG_DEBUG("[GO-DIAG] Sending CMSG_LOOT for GO 0x", std::hex,
                        owner_.lastInteractedGoGuidRef(), std::dec);
            owner_.lootTarget(owner_.lastInteractedGoGuidRef());
        }
        // Clear the GO interaction guard so future cancelCast() calls work
        // normally. Without this, pendingGameObjectInteractGuid_ stays stale
        // and suppresses CMSG_CANCEL_CAST for ALL subsequent spell casts.
        owner_.pendingGameObjectInteractGuidRef() = 0;

        if (owner_.spellCastAnimCallbackRef() && !rangedWeaponAttack && !restorationLoop) {
            owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), false, false, SpellCastType::OMNI);
        }

        if (owner_.addonEventCallbackRef() && !rangedWeaponAttack)
            owner_.addonEventCallbackRef()("UNIT_SPELLCAST_STOP", spellcastArgs("player", data.spellId));

        // Craft queue: re-cast if more crafts remaining
        if (craftQueueRemaining_ > 0 && craftQueueSpellId_ == data.spellId) {
            --craftQueueRemaining_;
            if (craftQueueRemaining_ > 0) {
                LOG_INFO("Craft queue: re-casting spell=", craftQueueSpellId_,
                         " remaining=", craftQueueRemaining_);
                castSpell(craftQueueSpellId_, 0);
            } else {
                craftQueueSpellId_ = 0;
            }
            // The count changed, and the trade skill frame only reads it when
            // told to: UPDATE_TRADESKILL_RECAST is what makes its quantity box
            // count down instead of sitting at the number first asked for.
            if (owner_.addonEventCallbackRef())
                owner_.addonEventCallbackRef()("UPDATE_TRADESKILL_RECAST", {});
        }
    } else {
        // For non-player casters: if no tracked cast state exists, this was an
        // instant cast - play a brief one-shot spell animation before stopping
        auto castIt = unitCastStates_.find(data.casterUnit);
        bool wasTrackedCast = (castIt != unitCastStates_.end());
        // Classify NPC instant spell from SPELL_GO target info
        SpellCastType npcGoType = SpellCastType::OMNI;
        if (data.targetGuid != 0 && data.targetGuid != data.casterUnit)
            npcGoType = SpellCastType::DIRECTED;
        else if (data.targetGuid == 0 && data.hitCount > 1)
            npcGoType = SpellCastType::AREA;
        if (!wasTrackedCast && !rangedWeaponAttack && owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(data.casterUnit, true, false, npcGoType);
        }
        if (!rangedWeaponAttack && owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(data.casterUnit, false, false, SpellCastType::OMNI);
        }
        bool targetsPlayer = false;
        for (const auto& tgt : data.hitTargets) {
            if (tgt == owner_.getPlayerGuid()) { targetsPlayer = true; break; }
        }
        if (targetsPlayer && !rangedWeaponAttack)
            playSpellCastSound(data.spellId);
    }

    // Clear unit cast bar
    unitCastStates_.erase(data.casterUnit);

    // Miss combat text
    if (!data.missTargets.empty()) {
        const uint64_t spellCasterGuid = data.casterUnit != 0 ? data.casterUnit : data.casterGuid;
        const bool playerIsCaster = (spellCasterGuid == owner_.getPlayerGuid());

        for (const auto& m : data.missTargets) {
            if (!playerIsCaster && m.targetGuid != owner_.getPlayerGuid()) {
                continue;
            }
            CombatTextEntry::Type ct = combatTextTypeFromSpellMissInfo(m.missType);
            owner_.addCombatText(ct, 0, data.spellId, playerIsCaster, 0, spellCasterGuid, m.targetGuid);
        }
    }

    // Fire UNIT_SPELLCAST_SUCCEEDED
    if (owner_.addonEventCallbackRef()) {
        std::string unitId = owner_.guidToUnitId(data.casterUnit);
        if (!unitId.empty())
            owner_.addonEventCallbackRef()("UNIT_SPELLCAST_SUCCEEDED", spellcastArgs(unitId, data.spellId));
    }

    // No impact sound of its own here: the impact kit plays its SoundID
    // where and when it plays - on a missile's arrival for one that flies
    // (SpellVisualSystem::playSpellVisual, 0x00745230). A sound by the
    // spell's school was played here at SMSG_SPELL_GO instead.

    // Trigger spell visual effects: cast kit at caster + impact kit at each
    // hit target, for every spell but a ranged weapon's shot.
    if (!rangedWeaponAttack) {
        uint32_t visualId = resolveSpellVisualId(data.spellId);
        if (visualId != 0) {
            // Cast-complete visual at caster (for instant spells that skip SPELL_START)
            glm::vec3 casterPos;
            const bool haveCasterPos = resolveUnitPosition(data.casterUnit, casterPos);
            if (haveCasterPos) {
                if (auto* renderer = owner_.services().renderer) {
                    if (auto* svs = renderer->getSpellVisualSystem()) {
                        // Where its kits' chains go (0x00724f50, 0x00715400).
                        std::optional<glm::vec3> destination;
                        if ((data.targetFlags & 0x40u) && data.hasDestLocation)
                            destination = core::coords::canonicalToRender(
                                targetPointInWorld(data.destTransportGuid, data.destX, data.destY, data.destZ));
                        svs->setUnitCastTargets(data.casterUnit, data.spellId, data.hitTargets, destination);
                        svs->playSpellVisual(visualId, casterPos, /*useImpactKit=*/false,
                                             owner_.resolveUnitRenderInstance(data.casterUnit), false,
                                             data.spellId);
                    }
                }
            }
            // Impact visual at each hit target - when a missile carries it,
            // on the missile's arrival instead.
            bool flewToPlace = false;
            const std::vector<uint64_t> carried = launchSpellMissiles(data, visualId, &flewToPlace);
            // A cast at a place (target flag 0x40) plays its area kits there;
            // the ImpactAreaKit as its missile lands where one flies
            // (0x0080e1b0, 0x00700e20).
            if ((data.targetFlags & 0x40u) && data.hasDestLocation) {
                if (auto* renderer = owner_.services().renderer) {
                    if (auto* svs = renderer->getSpellVisualSystem()) {
                        // The spell's speed times the ImpactAreaKit where no
                        // missile carries it (0x0080e1b0, 0x00728050 mode 1).
                        const auto spellIt = owner_.spellNameCacheRef().find(data.spellId);
                        const float speed =
                            spellIt != owner_.spellNameCacheRef().end() ? spellIt->second.missileSpeed : 0.0f;
                        svs->playSpellAreaKits(
                            visualId,
                            core::coords::canonicalToRender(
                                targetPointInWorld(data.destTransportGuid, data.destX, data.destY, data.destZ)),
                            flewToPlace, data.spellId, data.casterUnit, speed,
                            haveCasterPos ? std::optional<glm::vec3>(casterPos) : std::nullopt);
                    }
                }
            }
            for (const auto& tgt : data.hitTargets) {
                if (tgt != 0 && std::find(carried.begin(), carried.end(), tgt) == carried.end()) {
                    triggerImpactVisual(data.spellId, tgt, data.casterUnit);
                }
            }
        }
    }
}

void SpellHandler::handleSpellCooldown(network::Packet& packet) {
    const bool isClassicFormat = isClassicLikeExpansion();

    if (!packet.hasRemaining(8)) return;
    const uint64_t cooldownGuid = packet.readUInt64();

    uint8_t cooldownFlags = 0;
    if (!isClassicFormat) {
        if (!packet.hasRemaining(1)) return;
        cooldownFlags = packet.readUInt8();
    }
    // 0x00806dd0: the player's or the pet's go into its history - begun
    // now, the time sent or else the spell's own through the modifiers (the
    // category's through 0x007fef60), with the global cooldown for flag 1
    // unless held (0x00801220; flag 2 never holds).
    const bool forPlayer = cooldownGuid == owner_.getPlayerGuid();
    if (cooldownGuid != 0 && (forPlayer || cooldownGuid == owner_.getPetGuid()) && !isClassicFormat) {
        loadSpellNameCache();
        const int64_t now = historyNowMs();
        auto& history = forPlayer ? playerHistory_ : petHistory_;
        while (packet.getRemainingSize() >= 8) {
            const uint32_t spellId = packet.readUInt32();
            const uint32_t cooldownMs = packet.readUInt32();
            const auto it = owner_.spellNameCacheRef().find(spellId);
            if (it == owner_.spellNameCacheRef().end()) continue;
            const auto& e = it->second;
            spell_history::Entry h;
            h.spellId = spellId;
            h.start = now;
            h.duration = cooldownMs != 0 ? static_cast<int32_t>(cooldownMs)
                                         : moddedCooldown(spellId, static_cast<int32_t>(e.recoveryMs));
            h.category = e.category;
            h.categoryStart = now;
            h.categoryDuration = cooldownMs != 0 ? 0 : categoryCooldown(spellId, !forPlayer);
            h.onHold = (cooldownFlags & 0x2) == 0 && (e.attr & 0x2000000u) != 0;
            // StartRecoveryTime through its modifiers (op 21), not hastened.
            if ((cooldownFlags & 0x1) != 0 && !h.onHold) {
                h.gcdCategory = e.startRecoveryCategory;
                h.gcd = static_cast<int32_t>(
                    spell_mods::apply(static_cast<float>(e.startRecoveryMs), spellModifiers(spellId, 21)));
            }
            history.add(h);
        }
    }
    packet.skipAll();

    LOG_DEBUG("handleSpellCooldown: parsed for ",
              isClassicFormat ? "Classic" : "TBC/WotLK", " format");
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("SPELL_UPDATE_COOLDOWN", {});
        owner_.addonEventCallbackRef()("BAG_UPDATE_COOLDOWN", {});
        owner_.addonEventCallbackRef()("ACTIONBAR_UPDATE_COOLDOWN", {});
    }
}

void SpellHandler::handleCooldownEvent(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t spellId = packet.readUInt32();
    uint64_t eventGuid = 0;
    if (packet.hasRemaining(8))
        eventGuid = packet.readUInt64();
    // 0x00804010: the held cooldown begins now (0x00802970) - the pet's,
    // else the player's.
    (eventGuid != 0 && eventGuid == owner_.getPetGuid() ? petHistory_ : playerHistory_)
        .event(spellId, historyNowMs(), false);
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("SPELL_UPDATE_COOLDOWN", {});
        owner_.addonEventCallbackRef()("BAG_UPDATE_COOLDOWN", {});
        owner_.addonEventCallbackRef()("ACTIONBAR_UPDATE_COOLDOWN", {});
    }
}

/// Keep the by-guid copy of a unit's auras in step with the target view.
///
/// Every unit but the player is kept by guid as well, because that is the only
/// list a party or raid frame can reach: UnitBuff("party1", i) resolves a guid
/// and asks for it. Updates for a unit that happened to be the current target
/// went to the target's own list *instead* of that one, and the target's list
/// is emptied the moment the target changes.
///
/// So a healer watching a party member lost their buffs from the party frame as
/// soon as they clicked on them, and a buff cast on them while they were
/// targeted never appeared there at all - which reads exactly like the buff
/// being cancelled, or never landing.
void SpellHandler::mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras) {
    if (guid == 0 || guid == owner_.getPlayerGuid()) return;
    auto& cached = unitAurasCache_[guid];
    if (&cached != &auras) cached = auras;
}

void SpellHandler::replayAuraStateKits() {
    // A teleport resets the spell visuals, the aura kits on every unit
    // among them, while the units the server keeps in view keep their
    // auras and are not sent them again; and the auras sent while the world
    // loads come before the visual system can resolve a spell's visual. Either
    // way an imp's Phase Shift lost its fade until the imp cast it again.
    auto* renderer = owner_.services().renderer;
    auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr;
    if (!svs) return;
    const auto replay = [svs](uint64_t guid, const std::vector<AuraSlot>& auras) {
        std::vector<rendering::SpellVisualSystem::AuraSlotSpell> slots;
        for (size_t slot = 0; slot < auras.size(); ++slot) {
            if (auras[slot].spellId == 0) continue;
            slots.push_back({.slot = static_cast<uint32_t>(slot), .spellId = auras[slot].spellId,
                             .casterGuid = auras[slot].casterGuid});
        }
        if (!slots.empty()) svs->replayUnitAuraSlots(guid, slots);
    };
    if (owner_.getPlayerGuid() != 0) replay(owner_.getPlayerGuid(), playerAuras_);
    for (const auto& [guid, auras] : unitAurasCache_) {
        if (owner_.getEntityManager().getEntity(guid)) replay(guid, auras);
    }
}

void SpellHandler::logAuraChanges(uint64_t unit, const std::vector<AuraSlot>& before,
                                  const std::vector<std::pair<uint8_t, AuraSlot>>& updates, uint64_t nowMs) {
    auto& feed = owner_.combatLogFeed();
    const auto active = [](const AuraSlot& a) { return a.spellId != 0 && (a.flags & 7u) != 0; };
    const auto debuff = [](const AuraSlot& a) { return (a.flags & 0x80u) != 0; };
    // When a slot's aura runs out: its time as sent, or none.
    const auto expiry = [](const AuraSlot& a, uint64_t sentAt) -> uint64_t {
        return a.durationMs >= 0 ? sentAt + static_cast<uint64_t>(a.durationMs) : 0;
    };
    // What went: an aura that is not there now as it was.
    for (const auto& [slot, now] : updates) {
        if (slot >= before.size()) continue;
        const AuraSlot& was = before[slot];
        const bool kept = was.spellId == now.spellId && (now.flags & 7u) != 0;
        if ((active(was) && !kept) || now.spellId == 0)
            feed.aura(unit, was.casterGuid, was.spellId, debuff(was), false);
    }
    // What came, or changed: a new aura, its stack, or its time.
    for (const auto& [slot, now] : updates) {
        if (!active(now)) continue;
        const bool known = slot < before.size();
        const bool same = known && active(before[slot]) && before[slot].spellId == now.spellId;
        if (!same) {
            feed.aura(unit, now.casterGuid, now.spellId, debuff(now), true);
        } else if (before[slot].charges != now.charges) {
            feed.auraDose(unit, now.casterGuid, now.spellId, before[slot].charges, now.charges, debuff(now));
        } else if (expiry(before[slot], before[slot].receivedAtMs) != expiry(now, nowMs)) {
            feed.auraRefresh(unit, now.casterGuid, now.spellId, debuff(now));
        }
    }
}

void SpellHandler::handleAuraUpdate(network::Packet& packet, bool isAll) {
    AuraUpdateData data;
    if (!owner_.getPacketParsers()->parseAuraUpdate(packet, data, isAll)) return;

    std::vector<AuraSlot>* auraList = nullptr;
    if (data.guid == owner_.getPlayerGuid()) {
        auraList = &playerAuras_;
    } else if (data.guid == owner_.getTargetGuid()) {
        auraList = &targetAuras_;
    }
    if (data.guid != 0 && data.guid != owner_.getPlayerGuid() && data.guid != owner_.getTargetGuid()) {
        auraList = &unitAurasCache_[data.guid];
    }

    if (auraList) {
        if (isAll) {
            auraList->clear();
        }
        uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        // The combat log, from the slots as they were (0x0072f5d0). A whole
        // list logs nothing.
        if (!isAll && isActiveExpansion("wotlk")) logAuraChanges(data.guid, *auraList, data.updates, nowMs);
        // Grow once to fit the highest slot, instead of push_back-in-a-loop per update.
        if (!data.updates.empty()) {
            size_t maxSlot = 0;
            for (const auto& [slot, aura] : data.updates) {
                if (slot > maxSlot) maxSlot = slot;
            }
            if (auraList->size() <= maxSlot) {
                auraList->resize(maxSlot + 1);
            }
        }
        // The spells whose aura came or went on the player, for the stance bar.
        std::vector<uint32_t> playerAuraChanges;
        if (data.guid == owner_.getPlayerGuid()) {
            if (isAll) {
                for (const auto& [slot, aura] : data.updates) playerAuraChanges.push_back(aura.spellId);
            } else {
                for (const auto& [slot, aura] : data.updates) {
                    if ((*auraList)[slot].spellId != aura.spellId) {
                        playerAuraChanges.push_back((*auraList)[slot].spellId);
                        playerAuraChanges.push_back(aura.spellId);
                    }
                }
            }
        }
        for (auto [slot, aura] : data.updates) {
            if (aura.durationMs >= 0) {
                aura.receivedAtMs = nowMs;
            }
            (*auraList)[slot] = aura;
        }
        mirrorAurasByGuid(data.guid, *auraList);

        // 0x00722090 / 0x00727760: an aura of a stance bar spell that is no
        // shapeshift (aura 36) - Stealth, a presence - came or went. A real
        // form says so through its form field instead (0x006da130).
        if (!playerAuraChanges.empty() && owner_.addonEventCallbackRef()) {
            const auto forms = knownShapeshiftForms(owner_.getPlayerClass(), owner_.getKnownSpells());
            const bool barSpellChanged = std::any_of(forms.begin(), forms.end(), [&](const ShapeshiftForm& f) {
                return std::find(playerAuraChanges.begin(), playerAuraChanges.end(), f.spellId) !=
                           playerAuraChanges.end() &&
                       !owner_.spellAppliesAura(f.spellId, 36);
            });
            if (barSpellChanged) owner_.addonEventCallbackRef()("UPDATE_SHAPESHIFT_FORM", {});
        }

        // The auras' state kits on the unit, as each slot changes
        // (0x00724820, 0x0071e930).
        if (auto* renderer = owner_.services().renderer) {
            if (auto* svs = renderer->getSpellVisualSystem()) {
                if (isAll) {
                    std::vector<rendering::SpellVisualSystem::AuraSlotSpell> slots;
                    for (const auto& [slot, aura] : data.updates)
                        slots.push_back({.slot = slot, .spellId = aura.spellId, .casterGuid = aura.casterGuid});
                    svs->setUnitAuraSlots(data.guid, slots);
                } else {
                    for (const auto& [slot, aura] : data.updates)
                        svs->setUnitAuraSlot(data.guid, slot, aura.spellId, aura.casterGuid);
                }
            }
        }

        if (owner_.addonEventCallbackRef()) {
            std::string unitId;
            if (data.guid == owner_.getPlayerGuid()) unitId = "player";
            else if (data.guid == owner_.getTargetGuid()) unitId = "target";
            else if (data.guid == owner_.focusGuidRef()) unitId = "focus";
            else if (data.guid == owner_.petGuidRef()) unitId = "pet";
            if (!unitId.empty()) {
                owner_.addonEventCallbackRef()("UNIT_AURA", {unitId});
                // A 1.12 interface listens for this rather than for
                // UNIT_AURA, and it carries no unit: buffframe.lua
                // re-reads the player's whole list when it arrives.
                if (unitId == "player") {
                    owner_.addonEventCallbackRef()("PLAYER_AURAS_CHANGED", {});
                }
            }
            // Outside that, and indented to say so. It reads the same name and
            // tests it again, so the behaviour is unchanged - but it was
            // written as though the check above guarded it, which is the shape
            // that hid a call to a null callback elsewhere in this file.
            if (unitId == "player") owner_.announceCompanionChange();
        }

        // Mount aura detection
        if (data.guid == owner_.getPlayerGuid() && owner_.currentMountDisplayIdRef() != 0 && owner_.mountAuraSpellIdRef() == 0) {
            for (const auto& [slot, aura] : data.updates) {
                if (!aura.isEmpty() && aura.maxDurationMs < 0 && aura.casterGuid == owner_.getPlayerGuid()) {
                    owner_.mountAuraSpellIdRef() = aura.spellId;
                    LOG_INFO("Mount aura detected from aura update: spellId=", aura.spellId);
                }
            }
        }

        if (data.guid == owner_.getPlayerGuid()) {
            refreshRestorationFromPlayerAuras();
        }
    }
}

namespace {
// Whether a spell is one of the stance bar's (0x0053bd40), as the class has them.
bool isStanceBarSpell(uint8_t classId, uint32_t spellId) {
    for (const ShapeshiftForm& form : allShapeshiftForms(classId)) {
        if (form.spellId == spellId || form.supersededBy == spellId) return true;
    }
    return false;
}
}  // namespace

void SpellHandler::handleLearnedSpell(network::Packet& packet) {
    const bool classicSpellId = isClassicLikeExpansion();
    const size_t minSz = classicSpellId ? 2u : 4u;
    if (packet.getRemainingSize() < minSz) return;
    uint32_t spellId = classicSpellId ? packet.readUInt16() : packet.readUInt32();

    const bool alreadyKnown = knownSpells_.count(spellId) > 0;
    knownSpells_.insert(spellId);
    LOG_INFO("Learned spell: ", spellId, alreadyKnown ? " (already known)" : "");
    if (isPreWotlk()) {
        loadTalentDbc();
        syncPreWotlkTalentsFromKnownSpells();
    }

    // Check if this spell corresponds to a talent rank
    bool isTalentSpell = false;
    for (const auto& [talentId, talent] : talentCache_) {
        for (int rank = 0; rank < 5; ++rank) {
            if (talent.rankSpells[rank] == spellId) {
                uint8_t newRank = rank + 1;
                learnedTalents_[activeTalentSpec_][talentId] = newRank;
                LOG_INFO("Talent learned: id=", talentId, " rank=", (int)newRank,
                         " (spell ", spellId, ") in spec ", (int)activeTalentSpec_);
                isTalentSpell = true;
                if (owner_.addonEventCallbackRef()) {
                    owner_.addonEventCallbackRef()("CHARACTER_POINTS_CHANGED", {"0", "0"});
                    owner_.addonEventCallbackRef()("PLAYER_TALENT_UPDATE", {});
                }
                break;
            }
        }
        if (isTalentSpell) break;
    }

    if (!alreadyKnown && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LEARNED_SPELL_IN_TAB", {std::to_string(spellId)});
        owner_.addonEventCallbackRef()("SPELLS_CHANGED", {});
        // The stance bar's list took it (0x00542563 -> event 0x178).
        if (isStanceBarSpell(owner_.getPlayerClass(), spellId)) {
            owner_.addonEventCallbackRef()("UPDATE_SHAPESHIFT_FORMS", {});
        }
        // The mounts and critters list is built from the spellbook, so a new
        // spell can add to it. petpaperdollframe refreshes on this and on
        // nothing else, which is why the tab stayed as it was first drawn.
        owner_.addonEventCallbackRef()("COMPANION_LEARNED", {});
        // A crafting window's recipes are the spells that just changed, and
        // it refreshes on its own event rather than on that one.
        if (craftingWindowOpen_) owner_.addonEventCallbackRef()("TRADE_SKILL_UPDATE", {});
    }

    // The learn line in chat for every SMSG_LEARNED_SPELL, as 0x006e7d60 ->
    // 0x006e7b00 -> 0x00542030 prints it - a talent's spell and a new rank of
    // a known one included: the server sends SMSG_SUPERCEDED_SPELL first, and
    // that already put the new rank in the book. This was English, and left
    // talents and new ranks out.
    (void)isTalentSpell;
    const std::string learned = learnedSpellText(owner_, spellId);
    if (!learned.empty()) owner_.addSystemChatMessage(learned);
}

void SpellHandler::handleRemovedSpell(network::Packet& packet) {
    const bool classicSpellId = isClassicLikeExpansion();
    const size_t minSz = classicSpellId ? 2u : 4u;
    if (packet.getRemainingSize() < minSz) return;
    uint32_t spellId = classicSpellId ? packet.readUInt16() : packet.readUInt32();
    knownSpells_.erase(spellId);
    syncPreWotlkTalentsFromKnownSpells();
    LOG_INFO("Removed spell: ", spellId);
    // Braced. The two lines below were indented as though they sat inside the
    // check above and did not - the first if has no body braces - so the
    // crafting refresh called the callback without testing it, and a null one
    // throws rather than doing nothing.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("SPELLS_CHANGED", {});
        // Off the stance bar's list (0x0053fda8 -> event 0x178).
        if (isStanceBarSpell(owner_.getPlayerClass(), spellId)) {
            owner_.addonEventCallbackRef()("UPDATE_SHAPESHIFT_FORMS", {});
        }
        // The mirror of COMPANION_LEARNED on the other side of this file. The
        // mounts and critters list is built from the spellbook, so losing a
        // spell can take one off it, and the pet tab refreshes on this and on
        // nothing else.
        owner_.addonEventCallbackRef()("COMPANION_UNLEARNED", {});
        // A crafting window's recipes are the spells that just changed, and
        // it refreshes on its own event rather than on that one.
        if (craftingWindowOpen_) owner_.addonEventCallbackRef()("TRADE_SKILL_UPDATE", {});
    }

    // Learning a new talent rank legitimately removes/replaces its internal
    // talent spell. That is rank bookkeeping, not the player unlearning the
    // talent, and presenting it as "You have unlearned" is backwards.
    loadTalentDbc();
    bool isTalentRankSpell = false;
    for (const auto& [talentId, talent] : talentCache_) {
        (void)talentId;
        for (uint32_t rankSpell : talent.rankSpells) {
            if (rankSpell == spellId) {
                isTalentRankSpell = true;
                break;
            }
        }
        if (isTalentRankSpell) break;
    }

    // ERR_SPELL_UNLEARNED_S with "Name (Rank)" in chat, as 0x006e7840 ->
    // 0x006e71d0 prints it for any spell that is neither a trade spell
    // (Attributes 0x20) nor hidden (0x80) - a talent's old rank included.
    // This was English, and printed for an unknown spell as well.
    (void)isTalentRankSpell;
    if (const auto attrs = owner_.getSpellAttributes(spellId);
        attrs && (*attrs & 0xA0u) == 0 && !owner_.getSpellName(spellId).empty()) {
        owner_.addSystemChatMessage(interfaceFormat("ERR_SPELL_UNLEARNED_S", "You have unlearned %s.",
                                                    {spellNameWithRank(owner_, spellId)}));
    }

    bool barChanged = false;
    for (auto& slot : owner_.actionBarRef()) {
        if (slot.type == ActionBarSlot::SPELL && slot.id == spellId) {
            slot = ActionBarSlot{};
            barChanged = true;
        }
    }
    if (barChanged) owner_.saveCharacterConfig();
}

void SpellHandler::handleSupercededSpell(network::Packet& packet) {
    const bool classicSpellId = isClassicLikeExpansion();
    const size_t minSz = classicSpellId ? 4u : 8u;
    if (packet.getRemainingSize() < minSz) return;
    uint32_t oldSpellId = classicSpellId ? packet.readUInt16() : packet.readUInt32();
    uint32_t newSpellId = classicSpellId ? packet.readUInt16() : packet.readUInt32();

    knownSpells_.erase(oldSpellId);

    const bool newSpellAlreadyAnnounced = knownSpells_.count(newSpellId) > 0;

    knownSpells_.insert(newSpellId);
    syncPreWotlkTalentsFromKnownSpells();

    LOG_INFO("Spell superceded: ", oldSpellId, " -> ", newSpellId);

    bool barChanged = false;
    for (auto& slot : owner_.actionBarRef()) {
        if (slot.type == ActionBarSlot::SPELL && slot.id == oldSpellId) {
            slot.id = newSpellId;
            barChanged = true;
            LOG_DEBUG("Action bar slot upgraded: spell ", oldSpellId, " -> ", newSpellId);
        }
    }
    if (barChanged) {
        owner_.saveCharacterConfig();
        // Zero, not nothing. The slot is the argument, and zero is how the
        // event says "all of them" - ActionButton_OnEvent reads
        // `arg1 == 0 or arg1 == tonumber(self.action)`, so an absent one
        // matches neither and not a single button redraws. Which slots a
        // rank upgrade touched is not tracked here, and every slot is the
        // honest answer as well as the working one.
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("ACTIONBAR_SLOT_CHANGED", {"0"});
    }

    // No line: 0x006e7e00 only swaps the rank in the book. The new rank is
    // announced by the SMSG_LEARNED_SPELL that follows. This printed
    // "Upgraded to X" in English.
    (void)newSpellAlreadyAnnounced;
}

void SpellHandler::handleUnlearnSpells(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t spellCount = packet.readUInt32();
    LOG_INFO("Unlearning ", spellCount, " spells");

    bool barChanged = false;
    for (uint32_t i = 0; i < spellCount && packet.getRemainingSize() >= 4; ++i) {
        uint32_t spellId = packet.readUInt32();
        knownSpells_.erase(spellId);
        LOG_INFO("  Unlearned spell: ", spellId);
        for (auto& slot : owner_.actionBarRef()) {
            if (slot.type == ActionBarSlot::SPELL && slot.id == spellId) {
                slot = ActionBarSlot{};
                barChanged = true;
            }
        }
    }
    if (barChanged) owner_.saveCharacterConfig();
    // No line: 0x006e2240 only keeps the list. This printed "Unlearned N
    // spells" in English.
}

void SpellHandler::handleTalentsInfo(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t talentType = packet.readUInt8();
    if (talentType != 0) {
        return;
    }
    if (!packet.hasRemaining(6)) {
        LOG_WARNING("handleTalentsInfo: packet too short for header");
        return;
    }

    uint32_t unspentTalents    = packet.readUInt32();
    uint8_t  talentGroupCount  = packet.readUInt8();
    uint8_t  activeTalentGroup = packet.readUInt8();
    if (activeTalentGroup > 1) activeTalentGroup = 0;

    loadTalentDbc();

    activeTalentSpec_ = activeTalentGroup;

    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        if (!packet.hasRemaining(1)) break;
        uint8_t talentCount = packet.readUInt8();
        learnedTalents_[g].clear();
        for (uint8_t t = 0; t < talentCount; ++t) {
            if (!packet.hasRemaining(5)) break;
            uint32_t talentId = packet.readUInt32();
            uint8_t  rank     = packet.readUInt8();
            learnedTalents_[g][talentId] = rank + 1u;
        }
        learnedGlyphs_[g].fill(0);
        if (!packet.hasRemaining(1)) break;
        uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount; ++gl) {
            if (!packet.hasRemaining(2)) break;
            uint16_t glyphId = packet.readUInt16();
            if (gl < MAX_GLYPH_SLOTS) learnedGlyphs_[g][gl] = glyphId;
        }
    }

    unspentTalentPoints_[activeTalentGroup] =
        static_cast<uint8_t>(unspentTalents > 255 ? 255 : unspentTalents);

    LOG_INFO("handleTalentsInfo: unspent=", unspentTalents,
             " groups=", (int)talentGroupCount, " active=", (int)activeTalentGroup,
             " learned=", learnedTalents_[activeTalentGroup].size());

    if (owner_.addonEventCallbackRef()) {
        // Two arguments, both zero: the interface reads the second as a count
        // of skill points just gained and compares it against zero, which is an
        // error against nothing. These fire on a full refresh rather than an
        // increment, so nothing was gained and nothing is announced.
        owner_.addonEventCallbackRef()("CHARACTER_POINTS_CHANGED", {"0", "0"});
        owner_.addonEventCallbackRef()("ACTIVE_TALENT_GROUP_CHANGED", {});
        owner_.addonEventCallbackRef()("PLAYER_TALENT_UPDATE", {});
    }

    // No line about unspent points: 0x006cd770 -> 0x005c9e50 only updates the
    // talent frame. This printed one in English at login, and the flag that
    // kept it to the first refresh went with it.
}

void SpellHandler::handleAchievementEarned(network::Packet& packet) {
    // WotLK SMSG_ACHIEVEMENT_EARNED: packGUID player + uint32 achievementId + packedTime.
    // The player GUID is a PACKED guid, not a full uint64 - reading it as uint64 swallowed
    // 4 bytes of the achievement id (showing the raw guid + a garbage id), so decode the
    // packed guid and the fixed fields follow at the correct offset.
    if (!packet.hasFullPackedGuid()) return;
    uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;  // achievementId(4) + packedTime(4)
    uint32_t achievementId = packet.readUInt32();
    uint32_t earnDate      = packet.readUInt32();

    owner_.loadAchievementNameCache();
    auto nameIt = owner_.achievementNameCacheRef().find(achievementId);
    const std::string& achName = (nameIt != owner_.achievementNameCacheRef().end())
        ? nameIt->second : std::string();

    bool isSelf = (guid == owner_.getPlayerGuid());
    if (isSelf) {
        // No chat line of its own: 0x005b3020 raises the badge and nothing
        // else, and the player's own CHAT_MSG_ACHIEVEMENT reaches them in the
        // same say-range broadcast as everyone else's. This printed
        // "Achievement earned: X" in English on top.
        owner_.earnedAchievementsRef().insert(achievementId);
        owner_.achievementDatesRef()[achievementId] = earnDate;
        if (auto* ac = owner_.services().audioCoordinator) {
            if (auto* sfx = ac->getUiSoundManager())
                sfx->playAchievementAlert();
        }
        if (owner_.achievementEarnedCallbackRef()) {
            owner_.achievementEarnedCallbackRef()(achievementId, achName);
        }
        // Inside the branch, not after it. ACHIEVEMENT_EARNED is what raises
        // FrameXML's badge, and it is about the player who is reading it -
        // somebody else's is CHAT_MSG_ACHIEVEMENT, which the server sends
        // separately and this client already handles. Fired for everyone, a
        // stranger turning in a quest nearby put their achievement badge on
        // this player's screen.
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("ACHIEVEMENT_EARNED",
                                           {std::to_string(achievementId)});
    }
    // No line for somebody else's, because the server has already sent one.
    // AchievementMgr::SendAchievementEarned puts a CHAT_MSG_ACHIEVEMENT through
    // the say range and then sends this packet through the same range, so both
    // arrive together - and this client parses that chat type, substitutes the
    // achievement link into it, gives it a tab and fires CHAT_MSG_ACHIEVEMENT.
    // Writing one here as well was the third copy of the same news.

    LOG_INFO("SMSG_ACHIEVEMENT_EARNED: guid=0x", std::hex, guid, std::dec,
             " achievementId=", achievementId, " self=", isSelf,
             achName.empty() ? "" : " name=", achName);
}

// SMSG_EQUIPMENT_SET_LIST - moved to InventoryHandler

// ============================================================
// Pet spell methods (moved from GameHandler)
// ============================================================

void SpellHandler::handlePetSpells(network::Packet& packet) {
    // 0x005d6b90 begins by emptying the pet's spell history (0x00804420).
    petHistory_.clear();
    const size_t remaining = packet.getRemainingSize();
    // 0x005d6b90 lets go of the attack (0x00c234dc), then holds it again
    // for a pet in combat (0x0071afb0: UNIT_FLAG_PET_IN_COMBAT).
    owner_.setPetAttacking(false);
    if (remaining < 8) {
        owner_.petGuidRef() = 0;
        // No pet: its combo points go (0x005d6900 -> 0x005d34b0).
        owner_.petComboTargetRef() = 0;
        owner_.petComboPointsRef() = 0;
        owner_.petSpellListRef().clear();
        owner_.petAutocastSpellsRef().clear();
        owner_.petAutocastableSpellsRef().clear();
        memset(owner_.petActionSlotsRef(), 0, sizeof(owner_.petActionSlotsRef()));
        LOG_INFO("SMSG_PET_SPELLS: pet cleared");
        owner_.fireAddonEvent("UNIT_PET", {"player"});
        // The pet frame and the pet action bar both rebuild from this one;
        // UNIT_PET alone tells them the pet changed but not that its interface
        // should be redrawn.
        owner_.fireAddonEvent("PET_UI_UPDATE", {});
        owner_.updatePetActionBar();  // 0x005d4ad0
        return;
    }

    owner_.petGuidRef() = packet.readUInt64();
    // Ask what it is called. Nothing else does, and the name a player gave a
    // pet comes back in answer to this and nothing else.
    if (owner_.petGuidRef() != 0) requestPetName(owner_.petGuidRef());
    if (const auto pet = owner_.getEntityManager().getEntity(owner_.petGuidRef()); pet && pet->isUnit())
        owner_.setPetAttacking((entityField(*pet, UF::UNIT_FIELD_FLAGS) & 0x800u) != 0);
    if (owner_.petGuidRef() == 0) {
        owner_.petComboTargetRef() = 0;
        owner_.petComboPointsRef() = 0;
        owner_.petSpellListRef().clear();
        owner_.petAutocastSpellsRef().clear();
        owner_.petAutocastableSpellsRef().clear();
        memset(owner_.petActionSlotsRef(), 0, sizeof(owner_.petActionSlotsRef()));
        LOG_INFO("SMSG_PET_SPELLS: pet cleared (guid=0)");
        owner_.fireAddonEvent("UNIT_PET", {"player"});
        // The pet frame and the pet action bar both rebuild from this one;
        // UNIT_PET alone tells them the pet changed but not that its interface
        // should be redrawn.
        owner_.fireAddonEvent("PET_UI_UPDATE", {});
        owner_.updatePetActionBar();  // 0x005d4ad0
        return;
    }

    // Verified against AzerothCore's Player::PetSpellInitialize, which writes
    //   guid(8) family(2) duration(4) react(1) command(1) flags(2)
    // and then one uint32 per action-bar slot, a uint8 spell count, and one
    // packed uint32 per spell.
    //
    // This read the duration as a uint16 and never read the flags, so the
    // action bar started four bytes early: every slot held the one before it,
    // the first held the react and command bytes, and the last was lost. The
    // react and command themselves came out of the duration's upper half -
    // zero for a permanent pet, which is why they looked plausible.
    //
    // The family field is what a WotLK server sends for pet talents. Before
    // that the packet is the same one without it.
    do {
        if (!isPreWotlk()) {
            if (!packet.hasRemaining(2)) break;
            packet.readUInt16();  // creature family
        }
        if (!packet.hasRemaining(4)) break;
        packet.readUInt32();      // duration in ms; zero for a permanent pet

        if (!packet.hasRemaining(4)) break;
        owner_.petReactRef()   = packet.readUInt8();
        owner_.petCommandRef() = packet.readUInt8();
        packet.readUInt16();      // flags, unused by the server too

        if (!packet.hasRemaining(GameHandler::PET_ACTION_BAR_SLOTS * 4u)) break;
        for (int i = 0; i < GameHandler::PET_ACTION_BAR_SLOTS; ++i) {
            owner_.petActionSlotsRef()[i] = packet.readUInt32();
        }

        if (!packet.hasRemaining(1)) break;
        uint8_t spellCount = packet.readUInt8();
        owner_.petSpellListRef().clear();
        owner_.petAutocastSpellsRef().clear();
        owner_.petAutocastableSpellsRef().clear();
        for (uint8_t i = 0; i < spellCount; ++i) {
            // One uint32, not six bytes: MAKE_UNIT_ACTION_BUTTON puts the spell
            // in the low twenty-four bits and the state in the top byte, the
            // same packing the action-bar slots above use and that
            // GetPetActionInfo already masks for. Read as four plus two, the
            // spell id carried the state and the autocast flag was two bytes of
            // the next spell - and every spell after the first was misaligned.
            if (!packet.hasRemaining(4)) break;
            const uint32_t packed  = packet.readUInt32();
            const uint32_t spellId = packed & 0x00FFFFFFu;
            const uint8_t  state   = static_cast<uint8_t>(packed >> 24);
            if (spellId == 0) continue;
            owner_.petSpellListRef().push_back(spellId);
            // ACT_ENABLED is 0xC1 and ACT_DISABLED 0x81 - 0x40 is the autocast
            // bit. The old test was & 0x0001, which is ACT_PASSIVE's bit.
            if (state & 0x40) {
                owner_.petAutocastSpellsRef().insert(spellId);
            }
            if (state & 0x80) owner_.petAutocastableSpellsRef().insert(spellId);
        }

        // The cooldowns: spell, category (uint16), the time left of its
        // cooldown and of its category's, the latter's top bit holding it
        // until an event - each into the pet's history (0x00805b10).
        if (!packet.hasRemaining(1)) break;
        const uint8_t cooldownCount = packet.readUInt8();
        loadSpellNameCache();
        const int64_t now = historyNowMs();
        for (uint8_t i = 0; i < cooldownCount && packet.hasRemaining(14); ++i) {
            const uint32_t spellId = packet.readUInt32();
            const uint16_t category = packet.readUInt16();
            const uint32_t cooldown = packet.readUInt32();
            const uint32_t categoryCooldown = packet.readUInt32();
            const auto it = owner_.spellNameCacheRef().find(spellId);
            const int32_t recovery = it != owner_.spellNameCacheRef().end() ? static_cast<int32_t>(it->second.recoveryMs) : 0;
            const int32_t categoryRecovery =
                it != owner_.spellNameCacheRef().end() ? static_cast<int32_t>(it->second.categoryRecoveryMs) : 0;
            petHistory_.add(spell_history::fromRemaining(
                spellId, category, static_cast<int32_t>(cooldown), static_cast<int32_t>(categoryCooldown & 0x7FFFFFFFu),
                (categoryCooldown & 0x80000000u) != 0, recovery,
                categoryRecovery, now, [&](int32_t ms) { return moddedCooldown(spellId, ms); }));
        }
    } while (false);

    LOG_INFO("SMSG_PET_SPELLS: petGuid=0x", std::hex, owner_.petGuidRef(), std::dec,
             " react=", static_cast<int>(owner_.petReactRef()), " command=", static_cast<int>(owner_.petCommandRef()),
             " spells=", owner_.petSpellListRef().size());
    owner_.fireAddonEvent("UNIT_PET", {"player"});
        // The pet frame and the pet action bar both rebuild from this one;
        // UNIT_PET alone tells them the pet changed but not that its interface
        // should be redrawn.
        owner_.fireAddonEvent("PET_UI_UPDATE", {});
    // The bonus bar takes the pet's actions or gives them back, and
    // PET_BAR_UPDATE (0x005d6xxx -> 0x005d4ad0).
    owner_.updatePetActionBar();
}

void SpellHandler::sendPetAction(uint32_t action, uint64_t targetGuid) {
    if (!owner_.hasPet() || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto pkt = PetActionPacket::build(owner_.petGuidRef(), action, targetGuid);
    owner_.getSocket()->send(pkt);
    // The attack command holds the pet to be attacking (0x005d3790, command
    // 2 -> 0x00c234dc).
    if (action == pet::packPetAction(pet::ActionType::Command, pet::kAttack)) owner_.setPetAttacking(true);
    LOG_DEBUG("sendPetAction: petGuid=0x", std::hex, owner_.petGuidRef(),
              " action=0x", action, " target=0x", targetGuid, std::dec);
}

void SpellHandler::togglePetSpellAutocast(uint32_t spellId) {
    if (owner_.petGuidRef() == 0 || spellId == 0 || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    bool currentlyOn = owner_.petAutocastSpellsRef().count(spellId) != 0;
    uint8_t newState = currentlyOn ? 0 : 1;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_SPELL_AUTOCAST));
    pkt.writeUInt64(owner_.petGuidRef());
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(newState);
    owner_.getSocket()->send(pkt);
    if (newState)
        owner_.petAutocastSpellsRef().insert(spellId);
    else
        owner_.petAutocastSpellsRef().erase(spellId);
    LOG_DEBUG("togglePetSpellAutocast: spellId=", spellId, " autocast=", static_cast<int>(newState));
}

void SpellHandler::renamePet(const std::string& newName) {
    if (owner_.petGuidRef() == 0 || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (newName.empty() || newName.size() > 12) return;
    auto packet = PetRenamePacket::build(owner_.petGuidRef(), newName, 0);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_PET_RENAME: petGuid=0x", std::hex, owner_.petGuidRef(), std::dec, " name='", newName, "'");
}

void SpellHandler::handleListStabledPets(network::Packet& packet) {
    constexpr size_t kMinHeader = 8 + 1 + 1;
    if (!packet.hasRemaining(kMinHeader)) {
        LOG_WARNING("MSG_LIST_STABLED_PETS: packet too short (", packet.getSize(), ")");
        return;
    }
    const uint64_t stableBefore = owner_.stableWindowOpenRef() ? owner_.stableMasterGuidRef() : 0;
    owner_.stableMasterGuidRef() = packet.readUInt64();
    // 0x005a11a0: a stable master other than the one open sets the
    // interaction object.
    if (go_interaction::windowSetsInteraction(owner_.stableMasterGuidRef(), stableBefore)) {
        owner_.setInteractionObject(owner_.stableMasterGuidRef(), go_interaction::Window::Stable);
    }
    uint8_t petCount  = packet.readUInt8();
    owner_.stableNumSlotsRef()   = packet.readUInt8();

    owner_.stabledPetsRef().clear();
    owner_.stabledPetsRef().reserve(petCount);

    for (uint8_t i = 0; i < petCount; ++i) {
        // petNumber(4) + entry(4) + level(4) = 12 bytes before the name string
        if (!packet.hasRemaining(12)) break;
        GameHandler::StabledPet pet;
        pet.petNumber = packet.readUInt32();
        pet.entry     = packet.readUInt32();
        pet.level     = packet.readUInt32();
        pet.name      = packet.readString();
        // One byte after the name, and one only: SendStablePet writes
        // uint32 PetNumber, uint32 CreatureId, uint32 Level, the name, then
        // `uint8(1)` for the pet that is out and `uint8(2)` for one in a stable
        // slot. There is no display id on the wire.
        //
        // This read four bytes for one and then a fifth for the flag, so the
        // display id swallowed the flag plus three bytes of the next pet's
        // number, the flag came from that number's fourth byte, and every pet
        // after the first was read at the wrong offset - with the last dropped
        // for want of five bytes that were never there.
        if (!packet.hasRemaining(1)) break;
        pet.isActive  = (packet.readUInt8() == 1);
        owner_.stabledPetsRef().push_back(std::move(pet));
    }

    owner_.stableWindowOpenRef() = true;
    LOG_INFO("MSG_LIST_STABLED_PETS: stableMasterGuid=0x", std::hex, owner_.stableMasterGuidRef(), std::dec,
             " petCount=", static_cast<int>(petCount), " numSlots=", static_cast<int>(owner_.stableNumSlotsRef()));
    for (const auto& p : owner_.stabledPetsRef()) {
        LOG_DEBUG("  Pet: number=", p.petNumber, " entry=", p.entry,
                  " level=", p.level, " name='", p.name, "' active=", p.isActive);
    }
    // This packet both opens the stable window and is the only thing that
    // refreshes it, so it carries both events. Neither was fired, which is why
    // the original interface's stable could not appear.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("PET_STABLE_SHOW", {});
        owner_.addonEventCallbackRef()("PET_STABLE_UPDATE", {});
    }
}

// ============================================================
// Cast state methods (moved from GameHandler)
// ============================================================

void SpellHandler::stopCasting() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot stop casting: not in world or not connected");
        return;
    }

    if (!casting_) {
        return;
    }

    if (owner_.pendingGameObjectInteractGuidRef() == 0 && currentCastSpellId_ != 0) {
        auto packet = CancelCastPacket::build(currentCastSpellId_);
        owner_.getSocket()->send(packet);
    }

    casting_ = false;
    castIsChannel_ = false;
    currentCastSpellId_ = 0;
    castTimeRemaining_ = 0.0f;
    castTimeTotal_ = 0.0f;
    owner_.pendingGameObjectInteractGuidRef() = 0;
    owner_.lastInteractedGoGuidRef() = 0;
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;

    LOG_INFO("Cancelled spell cast");
}

void SpellHandler::resetCastState() {
    restorationActive_ = false;
    restorationSpellId_ = 0;
    restorationTimeRemaining_ = 0.0f;
    restorationTimeTotal_ = 0.0f;
    casting_ = false;
    castIsChannel_ = false;
    currentCastSpellId_ = 0;
    castTimeRemaining_ = 0.0f;
    castTimeTotal_ = 0.0f;  // Must match castTimeRemaining_ to keep getCastProgress() == 0
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;
    owner_.pendingGameObjectInteractGuidRef() = 0;
    // lastInteractedGoGuid_ is intentionally NOT cleared here - it must survive
    // until the server delivers SMSG_LOOT_RESPONSE after the cast completes.
    // InventoryHandler::handleLootResponse() clears it once loot has opened
    // (src/game/inventory_handler.cpp). Previously it was cleared here, which
    // meant the client-side cast-timer fallback destroyed the guid before
    // SMSG_SPELL_GO arrived, preventing loot from opening on quest chests.
}

void SpellHandler::resetAllState() {
    knownSpells_.clear();
    playerHistory_.clear();  // 0x00802a50
    petHistory_.clear();
    playerAuras_.clear();
    targetAuras_.clear();
    unitAurasCache_.clear();
    unitCastStates_.clear();
    missilesInFlight_.clear();
    // A spell the cursor held for a place or a unit goes with the character
    // (0x00809a60): kept, the next one's first click cast it, by the last
    // character's guid.
    groundTarget_ = GroundTarget{};
    unitTarget_ = UnitTarget{};
    resetCastState();
    resetTalentState();
}

void SpellHandler::newWorld() {
    cancelGroundTargeting();
    playerHistory_.clear();
    petHistory_.clear();
    playerCooldownsChanged();
}

void SpellHandler::resetTalentState() {
    learnedTalents_[0].clear();
    learnedTalents_[1].clear();
    learnedGlyphs_[0].fill(0);
    learnedGlyphs_[1].fill(0);
    unspentTalentPoints_[0] = 0;
    unspentTalentPoints_[1] = 0;
    activeTalentSpec_ = 0;
}

void SpellHandler::clearUnitCaches() {
    unitCastStates_.clear();
    unitAurasCache_.clear();
}

// ============================================================
// Aura duration update (moved from GameHandler)
// ============================================================

void SpellHandler::handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs) {
    if (slot >= playerAuras_.size()) return;
    if (playerAuras_[slot].isEmpty()) return;
    playerAuras_[slot].durationMs = static_cast<int32_t>(durationMs);
    playerAuras_[slot].receivedAtMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    refreshRestorationFromPlayerAuras();
}

// ============================================================
// Spell DBC / Cache methods (moved from GameHandler)
// ============================================================

static const std::string SPELL_EMPTY_STRING;

namespace {
// 0x00739650's columns (cast_check::Spell) and the ones 0x0080b5d0 and
// 0x008091d0 read beside them, named by the WotLK layout alone.
struct CastCheckColumns {
    uint32_t mechanic, stances, stancesNot, casterState, casterStateNot, casterAura, excludeAura, interrupt,
        auraInterrupt, channelInterrupt, powerType, manaCost, perLevel, costPct, baseLevel, maxLevel,
        castTimeIndex, effectMechanic, trigger, prevention, minFaction, minRep, areaGroup, itemClass,
        itemSubClassMask, runeCost, targetStateNot, targetAura, excludeTargetAura, totem, totemCategory;

    template <class Layout>
    explicit CastCheckColumns(const Layout* l) {
        const auto named = [&](const char* name) { return l ? l->tryField(name) : 0xFFFFFFFFu; };
        mechanic = named("Mechanic"), stances = named("Stances"), stancesNot = named("StancesNot");
        casterState = named("CasterAuraState"), casterStateNot = named("CasterAuraStateNot");
        casterAura = named("CasterAuraSpell"), excludeAura = named("ExcludeCasterAuraSpell");
        interrupt = named("InterruptFlags"), auraInterrupt = named("AuraInterruptFlags");
        channelInterrupt = named("ChannelInterruptFlags"), powerType = named("PowerType");
        manaCost = named("ManaCost"), perLevel = named("ManaCostPerLevel"), costPct = named("ManaCostPercentage");
        baseLevel = named("BaseLevel"), maxLevel = named("MaxLevel"), castTimeIndex = named("CastingTimeIndex");
        effectMechanic = named("EffectMechanic0"), trigger = named("EffectTriggerSpell0");
        prevention = named("PreventionType"), minFaction = named("MinFactionId"), minRep = named("MinReputation");
        areaGroup = named("AreaGroupId"), itemClass = named("EquippedItemClass");
        itemSubClassMask = named("EquippedItemSubClassMask"), runeCost = named("RuneCostID");
        targetStateNot = named("TargetAuraStateNot"), targetAura = named("TargetAuraSpell");
        excludeTargetAura = named("ExcludeTargetAuraSpell"), totem = named("Totem");
        totemCategory = named("TotemCategory");
    }

    void read(GameHandler::SpellNameEntry& entry, const pipeline::DBCFile& dbc, uint32_t i) const {
        const uint32_t fieldCount = dbc.getFieldCount();
        const auto u = [&](uint32_t f) { return f < fieldCount ? dbc.getUInt32(i, f) : 0u; };
        const auto n = [&](uint32_t f) { return static_cast<int32_t>(u(f)); };
        entry.mechanic = u(mechanic);
        for (uint32_t w = 0; w < 2; ++w) {
            if (stances < fieldCount) entry.stances[w] = u(stances + w);
            if (stancesNot < fieldCount) entry.stancesNot[w] = u(stancesNot + w);
            if (totem < fieldCount) entry.totems[w] = u(totem + w);
            if (totemCategory < fieldCount) entry.totemCategories[w] = u(totemCategory + w);
        }
        entry.casterAuraState = u(casterState);
        entry.casterAuraStateNot = u(casterStateNot);
        entry.casterAuraSpell = u(casterAura);
        entry.excludeCasterAuraSpell = u(excludeAura);
        entry.interruptFlags = u(interrupt);
        entry.auraInterruptFlags = u(auraInterrupt);
        entry.channelInterruptFlags = u(channelInterrupt);
        entry.powerType = u(powerType);
        entry.manaCost = n(manaCost);
        entry.manaCostPerLevel = n(perLevel);
        entry.manaCostPct = n(costPct);
        entry.baseLevel = n(baseLevel);
        entry.maxLevel = n(maxLevel);
        entry.castingTimeIndex = u(castTimeIndex);
        for (uint32_t k = 0; k < 3; ++k) {
            if (effectMechanic < fieldCount) entry.effectMechanic[k] = u(effectMechanic + k);
            if (trigger < fieldCount) entry.effectTriggerSpell[k] = u(trigger + k);
        }
        entry.preventionType = u(prevention);
        entry.minFactionId = u(minFaction);
        entry.minReputation = u(minRep);
        entry.areaGroupId = u(areaGroup);
        if (itemClass < fieldCount) entry.equippedItemClass = n(itemClass);
        entry.equippedItemSubClassMask = u(itemSubClassMask);
        entry.runeCostId = u(runeCost);
        entry.targetAuraStateNot = u(targetStateNot);
        entry.targetAuraSpell = u(targetAura);
        entry.excludeTargetAuraSpell = u(excludeTargetAura);
    }
};
}  // namespace

void SpellHandler::loadSpellNameCache() const {
    if (owner_.spellNameCacheLoadedRef()) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    owner_.spellNameCacheLoadedRef() = true;

    auto dbc = am->loadDBC("Spell.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("Trainer: Could not load Spell.dbc for spell names");
        return;
    }

    if (dbc->getFieldCount() < 148) {
        LOG_WARNING("Trainer: Spell.dbc has too few fields (", dbc->getFieldCount(), ")");
        return;
    }

    const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;

    uint32_t schoolMaskField = 0, schoolEnumField = 0;
    bool hasSchoolMask = false, hasSchoolEnum = false;
    if (spellL) {
        uint32_t f = spellL->field("SchoolMask");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) { schoolMaskField = f; hasSchoolMask = true; }
        f = spellL->field("SchoolEnum");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) { schoolEnumField = f; hasSchoolEnum = true; }
    }

    uint32_t dispelField = 0xFFFFFFFF;
    bool hasDispelField = false;
    if (spellL) {
        uint32_t f = spellL->field("DispelType");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) { dispelField = f; hasDispelField = true; }
    }

    uint32_t attrExField = 0xFFFFFFFF;
    bool hasAttrExField = false;
    if (spellL) {
        uint32_t f = spellL->field("AttributesEx");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) { attrExField = f; hasAttrExField = true; }
    }

    // AttributesEx3 and the spell family, for the modifiers (0x007fd970).
    const uint32_t attrEx3Field = spellL ? spellL->tryField("AttributesEx3") : 0xFFFFFFFF;
    const uint32_t attrEx2Field = spellL ? spellL->tryField("AttributesEx2") : 0xFFFFFFFF;
    const uint32_t creatureTypeField = spellL ? spellL->tryField("TargetCreatureType") : 0xFFFFFFFF;
    const uint32_t classMaskField = spellL ? spellL->tryField("EffectSpellClassMask") : 0xFFFFFFFF;
    const uint32_t categoryField = spellL ? spellL->tryField("Category") : 0xFFFFFFFF;
    const uint32_t startCategoryField = spellL ? spellL->tryField("StartRecoveryCategory") : 0xFFFFFFFF;
    const uint32_t startTimeField = spellL ? spellL->tryField("StartRecoveryTime") : 0xFFFFFFFF;
    const uint32_t dmgClassField = spellL ? spellL->tryField("DmgClass") : 0xFFFFFFFF;
    const uint32_t attrEx4Field = spellL ? spellL->tryField("AttributesEx4") : 0xFFFFFFFF;
    const uint32_t attrEx7Field = spellL ? spellL->tryField("AttributesEx7") : 0xFFFFFFFF;
    const uint32_t stackAmountField = spellL ? spellL->tryField("StackAmount") : 0xFFFFFFFF;
    const uint32_t attrEx5Field = spellL ? spellL->tryField("AttributesEx5") : 0xFFFFFFFF;
    const uint32_t attrEx6Field = spellL ? spellL->tryField("AttributesEx6") : 0xFFFFFFFF;
    const uint32_t activeIconField = spellL ? spellL->tryField("ActiveIconID") : 0xFFFFFFFF;
    const uint32_t familyField = spellL ? spellL->tryField("SpellFamilyName") : 0xFFFFFFFF;
    const uint32_t familyFlagsField = spellL ? spellL->tryField("SpellFamilyFlags") : 0xFFFFFFFF;
    const uint32_t familyFlagWords = isPreWotlk() ? 2u : 3u;

    // The base attribute word, beside the Ex one that was already read. Bit 6
    // marks a passive, which is the only thing asked of it so far.
    uint32_t attrField = 0xFFFFFFFF;
    bool hasAttrField = false;
    if (spellL) {
        uint32_t f = spellL->field("Attributes");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) { attrField = f; hasAttrField = true; }
    }

    uint32_t tooltipField = 0xFFFFFFFF;
    if (spellL) {
        uint32_t f = spellL->field("Tooltip");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) tooltipField = f;
    }
    // The full effect Description (e.g. "Restores X health over Y sec. ...become well
    // fed...") is richer than the short Tooltip; prefer it and fall back to Tooltip.
    uint32_t descriptionField = 0xFFFFFFFF;
    if (spellL) {
        uint32_t f = spellL->field("Description");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) descriptionField = f;
    }

    // Targets: SpellCastTargets mask the spell demands. Item-enhancement spells
    // (sharpening stones, weightstones, weapon oils) set TARGET_FLAG_ITEM here.
    uint32_t targetsField = 0xFFFFFFFF;
    if (spellL) {
        uint32_t f = spellL->field("Targets");
        if (f != 0xFFFFFFFF && f < dbc->getFieldCount()) targetsField = f;
    }

    // Cache field indices before the loop to avoid repeated layout lookups
    const uint32_t idField   = spellL ? (*spellL)["ID"]   : 0;
    const uint32_t nameField = spellL ? (*spellL)["Name"] : 136;
    const uint32_t rankField = spellL ? (*spellL)["Rank"] : 153;
    const uint32_t fieldCount = dbc->getFieldCount();

    // Which row of SpellDescriptionVariables.dbc a spell's $<name> tokens are
    // defined in. Named in the layout where the layout knows it; the WotLK
    // column otherwise, and only for a file that really is WotLK's - an index
    // written out here is right for one file and wrong for every other, which
    // is what the reagent columns above were.
    uint32_t descVarField = 0xFFFFFFFF;
    if (spellL) {
        uint32_t f = spellL->field("DescriptionVariables");
        if (f != 0xFFFFFFFF && f < fieldCount) descVarField = f;
    }
    if (descVarField == 0xFFFFFFFF && fieldCount == 234) descVarField = 232;

    // The declarations themselves, read once. Thirty rows in 3.3.5.
    if (owner_.spellDescriptionVariablesRef().empty()) {
        if (auto varDbc = am->loadDBC("SpellDescriptionVariables.dbc");
            varDbc && varDbc->isLoaded() && varDbc->getFieldCount() >= 2) {
            for (uint32_t i = 0; i < varDbc->getRecordCount(); ++i) {
                const uint32_t vid = varDbc->getUInt32(i, 0);
                std::string text = varDbc->getString(i, 1);
                if (vid != 0 && !text.empty()) {
                    owner_.spellDescriptionVariablesRef()[vid] = std::move(text);
                }
            }
            LOG_INFO("Loaded ", owner_.spellDescriptionVariablesRef().size(),
                     " spell description variable set(s)");
        }
    }
    // What a recipe makes and what it consumes, from the layout rather than
    // from three WotLK column numbers written out here.
    //
    // These were 71, 107, 52 and 60 - correct for the 234-field WotLK file and
    // for nothing else. TBC's effect block starts at 65 and vanilla's at 61,
    // because both carry an EffectBaseDice and an EffectDicePerLevel that
    // WotLK dropped, and the reagents sit six and ten columns earlier again.
    // The only guard was the file having *enough* fields, which every layout
    // passes, so on Classic and TBC the effect read was some neighbouring
    // column and never equalled CREATE_ITEM: no spell had a created item, no
    // spell had reagents, and getCraftingRecipes therefore filtered every
    // recipe out. The trade skill window came up empty - and since
    // tradeskillOpenerSkillLine will not open a window with nothing in it, it
    // did not come up at all. Smelting on Classic was the report; every
    // profession on Classic and TBC was the fault.
    //
    // A wrong column here is exactly the shape that cannot be seen by reading:
    // DBCFile answers zero for a column past the end and a plausible number
    // for one inside it, and both look like data.
    const uint32_t reagentField      = spellL ? spellL->field("Reagent0") : 0xFFFFFFFF;
    const uint32_t reagentCountField = spellL ? spellL->field("ReagentCount0") : 0xFFFFFFFF;
    const uint32_t itemTypeFields[3] = {
        spellL ? spellL->field("EffectItemType0") : 0xFFFFFFFF,
        spellL ? spellL->field("EffectItemType1") : 0xFFFFFFFF,
        spellL ? spellL->field("EffectItemType2") : 0xFFFFFFFF,
    };
    const uint32_t ebp0Field = spellL ? spellL->field("EffectBasePoints0") : 0xFFFFFFFF;
    const uint32_t ebp1Field = spellL ? spellL->field("EffectBasePoints1") : 0xFFFFFFFF;
    const uint32_t ebp2Field = spellL ? spellL->field("EffectBasePoints2") : 0xFFFFFFFF;
    const uint32_t effect0Field = spellL ? spellL->field("Effect0") : 0xFFFFFFFF;
    const uint32_t effect1Field = spellL ? spellL->field("Effect1") : 0xFFFFFFFF;
    const uint32_t effect2Field = spellL ? spellL->field("Effect2") : 0xFFFFFFFF;
    // Every column the recipe read needs, named and in range. Named and in
    // range is the whole question: the old test was that the file had enough
    // fields for the WotLK numbers, which says nothing about whether those are
    // the right columns for this file - and it passed on all three.
    const bool hasReagentFields =
        reagentField != 0xFFFFFFFF && reagentCountField != 0xFFFFFFFF &&
        reagentField + 8 <= fieldCount && reagentCountField + 8 <= fieldCount;
    const bool hasEffectFields =
        effect0Field != 0xFFFFFFFF && effect2Field < fieldCount &&
        itemTypeFields[0] != 0xFFFFFFFF && itemTypeFields[2] < fieldCount;
    // Say so, because the consequence is a feature quietly gone rather than
    // anything that looks like a fault. With either of these false no spell
    // gets a created item or a reagent, getCraftingRecipes filters every recipe
    // out, and tradeskillOpenerSkillLine will not open a window with nothing in
    // it - so no profession opens at all, for any character, with nothing said.
    //
    // Reported as "first aid window not opening". The layout in the installed
    // data directory is a copy taken when the assets were extracted, and it
    // predated the columns this reads: the repository had Reagent0 and
    // EffectItemType0 and the file the client actually loads did not. Names
    // still worked, because their fallback happens to be the right column for
    // WotLK, which is what made the cache look loaded.
    if (!hasReagentFields || !hasEffectFields) {
        LOG_WARNING("Spell.dbc layout is missing the columns recipes are read "
                    "from - Reagent0/ReagentCount0 ",
                    hasReagentFields ? "ok" : "MISSING",
                    ", Effect0-2/EffectItemType0-2 ",
                    hasEffectFields ? "ok" : "MISSING",
                    ". No recipe will be found and no profession window will "
                    "open. The layout is dbc_layouts.json in the data "
                    "directory, which is copied there when assets are "
                    "extracted and does not refresh itself.");
    }
    const uint32_t aura0Field = spellL ? spellL->field("EffectApplyAuraName0") : 0xFFFFFFFF;
    const uint32_t misc0Field = spellL ? spellL->tryField("EffectMiscValue0") : 0xFFFFFFFF;
    const uint32_t miscB0Field = spellL ? spellL->tryField("EffectMiscValueB0") : 0xFFFFFFFF;
    const uint32_t aura1Field = spellL ? spellL->field("EffectApplyAuraName1") : 0xFFFFFFFF;
    const uint32_t aura2Field = spellL ? spellL->field("EffectApplyAuraName2") : 0xFFFFFFFF;
    const uint32_t implicitTargetAField =
        spellL ? spellL->field("EffectImplicitTargetA") : 0xFFFFFFFF;
    const uint32_t implicitTargetBField =
        spellL ? spellL->tryField("EffectImplicitTargetB") : 0xFFFFFFFF;
    // The three radius indices follow the three ImplicitTargetB columns in
    // every layout (Spell.dbc 92-94 in WotLK, 88-90 in vanilla).
    uint32_t radiusIndexField = spellL ? spellL->tryField("EffectRadiusIndex") : 0xFFFFFFFF;
    if (radiusIndexField == 0xFFFFFFFF && implicitTargetBField != 0xFFFFFFFF)
        radiusIndexField = implicitTargetBField + 3;
    const uint32_t durIdxField = spellL ? spellL->field("DurationIndex") : 0xFFFFFFFF;
    const uint32_t rangeIdxField = spellL ? spellL->field("RangeIndex") : 0xFFFFFFFF;
    const uint32_t targetAuraStateField = spellL ? spellL->field("TargetAuraState") : 0xFFFFFFFF;
    const uint32_t spellVisualIdField = spellL ? spellL->field("SpellVisualID") : 0xFFFFFFFF;
    // Only the WotLK layout names Speed: it is what gates spell missiles.
    const uint32_t speedField = spellL ? spellL->tryField("Speed") : 0xFFFFFFFF;
    const uint32_t spellMissileField = spellL ? spellL->tryField("SpellMissileID") : 0xFFFFFFFF;
    const uint32_t powerDisplayField = spellL ? spellL->tryField("PowerDisplayId") : 0xFFFFFFFF;
    // Read off the file's own shape. Only TBC's layout named these two, so on
    // WotLK, Classic and Turtle every cooldown this client worked out for itself
    // came back zero - which is most of them, since the server sends a cooldown
    // only when it is correcting one the client should already have.
    // 0x00739650's (cast_check::Spell), named by the WotLK layout alone.
    const CastCheckColumns castColumns(spellL);
    const auto timing = pipeline::detectSpellTimingFields(dbc.get(), spellL);
    const uint32_t recoveryField = timing.recoveryTime;
    const uint32_t categoryRecoveryField = timing.categoryRecoveryTime;

    uint32_t count = dbc->getRecordCount();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = dbc->getUInt32(i, idField);
        if (id == 0) continue;
        std::string name = dbc->getString(i, nameField);
        std::string rank = dbc->getString(i, rankField);
        if (!name.empty()) {
            GameHandler::SpellNameEntry entry;
            entry.name = std::move(name);
            entry.rank = std::move(rank);
            if (descriptionField != 0xFFFFFFFF) {
                entry.description = dbc->getString(i, descriptionField);
            }
            if (tooltipField != 0xFFFFFFFF) {
                entry.auraDescription = dbc->getString(i, tooltipField);
            }
            if (entry.description.empty()) entry.description = entry.auraDescription;
            if (descVarField != 0xFFFFFFFF) {
                entry.descriptionVariableId = dbc->getUInt32(i, descVarField);
            }
            if (hasSchoolMask) {
                entry.schoolMask = dbc->getUInt32(i, schoolMaskField);
            } else if (hasSchoolEnum) {
                static constexpr uint32_t enumToBitmask[] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40};
                uint32_t e = dbc->getUInt32(i, schoolEnumField);
                entry.schoolMask = (e < 7) ? enumToBitmask[e] : 0;
            }
            if (hasDispelField) {
                entry.dispelType = static_cast<uint8_t>(dbc->getUInt32(i, dispelField));
            }
            if (hasAttrExField) {
                entry.attrEx = dbc->getUInt32(i, attrExField);
            }
            if (hasAttrField) {
                entry.attr = dbc->getUInt32(i, attrField);
            }
            if (attrEx3Field < fieldCount) entry.attrEx3 = dbc->getUInt32(i, attrEx3Field);
            if (attrEx2Field < fieldCount) entry.attrEx2 = dbc->getUInt32(i, attrEx2Field);
            if (creatureTypeField < fieldCount) entry.targetCreatureType = dbc->getUInt32(i, creatureTypeField);
            if (categoryField < fieldCount) entry.category = dbc->getUInt32(i, categoryField);
            if (startCategoryField < fieldCount) entry.startRecoveryCategory = dbc->getUInt32(i, startCategoryField);
            if (startTimeField < fieldCount) entry.startRecoveryMs = dbc->getUInt32(i, startTimeField);
            if (dmgClassField < fieldCount) entry.dmgClass = dbc->getUInt32(i, dmgClassField);
            if (classMaskField != 0xFFFFFFFF && classMaskField + 9 <= fieldCount) {
                for (uint32_t k = 0; k < 9; ++k)
                    entry.effectClassMask[k / 3][k % 3] = dbc->getUInt32(i, classMaskField + k);
            }
            if (attrEx4Field < fieldCount) entry.attrEx4 = dbc->getUInt32(i, attrEx4Field);
            if (attrEx7Field < fieldCount) entry.attrEx7 = dbc->getUInt32(i, attrEx7Field);
            if (stackAmountField < fieldCount) entry.stackAmount = dbc->getUInt32(i, stackAmountField);
            if (attrEx5Field < fieldCount) entry.attrEx5 = dbc->getUInt32(i, attrEx5Field);
            if (attrEx6Field < fieldCount) entry.attrEx6 = dbc->getUInt32(i, attrEx6Field);
            if (activeIconField < fieldCount) entry.activeIconId = dbc->getUInt32(i, activeIconField);
            if (familyField < fieldCount) entry.spellFamily = dbc->getUInt32(i, familyField);
            // A uint64 before WotLK, three words since (groups 0-63, 0-95).
            for (uint32_t w = 0; w < familyFlagWords; ++w) {
                if (familyFlagsField != 0xFFFFFFFF && familyFlagsField + w < fieldCount)
                    entry.spellFamilyFlags[w] = dbc->getUInt32(i, familyFlagsField + w);
            }
            if (targetsField != 0xFFFFFFFF) {
                entry.targetFlags = dbc->getUInt32(i, targetsField);
            }
            if (targetAuraStateField != 0xFFFFFFFF && targetAuraStateField < fieldCount) {
                entry.targetAuraState = dbc->getUInt32(i, targetAuraStateField);
            }
            castColumns.read(entry, *dbc, i);
            // Load effect base points for $s1/$s2/$s3 tooltip substitution
            if (ebp0Field != 0xFFFFFFFF) entry.effectBasePoints[0] = static_cast<int32_t>(dbc->getUInt32(i, ebp0Field));
            if (ebp1Field != 0xFFFFFFFF) entry.effectBasePoints[1] = static_cast<int32_t>(dbc->getUInt32(i, ebp1Field));
            if (ebp2Field != 0xFFFFFFFF) entry.effectBasePoints[2] = static_cast<int32_t>(dbc->getUInt32(i, ebp2Field));
            if (implicitTargetAField != 0xFFFFFFFF && implicitTargetAField < fieldCount) {
                entry.implicitTargetA = dbc->getUInt32(i, implicitTargetAField);
            }
            // The three effects' columns follow each other.
            for (uint32_t effect = 0; effect < 3; ++effect) {
                if (implicitTargetAField != 0xFFFFFFFF && implicitTargetAField + effect < fieldCount)
                    entry.implicitTargetsA[effect] = dbc->getUInt32(i, implicitTargetAField + effect);
                if (implicitTargetBField != 0xFFFFFFFF && implicitTargetBField + effect < fieldCount)
                    entry.implicitTargetsB[effect] = dbc->getUInt32(i, implicitTargetBField + effect);
            }
            for (uint32_t effect = 0; effect < 3; ++effect) {
                if (radiusIndexField != 0xFFFFFFFF && radiusIndexField + effect < fieldCount)
                    entry.effectRadiusIndex[effect] = dbc->getUInt32(i, radiusIndexField + effect);
            }
            const uint32_t effectFields[3] = {effect0Field, effect1Field, effect2Field};
            const uint32_t auraFields[3]   = {aura0Field, aura1Field, aura2Field};
            for (size_t effect = 0; effect < 3; ++effect) {
                if (effectFields[effect] != 0xFFFFFFFF && effectFields[effect] < fieldCount) {
                    entry.effectIds[effect] = dbc->getUInt32(i, effectFields[effect]);
                }
                if (auraFields[effect] != 0xFFFFFFFF && auraFields[effect] < fieldCount) {
                    entry.effectAuraIds[effect] = dbc->getUInt32(i, auraFields[effect]);
                }
                // The three effects' misc values follow each other.
                if (misc0Field != 0xFFFFFFFF && misc0Field + effect < fieldCount)
                    entry.effectMiscValues[effect] = dbc->getUInt32(i, misc0Field + static_cast<uint32_t>(effect));
                if (miscB0Field != 0xFFFFFFFF && miscB0Field + effect < fieldCount)
                    entry.effectMiscValuesB[effect] = dbc->getUInt32(i, miscB0Field + static_cast<uint32_t>(effect));
            }
            // Duration: read DurationIndex and resolve via SpellDuration.dbc later
            if (durIdxField != 0xFFFFFFFF)
                entry.durationSec = static_cast<float>(dbc->getUInt32(i, durIdxField)); // store index temporarily
            // Range: read RangeIndex and resolve via SpellRange.dbc later
            if (rangeIdxField != 0xFFFFFFFF) {
                entry.rangeIndex = dbc->getUInt32(i, rangeIdxField);
                entry.maxRange = static_cast<float>(entry.rangeIndex); // store index temporarily
            }
            // SpellVisualID: references SpellVisual.dbc for cast/impact M2 effects
            // and the second of the two, the next column (+0x210), which
            // 0x007fa290 shows under violenceLevel 2.
            entry.spellVisualId = spellVisualIdField < fieldCount ? dbc->getUInt32(i, spellVisualIdField) : 0;
            entry.spellVisualId2 = spellVisualIdField < fieldCount - 1 ? dbc->getUInt32(i, spellVisualIdField + 1) : 0;
            if (speedField != 0xFFFFFFFF && speedField < fieldCount)
                entry.missileSpeed = dbc->getFloat(i, speedField);
            if (spellMissileField != 0xFFFFFFFF && spellMissileField < fieldCount)
                entry.spellMissileId = dbc->getUInt32(i, spellMissileField);
            if (powerDisplayField != 0xFFFFFFFF && powerDisplayField < fieldCount)
                entry.powerDisplayId = dbc->getUInt32(i, powerDisplayField);
            if (recoveryField != 0xFFFFFFFF && recoveryField < fieldCount)
                entry.recoveryMs = dbc->getUInt32(i, recoveryField);
            if (categoryRecoveryField != 0xFFFFFFFF && categoryRecoveryField < fieldCount)
                entry.categoryRecoveryMs = dbc->getUInt32(i, categoryRecoveryField);
            if (hasEffectFields) {
                for (int e = 0; e < 3; ++e) {
                    const uint32_t effect = dbc->getUInt32(i, effectFields[e]);
                    if (effect == 24 || effect == 114) {   // CREATE_ITEM, CREATE_ITEM_2
                        entry.createdItemId = dbc->getUInt32(i, itemTypeFields[e]);
                        break;
                    }
                }
            }
            if (hasReagentFields) {
                for (int r = 0; r < 8; ++r) {
                    entry.reagents[r].itemId = dbc->getUInt32(i, reagentField + r);
                    entry.reagents[r].count  = dbc->getUInt32(i, reagentCountField + r);
                }
            }
            owner_.spellNameCacheRef()[id] = std::move(entry);
        }
    }
    auto durDbc = am->loadDBC("SpellDuration.dbc");
    if (durDbc && durDbc->isLoaded()) {
        std::unordered_map<uint32_t, float> durMap;
        for (uint32_t di = 0; di < durDbc->getRecordCount(); ++di) {
            uint32_t durId = durDbc->getUInt32(di, 0);
            int32_t baseMs = static_cast<int32_t>(durDbc->getUInt32(di, 1));
            if (baseMs > 0 && baseMs < 100000000)
                durMap[durId] = baseMs / 1000.0f;
        }
        for (auto& [sid, entry] : owner_.spellNameCacheRef()) {
            uint32_t durIdx = static_cast<uint32_t>(entry.durationSec);
            if (durIdx > 0) {
                auto it = durMap.find(durIdx);
                entry.durationSec = (it != durMap.end()) ? it->second : 0.0f;
            }
        }
    }
    // Resolve the stored RangeIndex into an actual max range. Entries that cannot
    // be resolved fall back to -1 (unknown) so callers keep their old behaviour
    // rather than mistaking a raw index for a distance in yards.
    const auto* rangeL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellRange") : nullptr;
    auto rangeDbc = am->loadDBC("SpellRange.dbc");
    std::unordered_map<uint32_t, float> rangeMap;
    if (rangeDbc && rangeDbc->isLoaded() && rangeL) {
        const uint32_t maxRangeField = rangeL->field("MaxRange");
        if (maxRangeField != 0xFFFFFFFF && maxRangeField < rangeDbc->getFieldCount()) {
            for (uint32_t ri = 0; ri < rangeDbc->getRecordCount(); ++ri) {
                rangeMap[rangeDbc->getUInt32(ri, 0)] = rangeDbc->getFloat(ri, maxRangeField);
            }
        }
    }
    for (auto& [sid, entry] : owner_.spellNameCacheRef()) {
        if (entry.maxRange < 0.0f) continue; // no RangeIndex field in this layout
        auto it = rangeMap.find(static_cast<uint32_t>(entry.maxRange));
        entry.maxRange = (it != rangeMap.end()) ? it->second : -1.0f;
    }
    // Both ends of the range, hostile and friendly. TBC and WotLK keep the
    // two apart (min hostile, min friendly, max hostile, max friendly);
    // vanilla has one of each.
    if (rangeDbc && rangeDbc->isLoaded()) {
        // WotLK's Flags (+0x14) follow: 1 melee, 2 ranged (0x007ff480).
        struct Ends { float min[2]; float max[2]; uint32_t flags; };
        std::unordered_map<uint32_t, Ends> ends;
        const uint32_t n = rangeDbc->getFieldCount();
        for (uint32_t ri = 0; ri < rangeDbc->getRecordCount(); ++ri) {
            Ends e{};
            if (n >= 5) {
                e = {{rangeDbc->getFloat(ri, 1), rangeDbc->getFloat(ri, 2)},
                     {rangeDbc->getFloat(ri, 3), rangeDbc->getFloat(ri, 4)},
                     n >= 6 && !isPreWotlk() ? rangeDbc->getUInt32(ri, 5) : 0u};
            } else if (n >= 3) {
                e = {{rangeDbc->getFloat(ri, 1), rangeDbc->getFloat(ri, 1)},
                     {rangeDbc->getFloat(ri, 2), rangeDbc->getFloat(ri, 2)}, 0u};
            } else {
                continue;
            }
            ends[rangeDbc->getUInt32(ri, 0)] = e;
        }
        for (auto& [sid, entry] : owner_.spellNameCacheRef()) {
            auto it = ends.find(entry.rangeIndex);
            if (it == ends.end()) continue;
            for (int k = 0; k < 2; ++k) {
                entry.rangeMin[k] = it->second.min[k];
                entry.rangeMax[k] = it->second.max[k];
            }
            entry.rangeFlags = it->second.flags;
            entry.hasRangeRow = true;
        }
    }
    // SpellRadius.dbc: ID, Radius, RadiusPerLevel, RadiusMax.
    if (auto radiusDbc = am->loadDBC("SpellRadius.dbc"); radiusDbc && radiusDbc->isLoaded() &&
                                                         radiusDbc->getFieldCount() >= 3) {
        std::unordered_map<uint32_t, std::pair<float, float>> radii;
        for (uint32_t ri = 0; ri < radiusDbc->getRecordCount(); ++ri) {
            radii[radiusDbc->getUInt32(ri, 0)] = {radiusDbc->getFloat(ri, 1), radiusDbc->getFloat(ri, 2)};
        }
        for (auto& [sid, entry] : owner_.spellNameCacheRef()) {
            for (int k = 0; k < 3; ++k) {
                auto it = radii.find(entry.effectRadiusIndex[k]);
                if (it == radii.end()) continue;
                entry.effectRadius[k] = it->second.first;
                entry.effectRadiusPerLevel[k] = it->second.second;
            }
        }
    }
    LOG_INFO("Trainer: Loaded ", owner_.spellNameCacheRef().size(), " spell names from Spell.dbc");
}

void SpellHandler::loadSkillLineAbilityDbc() {
    if (owner_.skillLineAbilityLoadedRef()) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    owner_.skillLineAbilityLoadedRef() = true;

    auto slaDbc = am->loadDBC("SkillLineAbility.dbc");
    if (slaDbc && slaDbc->isLoaded()) {
        const auto* slaL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SkillLineAbility") : nullptr;
        const uint32_t slaSkillField = slaL ? (*slaL)["SkillLineID"] : 1;
        const uint32_t slaSpellField = slaL ? (*slaL)["SpellID"]     : 2;
        const uint32_t slaFieldCount = slaDbc->getFieldCount();
        const bool hasDiffFields = (slaFieldCount > 11);
        for (uint32_t i = 0; i < slaDbc->getRecordCount(); i++) {
            uint32_t skillLineId = slaDbc->getUInt32(i, slaSkillField);
            uint32_t spellId = slaDbc->getUInt32(i, slaSpellField);
            if (spellId > 0 && skillLineId > 0) {
                owner_.spellToSkillLineRef()[spellId] = skillLineId;
                if (hasDiffFields) {
                    uint32_t trivHigh = slaDbc->getUInt32(i, 10);
                    uint32_t trivLow  = slaDbc->getUInt32(i, 11);
                    uint32_t minRank  = slaDbc->getUInt32(i, 7);
                    if (trivHigh > 0 || trivLow > 0) {
                        auto cit = owner_.spellNameCacheRef().find(spellId);
                        if (cit != owner_.spellNameCacheRef().end()) {
                            cit->second.trivialSkillHigh = trivHigh;
                            cit->second.trivialSkillLow  = trivLow;
                            cit->second.minSkillRank     = minRank;
                        }
                    }
                }
            }
        }
        LOG_INFO("Trainer: Loaded ", owner_.spellToSkillLineRef().size(), " skill line abilities");
    }
}

const std::string& SpellHandler::getSpellFocusName(uint32_t focusId) {
    static const std::string kEmpty;
    if (!spellFocusDbcLoaded_) {
        spellFocusDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            auto dbc = am->loadDBC("SpellFocusObject.dbc");
            // Layout is stable across expansions: ID(0) + Name locstring
            // whose English text sits at field 1.
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    uint32_t id = dbc->getUInt32(i, 0);
                    std::string name = dbc->getString(i, 1);
                    if (id != 0 && !name.empty())
                        spellFocusNames_[id] = std::move(name);
                }
                LOG_INFO("Loaded ", spellFocusNames_.size(), " spell focus object names");
            }
        }
    }
    auto it = spellFocusNames_.find(focusId);
    return it != spellFocusNames_.end() ? it->second : kEmpty;
}

const std::string& SpellHandler::getTotemCategoryName(uint32_t categoryId) {
    static const std::string kEmpty;
    if (!totemCategoryDbcLoaded_) {
        totemCategoryDbcLoaded_ = true;
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            // TBC/WotLK only - absent in Vanilla, where totem failures carry
            // item ids instead of category ids.
            auto dbc = am->loadDBC("TotemCategory.dbc");
            // ID(0) + Name locstring whose English text sits at field 1.
            if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 2) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    uint32_t id = dbc->getUInt32(i, 0);
                    std::string name = dbc->getString(i, 1);
                    if (id != 0 && !name.empty())
                        totemCategoryNames_[id] = std::move(name);
                }
                LOG_INFO("Loaded ", totemCategoryNames_.size(), " totem category names");
            }
        }
    }
    auto it = totemCategoryNames_.find(categoryId);
    return it != totemCategoryNames_.end() ? it->second : kEmpty;
}

uint32_t SpellHandler::tradeskillOpenerSkillLine(uint32_t spellId) {
    owner_.loadSpellNameCache();
    owner_.loadSkillLineDbc();
    owner_.loadSkillLineAbilityDbc();

    // SkillLine.dbc categories that hold crafting recipes
    static constexpr uint32_t CAT_SECONDARY  = 9;   // Cooking, First Aid, Fishing
    static constexpr uint32_t CAT_PROFESSION = 11;  // Alchemy, Blacksmithing, ...
    // What "this spell opens the trade skill window" is actually written as.
    static constexpr uint32_t SPELL_EFFECT_TRADE_SKILL = 47;

    // Silent until now, and this chain has four ways to answer no that look
    // identical from outside the client: the profession spell is cast, nothing
    // opens, nothing is said. It was reported that way for First Aid. The one
    // line it did have was at info, and the log a bug report arrives with is
    // warnings only - so the report came back with no line about it at all.
    //
    // Only for a spell that reached a profession skill line, which is a
    // handful of casts rather than every one.
    auto slIt = owner_.spellToSkillLineRef().find(spellId);
    if (slIt == owner_.spellToSkillLineRef().end()) return 0;
    const uint32_t skillLine = slIt->second;

    auto catIt = owner_.skillLineCategoriesRef().find(skillLine);
    if (catIt == owner_.skillLineCategoriesRef().end()) {
        LOG_WARNING("Profession: spell ", spellId, " (", owner_.getSpellName(spellId),
                    ") is in skill line ", skillLine,
                    ", which SkillLine.dbc has no category for - not opening a window");
        return 0;
    }
    if (catIt->second != CAT_SECONDARY && catIt->second != CAT_PROFESSION) return 0;

    // Spell.dbc says which spells open this window: effect 47, on eighty-one of
    // them, and on nothing else. That is the answer rather than a sign of it.
    //
    // The heuristic below it - the opener is named after its skill line - was
    // what decided this before, and it is right for Cooking and wrong for
    // everything whose window is not named after its skill: Smelting opens
    // Mining, and Weaponsmith, Gnomish Engineer, Shadoweave Tailoring and the
    // rest of the specialisations open their parent's. Smelting was carried by
    // a hardcoded 2656 and the others were simply missing.
    //
    // The name test is kept as the second answer rather than replaced by the
    // first. A private server's Spell.dbc is edited, and a profession whose
    // opener has lost its effect there would otherwise stop opening - where
    // before this change it worked, because its name still matched.
    bool opensWindow = false;
    auto cacheIt = owner_.spellNameCacheRef().find(spellId);
    if (cacheIt != owner_.spellNameCacheRef().end()) {
        for (uint32_t effect : cacheIt->second.effectIds) {
            if (effect == SPELL_EFFECT_TRADE_SKILL) { opensWindow = true; break; }
        }
    }
    if (!opensWindow) {
        const std::string& spellName = owner_.getSpellName(spellId);
        auto nameIt = owner_.skillLineNamesRef().find(skillLine);
        opensWindow = !spellName.empty() &&
                      nameIt != owner_.skillLineNamesRef().end() &&
                      spellName == nameIt->second;
    }
    if (!opensWindow) {
        LOG_WARNING("Profession: spell ", spellId, " (", owner_.getSpellName(spellId),
                    ") is in skill line ", skillLine,
                    " but has no trade-skill effect and is not named after the line - "
                    "casting it normally");
        return 0;
    }

    // Only open a window that will actually list something: require at least
    // one known recipe (creates an item or consumes reagents) in this line.
    // This keeps Fishing falling through to a normal bobber cast.
    for (uint32_t known : knownSpells_) {
        if (known == spellId) continue;
        auto kslIt = owner_.spellToSkillLineRef().find(known);
        if (kslIt == owner_.spellToSkillLineRef().end() || kslIt->second != skillLine) continue;
        auto cacheIt = owner_.spellNameCacheRef().find(known);
        if (cacheIt == owner_.spellNameCacheRef().end()) continue;
        const auto& entry = cacheIt->second;
        bool hasReagents = false;
        for (const auto& reagent : entry.reagents) {
            if (reagent.itemId != 0) { hasReagents = true; break; }
        }
        if (entry.createdItemId != 0 || hasReagents) return skillLine;
    }
    // An opener with nothing behind it. Fishing reaches here every cast and is
    // meant to; a profession reaching it means the recipes are missing, which
    // is not the same fault and cannot be told apart from outside.
    LOG_WARNING("Profession: spell ", spellId, " (", owner_.getSpellName(spellId),
                ") opens skill line ", skillLine,
                " but no known spell in that line creates an item or takes "
                "reagents - not opening an empty window");
    return 0;
}

// Trainer spell categorisation lives in InventoryHandler::categorizeTrainerSpells,
// which handleTrainerList calls and which reads InventoryHandler's own populated
// currentTrainerList_. A stale duplicate here read GameHandler's copy of that
// list - a copy nothing ever writes - so it always categorised an empty list;
// it had no caller and its output (GameHandler's trainerTabs_) no reader, the
// UI going through getTrainerTabs, which delegates to InventoryHandler. Removed
// with the dead GameHandler members it was the only user of, so no one later
// wires up the empty-copy version. See [[dead_duplicate_state]].

const int32_t* SpellHandler::getSpellEffectBasePoints(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.effectBasePoints : nullptr;
}

float SpellHandler::getSpellDuration(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.durationSec : 0.0f;
}

const std::string& SpellHandler::getSpellName(uint32_t spellId) const {
    // Lazy-load Spell.dbc so callers don't need to know about initialization order.
    // Every other DBC-backed getter (getSpellDescription, getSpellSchoolMask, etc.)
    // already does this; these two were missed.
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.name : SPELL_EMPTY_STRING;
}

const std::string& SpellHandler::getSpellRank(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.rank : SPELL_EMPTY_STRING;
}

const std::string& SpellHandler::getSpellDescription(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.description : SPELL_EMPTY_STRING;
}

std::string SpellHandler::getEnchantName(uint32_t enchantId) const {
    if (enchantId == 0) return {};
    auto* am = owner_.services().assetManager;
    if (!am || !am->isInitialized()) return {};
    auto dbc = am->loadDBC("SpellItemEnchantment.dbc");
    if (!dbc || !dbc->isLoaded()) return {};
    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    const uint32_t nameField = pipeline::detectEnchantmentNameField(dbc.get(), sieL);
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        if (dbc->getUInt32(i, 0) == enchantId) {
            return dbc->getString(i, nameField);
        }
    }
    return {};
}

uint32_t SpellHandler::getEnchantGemItem(uint32_t enchantId) const {
    if (enchantId == 0) return 0;
    auto* am = owner_.services().assetManager;
    if (!am || !am->isInitialized()) return 0;
    auto dbc = am->loadDBC("SpellItemEnchantment.dbc");
    if (!dbc || !dbc->isLoaded()) return 0;
    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    const uint32_t gemField = pipeline::detectEnchantmentGemItemField(dbc.get(), sieL);
    if (gemField == 0) return 0;
    const int32_t row = dbc->findRecordById(enchantId);
    if (row < 0) return 0;
    return dbc->getUInt32(static_cast<uint32_t>(row), gemField);
}

uint8_t SpellHandler::getSpellDispelType(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.dispelType : 0;
}

void SpellHandler::handleSpellUpdateChainTargets(network::Packet& packet) {
    SpellUpdateChainTargetsData data;
    if (!SpellUpdateChainTargetsParser::parse(packet, data)) {
        packet.skipAll();
        return;
    }
    const uint64_t casterGuid = data.casterGuid;
    const uint32_t spellId = data.spellId;
    const std::vector<uint64_t>& targets = data.targets;
    auto entity = owner_.getEntityManager().getEntity(casterGuid);
    if (!entity || !entity->isUnit()) return;
    // 0x00741210: a channelled spell's (AttributesEx 0x4) channel object
    // becomes the first target where it is another.
    const uint16_t channelField = fieldIndex(UF::UNIT_FIELD_CHANNEL_OBJECT);
    uint64_t movedChannelObject = 0;
    if (!targets.empty() && targets[0] != 0 && channelField != 0xFFFF) {
        const uint64_t channelObject = static_cast<uint64_t>(entity->getField(channelField)) |
                                       (static_cast<uint64_t>(entity->getField(static_cast<uint16_t>(channelField + 1)))
                                        << 32);
        loadSpellNameCache();
        auto it = owner_.spellNameCacheRef().find(spellId);
        if (channelObject != targets[0] && it != owner_.spellNameCacheRef().end() && (it->second.attrEx & 0x4u)) {
            entity->setField(channelField, static_cast<uint32_t>(targets[0]));
            entity->setField(static_cast<uint16_t>(channelField + 1), static_cast<uint32_t>(targets[0] >> 32));
            movedChannelObject = targets[0];
        }
    }
    // Then the unit's hits (0x00724f50) and the spell's effects on it again
    // (0x0073eb50).
    if (auto* renderer = owner_.services().renderer) {
        if (auto* svs = renderer->getSpellVisualSystem()) svs->updateChainTargets(casterGuid, spellId, targets, movedChannelObject);
    }
}

bool SpellHandler::isSpellInterruptible(uint32_t spellId) const {
    if (spellId == 0) return true;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return true;
    return (it->second.attrEx & 0x00000010u) == 0;
}

bool SpellHandler::isSpellPassive(uint32_t spellId) const {
    if (spellId == 0) return false;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return false;
    return (it->second.attr & 0x00000040u) != 0;   // SPELL_ATTR0_PASSIVE
}

uint32_t SpellHandler::getSpellSchoolMask(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.schoolMask : 0;
}

uint32_t SpellHandler::getSpellTargetFlags(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.targetFlags : 0;
}

uint32_t SpellHandler::getSpellTargetAuraState(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.targetAuraState : 0;
}

uint32_t SpellHandler::resolveHighestKnownRank(uint32_t spellId) const {
    if (spellId == 0 || knownSpells_.count(spellId) > 0) return spellId;

    loadSpellNameCache();
    const auto& cache = owner_.spellNameCacheRef();

    // Adapt the spell name cache to the pure resolver in spell_classification.hpp.
    thread_local spellclass::SpellRankInfo scratch;
    auto lookup = [&cache](uint32_t id) -> const spellclass::SpellRankInfo* {
        auto it = cache.find(id);
        if (it == cache.end()) return nullptr;
        scratch.name = it->second.name;
        scratch.rank = it->second.rank;
        return &scratch;
    };

    const uint32_t best = spellclass::resolveHighestKnownRank(spellId, knownSpells_, lookup);
    if (best != spellId) {
        LOG_INFO("Superseded rank: casting ", best, " instead of ", spellId);
    }
    return best;
}

float SpellHandler::getSpellMaxRange(uint32_t spellId) const {
    if (spellId == 0) return -1.0f;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.maxRange : -1.0f;
}

bool SpellHandler::isSpellKnownToClient(uint32_t spellId) const {
    if (spellId == 0) return false;
    loadSpellNameCache();
    return owner_.spellNameCacheRef().count(spellId) != 0;
}

uint32_t SpellHandler::getSpellImplicitTargetA(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    return (it != owner_.spellNameCacheRef().end()) ? it->second.implicitTargetA : 0;
}

const std::string& SpellHandler::getSkillLineName(uint32_t skillLineId) const {
    // By skill line id, which is what every caller holds: the trade skill
    // window's own line and a trainer service's required skill. Reading the id
    // as a spell and following SkillLineAbility gave a real name for the wrong
    // skill - Tailoring is line 197, spell 197 is a Two-Handed Axes rank, so
    // the tailoring window was titled "Two-Handed Axes".
    owner_.loadSkillLineDbc();
    auto nameIt = owner_.skillLineNamesRef().find(skillLineId);
    return (nameIt != owner_.skillLineNamesRef().end()) ? nameIt->second : SPELL_EMPTY_STRING;
}

// ============================================================
// Skill DBC methods (moved from GameHandler)
// ============================================================

void SpellHandler::loadSkillLineDbc() {
    if (owner_.skillLineDbcLoadedRef()) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    owner_.skillLineDbcLoadedRef() = true;

    auto dbc = am->loadDBC("SkillLine.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("GameHandler: Could not load SkillLine.dbc");
        return;
    }

    const auto* slL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SkillLine") : nullptr;
    const uint32_t slIdField   = slL ? (*slL)["ID"]       : 0;
    const uint32_t slCatField  = slL ? (*slL)["Category"] : 1;
    const uint32_t slNameField = slL ? (*slL)["Name"]     : 3;
    // The icon column moved between expansions with the locale block in front
    // of it: 37 where there is a description and an alternate verb, 21 in the
    // vanilla-era file that has neither. Both were read off the files rather
    // than assumed, and the default is only reached with no layout at all.
    const uint32_t slIconField = slL ? (*slL)["SpellIcon"] : 37;
    // The description moves with the same locale block: 20 where the file
    // carries seventeen name columns, 12 where it carries nine. Read off the
    // files rather than assumed, like the icon beside it.
    const uint32_t slDescField = slL ? (*slL)["Description"] : 20;
    const uint32_t fieldCount  = dbc->getFieldCount();
    for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
        uint32_t id = dbc->getUInt32(i, slIdField);
        uint32_t category = dbc->getUInt32(i, slCatField);
        std::string name = dbc->getString(i, slNameField);
        if (id > 0 && !name.empty()) {
            owner_.skillLineNamesRef()[id] = name;
            owner_.skillLineCategoriesRef()[id] = category;
            // Guarded on the count, because a layout naming a column this file
            // does not have would otherwise read past the end of the row and
            // give every tab the same nonsense icon rather than none.
            if (slIconField < fieldCount) {
                if (uint32_t icon = dbc->getUInt32(i, slIconField); icon > 0) {
                    owner_.skillLineIconsRef()[id] = icon;
                }
            }
            if (slDescField < fieldCount) {
                std::string desc = dbc->getString(i, slDescField);
                if (!desc.empty()) owner_.skillLineDescriptionsRef()[id] = std::move(desc);
            }
        }
    }
    LOG_INFO("GameHandler: Loaded ", owner_.skillLineNamesRef().size(), " skill line names");

    // The eight headings the skills tab groups under, read from the file that
    // names them rather than written out here - and with the order the file
    // itself gives, which is the order the original tab draws them in:
    // Attributes, Class Skills, Professions, Secondary Skills, Weapon Skills,
    // Armor Proficiencies, Languages, Not Displayed.
    auto catDbc = am->loadDBC("SkillLineCategory.dbc");
    if (!catDbc || !catDbc->isLoaded()) {
        LOG_WARNING("GameHandler: Could not load SkillLineCategory.dbc");
        return;
    }
    // Nineteen fields: the id, seventeen locale columns, and the sort index
    // last. Taken from the end rather than by number so a shorter localised
    // build still finds it.
    const uint32_t catFields = catDbc->getFieldCount();
    for (uint32_t i = 0; i < catDbc->getRecordCount(); i++) {
        const uint32_t id = catDbc->getUInt32(i, 0);
        if (id == 0) continue;
        std::string name = catDbc->getString(i, 1);
        if (!name.empty()) owner_.skillCategoryNamesRef()[id] = std::move(name);
        if (catFields >= 2) {
            owner_.skillCategorySortRef()[id] = catDbc->getUInt32(i, catFields - 1);
        }
    }
    LOG_INFO("GameHandler: Loaded ", owner_.skillCategoryNamesRef().size(),
             " skill categories");
}

void SpellHandler::extractSkillFields(const FlatFieldMap& fields) {
    loadSkillLineDbc();

    const uint16_t PLAYER_SKILL_INFO_START = fieldIndex(UF::PLAYER_SKILL_INFO_START);
    static constexpr int MAX_SKILL_SLOTS = 128;

    std::unordered_map<uint32_t, PlayerSkill> newSkills;

    for (int slot = 0; slot < MAX_SKILL_SLOTS; slot++) {
        uint16_t baseField = PLAYER_SKILL_INFO_START + slot * 3;

        auto idIt = fields.find(baseField);
        if (idIt == fields.end()) continue;

        uint32_t raw0 = idIt->second;
        uint16_t skillId = raw0 & 0xFFFF;
        if (skillId == 0) continue;

        auto valIt = fields.find(baseField + 1);
        if (valIt == fields.end()) continue;

        uint32_t raw1 = valIt->second;
        uint16_t value = raw1 & 0xFFFF;
        uint16_t maxValue = (raw1 >> 16) & 0xFFFF;

        uint16_t bonusTemp = 0;
        uint16_t bonusPerm = 0;
        auto bonusIt = fields.find(static_cast<uint16_t>(baseField + 2));
        if (bonusIt != fields.end()) {
            bonusTemp = bonusIt->second & 0xFFFF;
            bonusPerm = (bonusIt->second >> 16) & 0xFFFF;
        }

        PlayerSkill skill;
        skill.skillId = skillId;
        skill.value = value;
        skill.maxValue = maxValue;
        skill.bonusTemp = bonusTemp;
        skill.bonusPerm = bonusPerm;
        skill.step = static_cast<uint16_t>(raw0 >> 16);
        newSkills[skillId] = skill;
    }

    for (const auto& [skillId, skill] : newSkills) {
        if (skill.value == 0) continue;
        auto oldIt = owner_.playerSkillsRef().find(skillId);
        if (oldIt != owner_.playerSkillsRef().end() && skill.value > oldIt->second.value) {
            auto catIt = owner_.skillLineCategoriesRef().find(skillId);
            if (catIt != owner_.skillLineCategoriesRef().end()) {
                uint32_t category = catIt->second;
                if (category == 5 || category == 10 || category == 12) {
                    continue;
                }
            }

            const std::string& name = owner_.getSkillName(skillId);
            std::string skillName = name.empty() ? ("Skill #" + std::to_string(skillId)) : name;
            // CHAT_MSG_SKILL, which the interface colours blue, as the server
            // sends it - not a system line, which is yellow. The words are the
            // interface's SKILL_RANK_UP, so a German client says "Eure
            // Fertigkeit '...' hat sich auf ... erhöht."; enUS only for want
            // of one.
            std::string format;
            if (auto* addons = core::Application::getInstance().getAddonManager())
                if (auto* lua = addons->getLuaEngine()) format = lua->globalText("SKILL_RANK_UP");
            if (format.empty()) format = "Your skill in %s has increased to %d.";
            owner_.addLocalChatLine(ChatType::SKILL,
                                    formatSkillRankUp(format, skillName, skill.value));
        }
    }

    bool skillsChanged = (newSkills.size() != owner_.playerSkillsRef().size());
    if (!skillsChanged) {
        for (const auto& [id, sk] : newSkills) {
            auto it = owner_.playerSkillsRef().find(id);
            if (it == owner_.playerSkillsRef().end() || it->second.value != sk.value) {
                skillsChanged = true;
                break;
            }
        }
    }
    owner_.playerSkillsRef() = std::move(newSkills);
    if (skillsChanged)
        owner_.fireAddonEvent("SKILL_LINES_CHANGED", {});
}

void SpellHandler::extractExploredZoneFields(const FlatFieldMap& fields) {
    const size_t zoneCount = owner_.getPacketParsers()
        ? static_cast<size_t>(owner_.getPacketParsers()->exploredZonesCount())
        : GameHandler::PLAYER_EXPLORED_ZONES_COUNT;

    if (owner_.playerExploredZonesRef().size() != GameHandler::PLAYER_EXPLORED_ZONES_COUNT) {
        owner_.playerExploredZonesRef().assign(GameHandler::PLAYER_EXPLORED_ZONES_COUNT, 0u);
    }

    bool foundAny = false;
    for (size_t i = 0; i < zoneCount; i++) {
        const uint16_t fieldIdx = static_cast<uint16_t>(fieldIndex(UF::PLAYER_EXPLORED_ZONES_START) + i);
        auto it = fields.find(fieldIdx);
        if (it == fields.end()) continue;
        owner_.playerExploredZonesRef()[i] = it->second;
        foundAny = true;
    }
    for (size_t i = zoneCount; i < GameHandler::PLAYER_EXPLORED_ZONES_COUNT; i++) {
        owner_.playerExploredZonesRef()[i] = 0u;
    }

    if (foundAny) {
        owner_.hasPlayerExploredZonesRef() = true;
    }
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void SpellHandler::handleCastResult(network::Packet& packet) {
    uint32_t castResultSpellId = 0;
    uint8_t  castResult        = 0;
    uint32_t castResultMiscArg = 0;
    uint32_t castResultMiscArg2 = 0;
    if (owner_.getPacketParsers()->parseCastResult(packet, castResultSpellId, castResult,
                                                   castResultMiscArg, castResultMiscArg2)) {
        LOG_DEBUG("SMSG_CAST_RESULT: spellId=", castResultSpellId, " result=", static_cast<int>(castResult));
        if (castResult != 0) {
            if (spellclass::isFishingCast(castResultSpellId)) {
                const auto& movement = owner_.movementInfoRef();
                LOG_WARNING("Fishing cast failed: spell=", castResultSpellId,
                            " result=", static_cast<int>(castResult),
                            " pos=(", movement.x, ",", movement.y, ",", movement.z, ")",
                            " facing=", movement.orientation,
                            " selectedTarget=0x", std::hex, owner_.getTargetGuid(), std::dec);
            }
            const uint64_t gatherGoGuid = owner_.lastInteractedGoGuidRef();
            const bool gatherCast = gatherGoGuid != 0 && isGatherSpellId(castResultSpellId);
            casting_ = false; castIsChannel_ = false; currentCastSpellId_ = 0; castTimeRemaining_ = 0.0f;
            owner_.lastInteractedGoGuidRef() = 0;
            owner_.pendingGameObjectInteractGuidRef() = 0;
            craftQueueSpellId_ = 0; craftQueueRemaining_ = 0;
            int playerPowerType = -1;
            if (auto pe = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
                if (auto pu = std::dynamic_pointer_cast<Unit>(pe))
                    playerPowerType = static_cast<int>(pu->getPowerType());
            }
            // Totem failures name tool item ids; request their info so a retry
            // of the craft can show the tool's name even if it wasn't cached.
            if (castResult == kCastResultTotems) {
                if (castResultMiscArg != 0) owner_.ensureItemInfo(castResultMiscArg);
                if (castResultMiscArg2 != 0) owner_.ensureItemInfo(castResultMiscArg2);
            }
            std::string errMsg = castFailureMessage(owner_, castResultSpellId,
                                                     castResult, playerPowerType,
                                                     castResultMiscArg, castResultMiscArg2);
            if (gatherCast && shouldDespawnGatherTarget(castResult)) {
                owner_.despawnGameObjectLocally(gatherGoGuid);
            }
            showPlayerCastFailure(castResultSpellId, castResult, errMsg);
            if (owner_.spellCastFailedCallbackRef()) owner_.spellCastFailedCallbackRef()(castResultSpellId);
                owner_.fireAddonEvent("UNIT_SPELLCAST_FAILED", spellcastArgs("player", castResultSpellId));
                owner_.fireAddonEvent("UNIT_SPELLCAST_STOP",   spellcastArgs("player", castResultSpellId));
            // The second of the two chat writes. See handleCastFailed.
        }
    }
}

void SpellHandler::handleSpellFailedOther(network::Packet& packet) {
    const bool tbcLike2 = isPreWotlk();
    uint64_t failOtherGuid = tbcLike2
        ? (packet.hasRemaining(8) ? packet.readUInt64() : 0)
        : packet.readPackedGuid();
    if (failOtherGuid != 0 && failOtherGuid != owner_.getPlayerGuid()) {
        // Which spell it was, before the state that says so is dropped: the
        // cast bar matches the id on a stop against the one it started with,
        // and there is nowhere else to read it from once this is erased.
        uint32_t failedSpellId = 0;
        if (const auto* st = getUnitCastState(failOtherGuid)) failedSpellId = st->spellId;
        unitCastStates_.erase(failOtherGuid);
        noteUnitCastEnd(failOtherGuid, false);
        if (owner_.addonEventCallbackRef()) {
            std::string unitId;
            if (failOtherGuid == owner_.getTargetGuid())     unitId = "target";
            else if (failOtherGuid == owner_.focusGuidRef()) unitId = "focus";
            if (!unitId.empty()) {
                owner_.fireAddonEvent("UNIT_SPELLCAST_FAILED", spellcastArgs(unitId, failedSpellId));
                owner_.fireAddonEvent("UNIT_SPELLCAST_STOP",   spellcastArgs(unitId, failedSpellId));
            }
        }
    }
    packet.skipAll();
}

void SpellHandler::handleClearCooldown(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        // 0x00804010: the entries for it go (0x00802970) - the pet's, else
        // the player's.
        const uint64_t guid = packet.hasRemaining(8) ? packet.readUInt64() : 0;
        (guid != 0 && guid == owner_.getPetGuid() ? petHistory_ : playerHistory_)
            .event(spellId, historyNowMs(), true);
        playerCooldownsChanged();
    }
}

void SpellHandler::handleModifyCooldown(network::Packet& packet) {
    // uint32 spellId, uint64 playerGuid, int32 cooldownMod - the guid was not
    // read, so the change in milliseconds came from its low half and the
    // cooldown moved by whatever that happened to be.
    if (packet.hasRemaining(16)) {
        uint32_t spellId = packet.readUInt32();
        const uint64_t guid = packet.readUInt64();
        int32_t  diffMs  = static_cast<int32_t>(packet.readUInt32());
        // 0x00804010: the starts move (0x00802a10) - the pet's, else the
        // player's.
        (guid != 0 && guid == owner_.getPetGuid() ? petHistory_ : playerHistory_).modify(spellId, diffMs);
        playerCooldownsChanged();
    }
}

void SpellHandler::handlePlaySpellVisual(network::Packet& packet) {
    if (!packet.hasRemaining(12)) return;
    uint64_t casterGuid = packet.readUInt64();
    uint32_t visualId   = packet.readUInt32();
    if (visualId == 0) return;
    auto* renderer = owner_.services().renderer;
    if (!renderer) return;
    glm::vec3 spawnPos;
    if (casterGuid == owner_.getPlayerGuid()) {
        spawnPos = renderer->getCharacterPosition();
    } else {
        auto entity = owner_.getEntityManager().getEntity(casterGuid);
        if (!entity) return;
        glm::vec3 canonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
        spawnPos = core::coords::canonicalToRender(canonical);
    }
    // The packet names a SpellVisualKit, played on the unit as type 1
    // (0x008006c0).
    if (auto* sv = renderer->getSpellVisualSystem())
        sv->playKit(visualId, rendering::spell_kit::KitType::Cast, spawnPos,
                    owner_.resolveUnitRenderInstance(casterGuid));
}

void SpellHandler::handleSpellModifier(network::Packet& packet, bool isFlat) {
    auto& modMap = isFlat ? owner_.spellFlatModsRef() : owner_.spellPctModsRef();
    while (packet.hasRemaining(6)) {
        uint8_t groupIndex = packet.readUInt8();
        uint8_t modOpRaw   = packet.readUInt8();
        int32_t value      = static_cast<int32_t>(packet.readUInt32());
        // The group is a SpellFamilyFlags bit, 0 to 95 (0x007fdc60).
        if (groupIndex >= spell_mods::kFamilyFlagBits || modOpRaw >= GameHandler::SPELL_MOD_OP_COUNT) continue;
        GameHandler::SpellModKey key{ .op = static_cast<GameHandler::SpellModOp>(modOpRaw), .group = groupIndex };
        modMap[key] = value;
    }
    packet.skipAll();
}

void SpellHandler::handleSpellDelayed(network::Packet& packet) {
    const bool spellDelayTbcLike = isPreWotlk();
    if (!packet.hasRemaining(spellDelayTbcLike ? 8u : 1u) ) return;
    uint64_t caster = spellDelayTbcLike
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t delayMs = packet.readUInt32();
    if (delayMs == 0) return;
    float delaySec = delayMs / 1000.0f;
    if (caster == owner_.getPlayerGuid()) {
        if (casting_) {
            castTimeRemaining_ += delaySec;
            castTimeTotal_     += delaySec;
        }
    } else {
        auto it = unitCastStates_.find(caster);
        if (it != unitCastStates_.end() && it->second.casting) {
            it->second.timeRemaining += delaySec;
            it->second.timeTotal     += delaySec;
        }
    }
    // The cast bar redraws itself from this: it is already on screen and its
    // end moved, so without the event it counts down to the wrong moment.
    const std::string delayedUnit = owner_.guidToUnitId(caster);
    if (!delayedUnit.empty()) {
        owner_.fireAddonEvent("UNIT_SPELLCAST_DELAYED", {delayedUnit});
    }
}

// ============================================================
// Extracted opcode handlers (from registerOpcodeHandlers)
// ============================================================

void SpellHandler::handleSpellLogMiss(network::Packet& packet) {
    // All expansions: uint32 spellId first.
    // WotLK/Classic: spellId(4) + packed_guid caster + uint8 unk + uint32 count
    //                 + count × (packed_guid victim + uint8 missInfo)
    // TBC:            spellId(4) + uint64 caster + uint8 unk + uint32 count
    //                 + count × (uint64 victim + uint8 missInfo)
    // All expansions append uint32 reflectSpellId + uint8 reflectResult when
    // missInfo==REFLECT (11).
    if (isActiveExpansion("wotlk")) {
        // 0x006d3090: full guids and no reflect tail; a miss under the debug
        // byte carries two floats and is not logged.
        SpellLogMissData d;
        if (!SpellLogMissParser::parse(packet, d)) {
            packet.skipAll();
            return;
        }
        if (d.debug) return;
        for (const auto& miss : d.misses) {
            // A damage shield's miss (0x00751b80 with that flag): this packet
            // is what a shield that failed to strike sends.
            owner_.combatLogFeed().spellMissed(d.casterGuid, miss.victimGuid, d.spellId, miss.missInfo, true, false);
            const CombatTextEntry::Type ct = combatTextTypeFromSpellMissInfo(miss.missInfo);
            if (d.casterGuid == owner_.getPlayerGuid())
                owner_.addCombatText(ct, 0, d.spellId, true, 0, d.casterGuid, miss.victimGuid);
            else if (miss.victimGuid == owner_.getPlayerGuid())
                owner_.addCombatText(ct, 0, d.spellId, false, 0, d.casterGuid, miss.victimGuid);
        }
        return;
    }
    const bool spellMissUsesFullGuid = isActiveExpansion("tbc");
    auto readSpellMissGuid = [&]() -> uint64_t {
        if (spellMissUsesFullGuid)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    // spellId prefix present in all expansions
    if (!packet.hasRemaining(4)) return;
    uint32_t spellId = packet.readUInt32();
    if (!packet.hasRemaining(spellMissUsesFullGuid ? 8u : 1u)
        || (!spellMissUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = readSpellMissGuid();
    if (!packet.hasRemaining(5)) return;
    /*uint8_t unk =*/ packet.readUInt8();
    const uint32_t rawCount = packet.readUInt32();
    if (rawCount > 128) {
        LOG_WARNING("SMSG_SPELLLOGMISS: miss count capped (requested=", rawCount, ")");
    }
    const uint32_t storedLimit = std::min<uint32_t>(rawCount, 128u);

    struct SpellMissLogEntry {
        uint64_t victimGuid = 0;
        uint8_t missInfo = 0;
        uint32_t reflectSpellId = 0;  // Only valid when missInfo==REFLECT
    };
    std::vector<SpellMissLogEntry> parsedMisses;
    parsedMisses.reserve(storedLimit);

    bool truncated = false;
    for (uint32_t i = 0; i < rawCount; ++i) {
        if (!packet.hasRemaining(spellMissUsesFullGuid ? 9u : 2u)
            || (!spellMissUsesFullGuid && !packet.hasFullPackedGuid())) {
            truncated = true;
            return;
        }
        const uint64_t victimGuid = readSpellMissGuid();
        if (!packet.hasRemaining(1)) {
            truncated = true;
            return;
        }
        const uint8_t missInfo = packet.readUInt8();
        // REFLECT: extra uint32 reflectSpellId + uint8 reflectResult
        uint32_t reflectSpellId = 0;
        if (missInfo == SpellMissInfo::REFLECT) {
            if (packet.hasRemaining(5)) {
                reflectSpellId = packet.readUInt32();
                /*uint8_t reflectResult =*/ packet.readUInt8();
            } else {
                truncated = true;
                return;
            }
        }
        if (i < storedLimit) {
            parsedMisses.push_back({.victimGuid = victimGuid, .missInfo = missInfo, .reflectSpellId = reflectSpellId});
        }
    }

    if (truncated) {
        packet.skipAll();
        return;
    }

    for (const auto& miss : parsedMisses) {
        const uint64_t victimGuid = miss.victimGuid;
        const uint8_t missInfo = miss.missInfo;
        CombatTextEntry::Type ct = combatTextTypeFromSpellMissInfo(missInfo);
        // For REFLECT, use the reflected spell ID so combat text shows the spell name
        uint32_t combatSpellId = (ct == CombatTextEntry::REFLECT && miss.reflectSpellId != 0)
                                 ? miss.reflectSpellId : spellId;
        if (casterGuid == owner_.getPlayerGuid()) {
            // We cast a spell and it missed the target
            owner_.addCombatText(ct, 0, combatSpellId, true, 0, casterGuid, victimGuid);
        } else if (victimGuid == owner_.getPlayerGuid()) {
            // Enemy spell missed us (we dodged/parried/blocked/resisted/etc.)
            owner_.addCombatText(ct, 0, combatSpellId, false, 0, casterGuid, victimGuid);
        }
    }
}

void SpellHandler::handleSpellFailure(network::Packet& packet) {
    // WotLK: packed_guid + uint8 castCount + uint32 spellId + uint8 failReason
    // TBC:   full uint64 + uint8 castCount + uint32 spellId + uint8 failReason
    // Classic: full uint64 + uint32 spellId + uint8 failReason  (NO castCount)
    const bool isClassic = isClassicLikeExpansion();
    const bool isTbc     = isActiveExpansion("tbc");
    uint64_t failGuid = (isClassic || isTbc)
        ? (packet.hasRemaining(8) ? packet.readUInt64() : 0)
        : packet.readPackedGuid();
    // Classic omits the castCount byte; TBC and WotLK include it
    const size_t remainingFields = isClassic ? 5u : 6u;  // spellId(4)+reason(1) [+castCount(1)]
    if (packet.hasRemaining(remainingFields)) {
        if (!isClassic) /*uint8_t castCount =*/ packet.readUInt8();
        uint32_t failSpellId = packet.readUInt32();
        uint8_t rawFailReason = packet.readUInt8();
        // Classic result enum starts at 0=AFFECTING_COMBAT; shift +1 for WotLK table
        uint8_t failReason = isClassic ? static_cast<uint8_t>(rawFailReason + 1) : rawFailReason;
        // Nothing said, for the player's own failure either: 0x00809c70 only
        // ends the cast (0x007fecc0), and the reason, where there is one,
        // came with SMSG_CAST_FAILED. This put "Spell: reason" on screen and
        // in chat a second time.
        (void)failSpellId;
        (void)failReason;
    }
    // Fire UNIT_SPELLCAST_INTERRUPTED for Lua addons
    if (owner_.addonEventCallbackRef()) {
        auto unitId = (failGuid == 0) ? std::string("player") : owner_.guidToUnitId(failGuid);
        if (!unitId.empty()) {
            uint32_t spellId = 0;
            if (const auto* st = getUnitCastState(failGuid)) spellId = st->spellId;
            owner_.fireAddonEvent("UNIT_SPELLCAST_INTERRUPTED", spellcastArgs(unitId, spellId));
            owner_.fireAddonEvent("UNIT_SPELLCAST_STOP", spellcastArgs(unitId, spellId));
        }
    }
    if (failGuid == owner_.getPlayerGuid() || failGuid == 0) {
        // Player's own cast failed - clear gather-node loot target so the
        // next timed cast doesn't try to loot a stale interrupted gather node.
        casting_ = false; castIsChannel_ = false; currentCastSpellId_ = 0;
        owner_.lastInteractedGoGuidRef() = 0;
        craftQueueSpellId_ = 0;
        craftQueueRemaining_ = 0;
        // Remove lingering precast visual effects
        if (auto* renderer = owner_.services().renderer) {
            if (auto* svs = renderer->getSpellVisualSystem())
                svs->cancelAllPrecastVisuals();
        }
        if (auto* ac = owner_.services().audioCoordinator) {
            if (auto* ssm = ac->getSpellSoundManager()) {
                ssm->stopPrecast();
            }
        }
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(owner_.getPlayerGuid(), false, false, SpellCastType::OMNI);
        }
    } else {
        // Another unit's cast failed - clear their tracked cast bar
        unitCastStates_.erase(failGuid);
        noteUnitCastEnd(failGuid, false);
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(failGuid, false, false, SpellCastType::OMNI);
        }
    }
}

void SpellHandler::handleItemCooldown(network::Packet& packet) {
    // 0x00807060: the item's guid and a spell - the item, there, and the
    // spell put the spell in the player's history as cast from it, thirty
    // seconds from now.
    if (packet.hasRemaining(12)) {
        const uint64_t itemGuid = packet.readUInt64();
        const uint32_t spellId = packet.readUInt32();
        loadSpellNameCache();
        const auto item = owner_.onlineItemsRef().find(itemGuid);
        if (item != owner_.onlineItemsRef().end() && owner_.spellNameCacheRef().count(spellId) != 0) {
            spell_history::Entry h;
            h.spellId = spellId;
            h.itemId = item->second.entry;
            h.start = historyNowMs();
            h.duration = 30000;
            playerHistory_.add(h);
        }
        playerCooldownsChanged();
    }
    // The bags redraw their cooldown swirls from this and nothing else, so a
    // potion or trinket put on cooldown showed none. This message is the item
    // cooldown, so it is the one place the event certainly belongs.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("BAG_UPDATE_COOLDOWN", {});
    }
}

void SpellHandler::handleDispelFailed(network::Packet& packet) {
    // WotLK:       uint32 dispelSpellId + packed_guid caster + packed_guid victim
    //              [+ count × uint32 failedSpellId]
    // Classic:     uint32 dispelSpellId + packed_guid caster + packed_guid victim
    //              [+ count × uint32 failedSpellId]
    // TBC:         uint64 caster + uint64 victim + uint32 spellId
    //              [+ count × uint32 failedSpellId]
    // WotLK:       as TBC (0x00750ea0).
    if (isActiveExpansion("wotlk")) {
        DispelFailedData d;
        if (DispelFailedParser::parse(packet, d))
            owner_.combatLogFeed().dispelFailed(d.casterGuid, d.victimGuid, d.spellId, d.failedSpells);
        packet.skipAll();
        return;
    }
    const bool dispelUsesFullGuid = isActiveExpansion("tbc");
    uint32_t dispelSpellId = 0;
    uint64_t dispelCasterGuid = 0;
    if (dispelUsesFullGuid) {
        if (!packet.hasRemaining(20)) return;
        dispelCasterGuid = packet.readUInt64();
        /*uint64_t victim =*/ packet.readUInt64();
        dispelSpellId = packet.readUInt32();
    } else {
        if (!packet.hasRemaining(4)) return;
        dispelSpellId = packet.readUInt32();
        if (!packet.hasFullPackedGuid()) {
            packet.skipAll(); return;
        }
        dispelCasterGuid = packet.readPackedGuid();
        if (!packet.hasFullPackedGuid()) {
            packet.skipAll(); return;
        }
        /*uint64_t victim =*/ packet.readPackedGuid();
    }
    // No chat line: 0x00750ea0 writes this to the combat log (event 0x27)
    // and nowhere else. This printed "X failed to dispel." in English.
    (void)dispelSpellId;
    (void)dispelCasterGuid;
}

void SpellHandler::handleTotemCreated(network::Packet& packet) {
    // 0x00526530 (0x413): slot, the totem's guid as a full uint64 on every
    // expansion - TrinityCore and AzerothCore write GetGUID() whole - then
    // duration and spell. Read as a packed guid on 3.3.5, the mask byte and
    // the guid's bytes became the duration and the spell, and the totem bar
    // showed noise. A zero guid changes nothing.
    TotemCreatedData totem;
    if (!TotemCreatedParser::parse(packet, totem)) return;
    if (totem.guid == 0) return;
    const uint8_t slot = totem.slot;
    const uint32_t duration = totem.durationMs;
    const uint32_t spellId = totem.spellId;
    LOG_DEBUG("SMSG_TOTEM_CREATED: slot=", static_cast<int>(slot),
              " spellId=", spellId, " duration=", duration, "ms");
    if (slot < GameHandler::NUM_TOTEM_SLOTS) {
        owner_.activeTotemSlotsRef()[slot].spellId    = spellId;
        owner_.activeTotemSlotsRef()[slot].durationMs = duration;
        owner_.activeTotemSlotsRef()[slot].guid       = totem.guid;
        owner_.activeTotemSlotsRef()[slot].placedAt   = std::chrono::steady_clock::now();
        // The totem bar draws each slot from its start and duration, and
        // refreshes on this alone - without it a totem is placed and the bar
        // goes on showing whatever was there before.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_TOTEM_UPDATE",
                                           {std::to_string(slot + 1)});
        }
    }
}

void SpellHandler::handlePeriodicAuraLog(network::Packet& packet) {
    // Classic, TBC, and WotLK all serialize victim and caster as packed GUIDs.
    if (!packet.hasFullPackedGuid()) return;
    uint64_t victimGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) return;
    uint64_t casterGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;
    uint32_t spellId = packet.readUInt32();
    uint32_t count   = packet.readUInt32();
    bool isPlayerVictim = (victimGuid == owner_.getPlayerGuid());
    bool isPlayerCaster = (casterGuid == owner_.getPlayerGuid());
    // 0x007531f0 logs every tick of a unit it knows; the floating text is
    // only the player's.
    const auto victimEntity = owner_.getEntityManager().getEntity(victimGuid);
    const bool logIt = isActiveExpansion("wotlk") && victimEntity && victimEntity->isUnit();
    auto& feed = owner_.combatLogFeed();
    const bool ours = isPlayerVictim || isPlayerCaster;
    if (!ours && !logIt) {
        packet.skipAll();
        return;
    }
    // SpellPeriodicAuraLogInfo serializes AuraType as uint32 on the wire.
    // Reading one byte leaves three zero bytes in front of the amount and
    // turns ordinary poison ticks into corrupt multi-byte damage values.
    if (count > 64) {
        LOG_WARNING("SMSG_PERIODICAURALOG: unreasonable effect count ", count);
        return;
    }
    for (uint32_t i = 0; i < count && packet.hasRemaining(4); ++i) {
        uint32_t auraType = packet.readUInt32();
        if (auraType == 3 || auraType == 89) {
            // Classic/TBC: damage(4)+school(4)+absorbed(4)+resisted(4)  = 16 bytes
            // WotLK 3.3.5a: damage(4)+overkill(4)+school(4)+absorbed(4)+resisted(4)+isCrit(1) = 21 bytes
            const bool periodicWotlk = isActiveExpansion("wotlk");
            const size_t dotSz = periodicWotlk ? 21u : 16u;
            if (!packet.hasRemaining(dotSz)) break;
            uint32_t dmg      = packet.readUInt32();
            uint32_t overkill = periodicWotlk ? packet.readUInt32() : 0;
            uint32_t school   = packet.readUInt32();
            uint32_t abs      = packet.readUInt32();
            uint32_t res      = packet.readUInt32();
            bool dotCrit = false;
            if (periodicWotlk) dotCrit = (packet.readUInt8() != 0);
            if (logIt)
                feed.spellDamage(casterGuid, victimGuid, spellId, static_cast<int32_t>(dmg),
                                 static_cast<int32_t>(overkill), school, static_cast<int32_t>(abs),
                                 static_cast<int32_t>(res), 0, dotCrit ? 2u : 0u, true);
            if (!ours) continue;
            if (dmg > 0)
                owner_.addCombatText(dotCrit ? CombatTextEntry::CRIT_DAMAGE : CombatTextEntry::PERIODIC_DAMAGE,
                              static_cast<int32_t>(dmg),
                              spellId, isPlayerCaster, 0, casterGuid, victimGuid);
            if (abs > 0)
                owner_.addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(abs),
                              spellId, isPlayerCaster, 0, casterGuid, victimGuid);
            if (res > 0)
                owner_.addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(res),
                              spellId, isPlayerCaster, 0, casterGuid, victimGuid);
        } else if (auraType == 8 || auraType == 20) {
            // Classic/TBC: heal(4)
            // WotLK: heal(4)+overheal(4)+absorbed(4)+isCrit(1)
            const bool healWotlk = isActiveExpansion("wotlk");
            const size_t hotSz = healWotlk ? 13u : 4u;
            if (!packet.hasRemaining(hotSz)) break;
            uint32_t heal    = packet.readUInt32();
            uint32_t hotAbs  = 0;
            uint32_t overheal = 0;
            bool hotCrit = false;
            if (healWotlk) {
                overheal = packet.readUInt32();
                hotAbs = packet.readUInt32();
                hotCrit = (packet.readUInt8() != 0);
            }
            if (logIt)
                feed.spellHeal(casterGuid, victimGuid, spellId, static_cast<int32_t>(heal),
                               static_cast<int32_t>(overheal), static_cast<int32_t>(hotAbs), hotCrit, true);
            if (!ours) continue;
            owner_.addCombatText(hotCrit ? CombatTextEntry::CRIT_HEAL : CombatTextEntry::PERIODIC_HEAL,
                          static_cast<int32_t>(heal),
                          spellId, isPlayerCaster, 0, casterGuid, victimGuid);
            if (hotAbs > 0)
                owner_.addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(hotAbs),
                              spellId, isPlayerCaster, 0, casterGuid, victimGuid);
        } else if (auraType == 21 || auraType == 24) {
            // OBS_MOD_POWER / PERIODIC_ENERGIZE: miscValue(powerType) + amount
            // Common in WotLK: Replenishment, Mana Spring Totem, Divine Plea, etc.
            if (!packet.hasRemaining(8)) break;
            uint8_t periodicPowerType = static_cast<uint8_t>(packet.readUInt32());
            uint32_t amount = packet.readUInt32();
            if (amount != 0) owner_.addPredictedPower(victimGuid, periodicPowerType, static_cast<int32_t>(amount));  // 0x00752040
            if (logIt)
                feed.spellEnergize(casterGuid, victimGuid, spellId, periodicPowerType, static_cast<int32_t>(amount),
                                   true);
            if ((isPlayerVictim || isPlayerCaster) && amount > 0)
                owner_.addCombatText(CombatTextEntry::ENERGIZE, static_cast<int32_t>(amount),
                              spellId, isPlayerCaster, periodicPowerType, casterGuid, victimGuid);
        } else if (auraType == 64) {
            // PERIODIC_MANA_LEECH: miscValue(powerType) + amount + float multiplier
            if (!packet.hasRemaining(12)) break;
            uint8_t powerType = static_cast<uint8_t>(packet.readUInt32());
            uint32_t amount = packet.readUInt32();
            float multiplier = packet.readFloat();
            // 0x00752100: taken from the victim's predicted power, happiness aside.
            if (amount != 0 && powerType != 4) owner_.addPredictedPower(victimGuid, powerType, -static_cast<int32_t>(amount));
            if (logIt) feed.spellDrain(casterGuid, victimGuid, spellId, powerType, amount, multiplier, true);
            if (isPlayerVictim && amount > 0)
                owner_.addCombatText(CombatTextEntry::POWER_DRAIN, static_cast<int32_t>(amount),
                              spellId, false, powerType, casterGuid, victimGuid);
            if (isPlayerCaster && amount > 0 && multiplier > 0.0f && std::isfinite(multiplier)) {
                const uint32_t gainedAmount = static_cast<uint32_t>(
                    std::lround(static_cast<double>(amount) * static_cast<double>(multiplier)));
                if (gainedAmount > 0) {
                    owner_.addCombatText(CombatTextEntry::ENERGIZE, static_cast<int32_t>(gainedAmount),
                                  spellId, true, powerType, casterGuid, casterGuid);
                }
            }
        } else {
            // Unknown/untracked aura type - stop parsing this event safely
            packet.skipAll();
            break;
        }
    }
    packet.skipAll();
}

void SpellHandler::handleSpellEnergizeLog(network::Packet& packet) {
    // WotLK: packed_guid victim + packed_guid caster + uint32 spellId + uint32 powerType + int32 amount
    // TBC: full uint64 victim + uint64 caster + uint32 spellId + uint32 powerType + int32 amount
    // Classic/Vanilla: packed_guid (same as WotLK). PowerType is uint32 in every expansion.
    const bool energizeTbc = isActiveExpansion("tbc");
    auto readEnergizeGuid = [&]() -> uint64_t {
        if (energizeTbc)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    if (!packet.hasRemaining(energizeTbc ? 8u : 1u)
        || (!energizeTbc && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = readEnergizeGuid();
    if (!packet.hasRemaining(energizeTbc ? 8u : 1u)
        || (!energizeTbc && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = readEnergizeGuid();
    // spellId(4) + powerType(4) + amount(4) = 12. PowerType is a uint32 on the wire in
    // every expansion (Vanilla/TBC/WotLK); reading it as a uint8 left 3 bytes in front of
    // amount, turning a small power gain (e.g. 10 = 0x0000000A after powerType 0x00000001)
    // into 0x0A000000 = 167772160 floating combat text.
    if (!packet.hasRemaining(12)) {
        packet.skipAll(); return;
    }
    uint32_t spellId       = packet.readUInt32();
    uint8_t  energizePowerType = static_cast<uint8_t>(packet.readUInt32());
    int32_t  amount        = static_cast<int32_t>(packet.readUInt32());
    bool isPlayerVictim = (victimGuid == owner_.getPlayerGuid());
    bool isPlayerCaster = (casterGuid == owner_.getPlayerGuid());
    // 0x00752040: the victim's predicted power, by the amount.
    if (amount != 0) owner_.addPredictedPower(victimGuid, energizePowerType, amount);
    if (isActiveExpansion("wotlk"))
        owner_.combatLogFeed().spellEnergize(casterGuid, victimGuid, spellId, energizePowerType, amount,
                                             owner_.combatLogFeed().periodicByAttribute(spellId));
    if ((isPlayerVictim || isPlayerCaster) && amount > 0)
        owner_.addCombatText(CombatTextEntry::ENERGIZE, amount, spellId, isPlayerCaster, energizePowerType, casterGuid, victimGuid);
    packet.skipAll();
}

void SpellHandler::handleExtraAuraInfo(network::Packet& packet, bool isInit) {
    // TBC 2.4.3 aura tracking: replaces SMSG_AURA_UPDATE which doesn't exist in TBC.
    // Format: uint64 targetGuid + uint8 count + N×{uint8 slot, uint32 spellId,
    //         uint8 effectIndex, uint8 flags, uint32 durationMs, uint32 maxDurationMs}
    auto remaining = [&]() { return packet.getRemainingSize(); };
    if (remaining() < 9) { packet.skipAll(); return; }
    uint64_t auraTargetGuid = packet.readUInt64();
    uint8_t count = packet.readUInt8();

    std::vector<AuraSlot>* auraList = nullptr;
    if (auraTargetGuid == owner_.getPlayerGuid())       auraList = &playerAuras_;
    else if (auraTargetGuid == owner_.getTargetGuid())   auraList = &targetAuras_;
    else if (auraTargetGuid != 0)                   auraList = &unitAurasCache_[auraTargetGuid];

    if (auraList && isInit) auraList->clear();

    uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    for (uint8_t i = 0; i < count && remaining() >= 15; i++) {
        uint8_t  slot        = packet.readUInt8();   // 1 byte
        uint32_t spellId     = packet.readUInt32();  // 4 bytes
        (void)               packet.readUInt8();     // effectIndex: 1 byte (unused for slot display)
        uint8_t  flags       = packet.readUInt8();   // 1 byte
        uint32_t durationMs  = packet.readUInt32();  // 4 bytes
        uint32_t maxDurMs    = packet.readUInt32();  // 4 bytes - total 15 bytes per entry

        if (auraList) {
            while (auraList->size() <= slot) auraList->push_back(AuraSlot{});
            AuraSlot& a = (*auraList)[slot];
            a.spellId      = spellId;
            // TBC uses same flag convention as Classic: 0x02=harmful, 0x04=beneficial.
            // Normalize to WotLK SMSG_AURA_UPDATE convention: 0x80=debuff, 0=buff.
            a.flags        = (flags & 0x02) ? 0x80u : 0u;
            a.durationMs   = (durationMs == 0xFFFFFFFF) ? -1 : static_cast<int32_t>(durationMs);
            a.maxDurationMs= (maxDurMs   == 0xFFFFFFFF) ? -1 : static_cast<int32_t>(maxDurMs);
            a.receivedAtMs = nowMs;
        }
    }
    if (auraList) mirrorAurasByGuid(auraTargetGuid, *auraList);
    if (auraList && owner_.addonEventCallbackRef()) {
        std::string unitId;
        if (auraTargetGuid == owner_.getPlayerGuid()) unitId = "player";
        else if (auraTargetGuid == owner_.getTargetGuid()) unitId = "target";
        else if (auraTargetGuid == owner_.focusGuidRef()) unitId = "focus";
        else if (auraTargetGuid == owner_.petGuidRef()) unitId = "pet";
        if (!unitId.empty()) owner_.addonEventCallbackRef()("UNIT_AURA", {unitId});
        // The 1.12 name, which carries no unit. See the note at the
        // site that fires it with a comment in full.
        if (unitId == "player") owner_.addonEventCallbackRef()("PLAYER_AURAS_CHANGED", {});
        // Whether a companion is out is an aura on the player, so the tab's
        // "active" mark moves with one.
        if (unitId == "player") owner_.announceCompanionChange();
    }
    if (auraTargetGuid == owner_.getPlayerGuid()) {
        refreshRestorationFromPlayerAuras();
    }
    packet.skipAll();
}

void SpellHandler::handleSpellDispelLog(network::Packet& packet) {
    // Classic/Turtle: packed casterGuid + packed victimGuid + uint32 dispelSpell + uint8 isStolen
    // TBC:            full uint64 casterGuid + full uint64 victimGuid + ...
    // + uint32 count + count × (uint32 dispelled_spellId + uint32 unk)
    // WotLK (0x006ce2b0): the victim first, and the byte after the spell
    // dropped - a dispel is a dispel, a steal comes as SMSG_SPELLSTEALLOG.
    if (isActiveExpansion("wotlk")) {
        SpellDispelLogData d;
        const bool parsed = SpellDispelLogParser::parse(packet, d);
        if (parsed) {
            for (const auto& aura : d.auras)
                owner_.combatLogFeed().dispel(d.casterGuid, d.victimGuid, d.spellId, aura.spellId, aura.flag != 0,
                                              false);
        }
        if (parsed && (d.victimGuid == owner_.getPlayerGuid() || d.casterGuid == owner_.getPlayerGuid())) {
            const bool isPlayerCaster = d.casterGuid == owner_.getPlayerGuid();
            for (const auto& aura : d.auras)
                if (aura.spellId != 0)
                    owner_.addCombatText(CombatTextEntry::DISPEL, 0, aura.spellId, isPlayerCaster, 0, d.casterGuid,
                                         d.victimGuid);
        }
        packet.skipAll();
        return;
    }
    const bool dispelUsesFullGuid = isActiveExpansion("tbc");
    if (!packet.hasRemaining(dispelUsesFullGuid ? 8u : 1u)
        || (!dispelUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = dispelUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(dispelUsesFullGuid ? 8u : 1u)
        || (!dispelUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = dispelUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(9)) return;
    /*uint32_t dispelSpell =*/ packet.readUInt32();
    uint8_t isStolen = packet.readUInt8();
    uint32_t count   = packet.readUInt32();
    // Preserve every dispelled aura in the combat log instead of collapsing
    // multi-aura packets down to the first entry only.
    const size_t dispelEntrySize = dispelUsesFullGuid ? 8u : 5u;
    std::vector<uint32_t> dispelledIds;
    dispelledIds.reserve(count);
    for (uint32_t i = 0; i < count && packet.hasRemaining(dispelEntrySize); ++i) {
        uint32_t dispelledId = packet.readUInt32();
        if (dispelUsesFullGuid) {
            /*uint32_t unk =*/ packet.readUInt32();
        } else {
            /*uint8_t isPositive =*/ packet.readUInt8();
        }
        if (dispelledId != 0) {
            dispelledIds.push_back(dispelledId);
        }
    }
    // Show system message if player was victim or caster
    if (victimGuid == owner_.getPlayerGuid() || casterGuid == owner_.getPlayerGuid()) {
        std::vector<uint32_t> loggedIds;
        if (isStolen) {
            loggedIds.reserve(dispelledIds.size());
            for (uint32_t dispelledId : dispelledIds) {
                if (owner_.shouldLogSpellstealAura(casterGuid, victimGuid, dispelledId))
                    loggedIds.push_back(dispelledId);
            }
        } else {
            loggedIds = dispelledIds;
        }

        // No chat line: SMSG_SPELLDISPELLOG goes to the combat log alone
        // (0x006ce3b0 -> 0x006ce2b0). This printed "You dispel X." and the
        // like in English.
        // Preserve stolen auras as spellsteal events so the log wording stays accurate.
        if (!loggedIds.empty()) {
            bool isPlayerCaster = (casterGuid == owner_.getPlayerGuid());
            for (uint32_t dispelledId : loggedIds) {
                owner_.addCombatText(isStolen ? CombatTextEntry::STEAL : CombatTextEntry::DISPEL,
                              0, dispelledId, isPlayerCaster, 0,
                              casterGuid, victimGuid);
            }
        }
    }
    packet.skipAll();
}

void SpellHandler::handleSpellBreakLog(network::Packet& packet) {
    // 0x006ce2b0 with 0x14f: the dispel log's shape, each aura broken - by the
    // header's spell, SPELL_AURA_BROKEN_SPELL, or by damage when it is 0.
    SpellDispelLogData d;
    if (isActiveExpansion("wotlk") && SpellDispelLogParser::parse(packet, d)) {
        for (const auto& aura : d.auras)
            owner_.combatLogFeed().auraBroken(d.casterGuid, d.victimGuid, d.spellId, aura.spellId, aura.flag != 0);
    }
    packet.skipAll();
}

void SpellHandler::handleSpellStealLog(network::Packet& packet) {
    // Sent to the CASTER (Mage) when Spellsteal succeeds.
    if (isActiveExpansion("wotlk")) {
        // 0x006ce2b0 with 0x333: the dispel log's shape, logged as stolen.
        SpellDispelLogData d;
        if (SpellDispelLogParser::parse(packet, d)) {
            for (const auto& aura : d.auras)
                owner_.combatLogFeed().dispel(d.casterGuid, d.victimGuid, d.spellId, aura.spellId, aura.flag != 0,
                                              true);
            if (d.casterGuid == owner_.getPlayerGuid() || d.victimGuid == owner_.getPlayerGuid()) {
                const bool isPlayerCaster = d.casterGuid == owner_.getPlayerGuid();
                for (const auto& aura : d.auras) {
                    if (owner_.shouldLogSpellstealAura(d.casterGuid, d.victimGuid, aura.spellId))
                        owner_.addCombatText(CombatTextEntry::STEAL, 0, aura.spellId, isPlayerCaster, 0,
                                             d.casterGuid, d.victimGuid);
                }
            }
        }
        packet.skipAll();
        return;
    }
    // Wire format mirrors SPELLDISPELLOG:
    // WotLK/Classic/Turtle: packed victim + packed caster + uint32 spellId + uint8 isStolen + uint32 count
    //                        + count × (uint32 stolenSpellId + uint8 isPositive)
    // TBC:                   full uint64 victim + full uint64 caster + same tail
    const bool stealUsesFullGuid = isActiveExpansion("tbc");
    if (!packet.hasRemaining(stealUsesFullGuid ? 8u : 1u)
        || (!stealUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t stealVictim = stealUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(stealUsesFullGuid ? 8u : 1u)
        || (!stealUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t stealCaster = stealUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(9)) {
        packet.skipAll(); return;
    }
    /*uint32_t stealSpellId =*/ packet.readUInt32();
    /*uint8_t  isStolen    =*/ packet.readUInt8();
    uint32_t stealCount   = packet.readUInt32();
    // Preserve every stolen aura in the combat log instead of only the first.
    const size_t stealEntrySize = stealUsesFullGuid ? 8u : 5u;
    std::vector<uint32_t> stolenIds;
    stolenIds.reserve(stealCount);
    for (uint32_t i = 0; i < stealCount && packet.hasRemaining(stealEntrySize); ++i) {
        uint32_t stolenId = packet.readUInt32();
        if (stealUsesFullGuid) {
            /*uint32_t unk =*/ packet.readUInt32();
        } else {
            /*uint8_t isPos  =*/ packet.readUInt8();
        }
        if (stolenId != 0) {
            stolenIds.push_back(stolenId);
        }
    }
    if (stealCaster == owner_.getPlayerGuid() || stealVictim == owner_.getPlayerGuid()) {
        std::vector<uint32_t> loggedIds;
        loggedIds.reserve(stolenIds.size());
        for (uint32_t stolenId : stolenIds) {
            if (owner_.shouldLogSpellstealAura(stealCaster, stealVictim, stolenId))
                loggedIds.push_back(stolenId);
        }

        // No chat line: SMSG_SPELLSTEALLOG goes to the combat log alone
        // (0x006ce3b0, as the dispel log). This printed "You stole X." in
        // English.
        // Some servers emit both SPELLDISPELLOG(isStolen=1) and SPELLSTEALLOG
        // for the same aura. Keep the first event and suppress the duplicate.
        if (!loggedIds.empty()) {
            bool isPlayerCaster = (stealCaster == owner_.getPlayerGuid());
            for (uint32_t stolenId : loggedIds) {
                owner_.addCombatText(CombatTextEntry::STEAL, 0, stolenId, isPlayerCaster, 0,
                              stealCaster, stealVictim);
            }
        }
    }
    packet.skipAll();
}

void SpellHandler::handleSpellChanceProcLog(network::Packet& packet) {
    // WotLK/Classic/Turtle: packed_guid target + packed_guid caster + uint32 spellId + ...
    // TBC:                  uint64 target + uint64 caster + uint32 spellId + ...
    const bool procChanceUsesFullGuid = isActiveExpansion("tbc");
    auto readProcChanceGuid = [&]() -> uint64_t {
        if (procChanceUsesFullGuid)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    if (!packet.hasRemaining(procChanceUsesFullGuid ? 8u : 1u)
        || (!procChanceUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t procTargetGuid = readProcChanceGuid();
    if (!packet.hasRemaining(procChanceUsesFullGuid ? 8u : 1u)
        || (!procChanceUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t procCasterGuid = readProcChanceGuid();
    if (!packet.hasRemaining(4)) {
        packet.skipAll(); return;
    }
    uint32_t procSpellId = packet.readUInt32();
    // Show a "PROC!" floating text when the player triggers the proc
    if (procCasterGuid == owner_.getPlayerGuid() && procSpellId > 0)
        owner_.addCombatText(CombatTextEntry::PROC_TRIGGER, 0, procSpellId, true, 0,
                      procCasterGuid, procTargetGuid);
    packet.skipAll();
}

void SpellHandler::handleSpellInstaKillLog(network::Packet& packet) {
    // Sent when a unit is killed by a spell with SPELL_ATTR_EX2_INSTAKILL (e.g. Execute, Obliterate, etc.)
    // Classic/Turtle: packed_guid caster + packed_guid victim + uint32 spellId
    // TBC, WotLK:     full uint64 caster + full uint64 victim + uint32 spellId (0x006ce260)
    SpellLogGuidsData ik;
    if (!isClassicLikeExpansion()) {
        if (!SpellLogGuidsParser::parse(packet, ik, false)) {
            packet.skipAll(); return;
        }
    } else {
        if (!packet.hasFullPackedGuid()) {
            packet.skipAll(); return;
        }
        ik.casterGuid = packet.readPackedGuid();
        if (!packet.hasFullPackedGuid()) {
            packet.skipAll(); return;
        }
        ik.victimGuid = packet.readPackedGuid();
        if (!packet.hasRemaining(4)) {
            packet.skipAll(); return;
        }
        ik.spellId = packet.readUInt32();
    }
    const uint64_t ikCaster = ik.casterGuid;
    const uint64_t ikVictim = ik.victimGuid;
    const uint32_t ikSpell = ik.spellId;
    if (isActiveExpansion("wotlk")) owner_.combatLogFeed().instakill(ikCaster, ikVictim, ikSpell);
    // Show kill/death feedback for the local player
    if (ikCaster == owner_.getPlayerGuid()) {
        owner_.addCombatText(CombatTextEntry::INSTAKILL, 0, ikSpell, true, 0, ikCaster, ikVictim);
    } else if (ikVictim == owner_.getPlayerGuid()) {
        // Into the combat log and no further: 0x006ce260 -> 0x00752210 logs
        // it and says nothing. This printed an English line on screen and
        // in chat.
        owner_.addCombatText(CombatTextEntry::INSTAKILL, 0, ikSpell, false, 0, ikCaster, ikVictim);
    }
    LOG_DEBUG("SMSG_SPELLINSTAKILLLOG: caster=0x", std::hex, ikCaster,
              " victim=0x", ikVictim, std::dec, " spell=", ikSpell);
    packet.skipAll();
}

// ---- handleSpellLogExecute, per effect logged (extracted to reduce nesting) ----

void SpellHandler::logEffectPowerDrain(const SpellLogExecuteEntry& entry, uint64_t caster, uint32_t spellId,
                                       bool isPlayerCaster) {
    // SPELL_EFFECT_POWER_DRAIN / POWER_BURN: target + amount + powerType + multiplier
    const uint64_t playerGuid = owner_.getPlayerGuid();
    const uint64_t drainTarget = entry.targetGuid;
    const uint32_t drainAmount = entry.value1;
    const uint32_t drainPower = entry.value2;  // 0=mana,1=rage,3=energy,6=runic
    const float drainMult = entry.multiplier;
    LOG_DEBUG("SMSG_SPELLLOGEXECUTE POWER_DRAIN: spell=", spellId,
              " power=", drainPower, " amount=", drainAmount,
              " multiplier=", drainMult);
    if (drainAmount == 0) return;
    // 0x00752100: taken from the target's predicted power, happiness aside.
    if (drainPower != 4) owner_.addPredictedPower(drainTarget, static_cast<int>(drainPower), -static_cast<int32_t>(drainAmount));

    const auto powerByte = static_cast<uint8_t>(drainPower);
    if (drainTarget == playerGuid)
        owner_.addCombatText(CombatTextEntry::POWER_DRAIN,
                             static_cast<int32_t>(drainAmount), spellId, false,
                             powerByte, caster, drainTarget);
    if (!isPlayerCaster) return;
    if (drainTarget != playerGuid)
        owner_.addCombatText(CombatTextEntry::POWER_DRAIN,
                             static_cast<int32_t>(drainAmount), spellId, true,
                             powerByte, caster, drainTarget);
    if (drainMult <= 0.0f || !std::isfinite(drainMult)) return;
    const uint32_t gained = static_cast<uint32_t>(
        std::lround(static_cast<double>(drainAmount) * static_cast<double>(drainMult)));
    if (gained > 0)
        owner_.addCombatText(CombatTextEntry::ENERGIZE,
                             static_cast<int32_t>(gained), spellId, true,
                             powerByte, caster, caster);
}

void SpellHandler::logEffectCreateItem(const SpellLogExecuteEntry& entry, uint32_t spellId, bool isPlayerCaster) {
    // SPELL_EFFECT_CREATE_ITEM / CREATE_RANDOM_ITEM / CREATE_ITEM_2: the item
    const uint32_t itemEntry = entry.value1;
    if (!isPlayerCaster || itemEntry == 0) return;
    // No chat line here: SMSG_SPELLLOGEXECUTE is combat log only
    // (0x006d3730), and the item is announced by the item push that
    // brings it (LOOT_ITEM_CREATED_SELF). This printed "You create X
    // using Y." in English.
    owner_.ensureItemInfo(itemEntry);
    LOG_DEBUG("SMSG_SPELLLOGEXECUTE CREATE_ITEM: spell=", spellId, " item=", itemEntry);
}

void SpellHandler::logEffectInterruptCast(const SpellLogExecuteEntry& entry, uint64_t caster, uint32_t spellId,
                                          bool isPlayerCaster) {
    // SPELL_EFFECT_INTERRUPT_CAST: target + interrupted_spell_id
    const uint64_t playerGuid = owner_.getPlayerGuid();
    const uint64_t icTarget = entry.targetGuid;
    const uint32_t icSpellId = entry.value1;
    // Clear the interrupted unit's cast bar immediately
    unitCastStates_.erase(icTarget);
    noteUnitCastEnd(icTarget, false);
    // Record interrupt in combat log when player is involved
    if (isPlayerCaster || icTarget == playerGuid)
        owner_.addCombatText(CombatTextEntry::INTERRUPT, 0, icSpellId, isPlayerCaster, 0,
                             caster, icTarget);
    LOG_DEBUG("SMSG_SPELLLOGEXECUTE INTERRUPT_CAST: spell=", spellId,
              " interrupted=", icSpellId, " target=0x", std::hex, icTarget, std::dec);
}

void SpellHandler::logEffectFeedPet(const SpellLogExecuteEntry& entry, bool isPlayerCaster) {
    // SPELL_EFFECT_FEED_PET: the item
    const uint32_t feedItem = entry.value1;
    if (!isPlayerCaster || feedItem == 0) return;
    // No chat line: combat log only (0x006d3730). This printed "You feed
    // your pet X." in English.
    owner_.ensureItemInfo(feedItem);
    LOG_DEBUG("SMSG_SPELLLOGEXECUTE FEED_PET: item=", feedItem);
}

/// 0x006d3220's table: the combat log entry each logged effect makes.
void SpellHandler::logExecuteEntry(const SpellLogExecuteEntry& entry, uint64_t caster, uint32_t spellId) {
    auto& feed = owner_.combatLogFeed();
    switch (entry.effect) {
        case SpellEffect::POWER_DRAIN:
        case SpellEffect::POWER_BURN:
            feed.spellDrain(caster, entry.targetGuid, spellId, entry.value2, entry.value1, entry.multiplier,
                            feed.periodicByAttribute(spellId));
            break;
        case 18: case 113:  // RESURRECT, RESURRECT_NEW
            feed.resurrect(caster, entry.targetGuid, spellId);
            break;
        case 19:  // ADD_EXTRA_ATTACKS
            feed.extraAttacks(caster, entry.targetGuid, spellId, static_cast<int32_t>(entry.value1));
            break;
        case SpellEffect::INTERRUPT_CAST:
            feed.interrupt(caster, entry.targetGuid, spellId, entry.value1);
            break;
        case 28: case 50: case 56: case 76: case 81: case 83:  // the summons and the objects made
        case 104: case 105: case 106: case 107:
            feed.summon(caster, entry.targetGuid, spellId);
            break;
        case 111:  // DURABILITY_DAMAGE: every item when both words are -1
            if (entry.value1 == 0xFFFFFFFFu && entry.value2 == 0xFFFFFFFFu)
                feed.durabilityDamageAll(caster, entry.targetGuid, spellId);
            else
                feed.durabilityDamage(caster, entry.targetGuid, spellId, entry.value1);
            break;
        default:
            break;
    }
}

void SpellHandler::handleSpellLogExecute(network::Packet& packet) {
    // WotLK/Classic/Turtle: packed_guid caster + uint32 spellId + uint32 effectCount
    // TBC:                  uint64 caster + uint32 spellId + uint32 effectCount
    // Per effect: uint32 effect + uint32 count + what 0x006d3220 reads for it
    // (SpellLogExecuteParser) - nothing for an effect it does not name.
    SpellLogExecuteData data;
    const bool ok = SpellLogExecuteParser::parse(packet, data, isActiveExpansion("tbc"));
    packet.skipAll();
    if (!ok && data.entries.empty()) return;

    const bool isPlayerCaster = (data.casterGuid == owner_.getPlayerGuid());
    if (isActiveExpansion("wotlk")) {
        for (const auto& entry : data.entries) logExecuteEntry(entry, data.casterGuid, data.spellId);
    }
    for (const auto& entry : data.entries) {
        switch (entry.effect) {
            case SpellEffect::POWER_DRAIN:
            case SpellEffect::POWER_BURN:
                logEffectPowerDrain(entry, data.casterGuid, data.spellId, isPlayerCaster);
                break;
            case SpellEffect::CREATE_ITEM:
            case SpellEffect::CREATE_RANDOM_ITEM:
            case SpellEffect::CREATE_ITEM2:
                logEffectCreateItem(entry, data.spellId, isPlayerCaster);
                break;
            case SpellEffect::INTERRUPT_CAST:
                logEffectInterruptCast(entry, data.casterGuid, data.spellId, isPlayerCaster);
                break;
            case SpellEffect::FEED_PET:
                logEffectFeedPet(entry, isPlayerCaster);
                break;
            default:
                break;
        }
    }
}

void SpellHandler::handleClearExtraAuraInfo(network::Packet& packet) {
    // TBC 2.4.3: clear a single aura slot for a unit
    // Format: uint64 targetGuid + uint8 slot
    if (packet.hasRemaining(9)) {
        uint64_t clearGuid  = packet.readUInt64();
        uint8_t  slot       = packet.readUInt8();
        std::vector<AuraSlot>* auraList = nullptr;
        if (clearGuid == owner_.getPlayerGuid())       auraList = &playerAuras_;
        else if (clearGuid == owner_.getTargetGuid())   auraList = &targetAuras_;
        else if (clearGuid != 0)                         auraList = &unitAurasCache_[clearGuid];
        if (auraList && slot < auraList->size()) {
            (*auraList)[slot] = AuraSlot{};
        }
        if (auraList) mirrorAurasByGuid(clearGuid, *auraList);
        if (auraList && owner_.addonEventCallbackRef()) {
            std::string unitId;
            if (clearGuid == owner_.getPlayerGuid()) unitId = "player";
            else if (clearGuid == owner_.getTargetGuid()) unitId = "target";
            else if (clearGuid == owner_.focusGuidRef()) unitId = "focus";
            else if (clearGuid == owner_.petGuidRef()) unitId = "pet";
            if (!unitId.empty()) owner_.addonEventCallbackRef()("UNIT_AURA", {unitId});
            // The 1.12 name, which carries no unit. See the note at the
            // site that fires it with a comment in full.
            if (unitId == "player") owner_.addonEventCallbackRef()("PLAYER_AURAS_CHANGED", {});
        }
        if (clearGuid == owner_.getPlayerGuid()) {
            refreshRestorationFromPlayerAuras();
        }
    }
    packet.skipAll();
}

void SpellHandler::handleItemEnchantTimeUpdate(network::Packet& packet) {
    // Format: uint64 itemGuid + uint32 enchantmentSlot + uint32 durationSec + uint64 playerGuid
    //
    // The slot here is the item's *enchantment* slot (TEMP_ENCHANTMENT_SLOT = 1),
    // not the equipment slot - reading it as one labels every temporary enchant
    // "Off Hand", even on a two-hander. The item GUID is what says where it sits.
    if (!packet.hasRemaining(24)) {
        packet.skipAll(); return;
    }
    uint64_t itemGuid    = packet.readUInt64();
    /*uint32_t enchantmentSlot =*/ packet.readUInt32();
    uint32_t durationSec = packet.readUInt32();
    /*uint64_t playerGuid =*/ packet.readUInt64();

    if (itemGuid == 0) return;

    // Map the enchanted item to the weapon slot it is equipped in.
    static constexpr EquipSlot kWeaponSlots[] = {
        EquipSlot::MAIN_HAND, EquipSlot::OFF_HAND, EquipSlot::RANGED
    };
    uint32_t enchSlot = 0xFFFFFFFF;
    for (uint32_t i = 0; i < 3; ++i) {
        if (owner_.getEquipSlotGuid(static_cast<int>(kWeaponSlots[i])) == itemGuid) {
            enchSlot = i;
            break;
        }
    }
    if (enchSlot > 2) return;  // enchanted item is not an equipped weapon

    uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (durationSec == 0) {
        // Enchant expired / removed - erase the slot entry
        owner_.tempEnchantTimersRef().erase(
            std::remove_if(owner_.tempEnchantTimersRef().begin(), owner_.tempEnchantTimersRef().end(),
                           [enchSlot](const GameHandler::TempEnchantTimer& t) { return t.slot == enchSlot; }),
            owner_.tempEnchantTimersRef().end());
    } else {
        uint64_t expireMs = nowMs + static_cast<uint64_t>(durationSec) * 1000u;
        bool found = false;
        for (auto& t : owner_.tempEnchantTimersRef()) {
            if (t.slot == enchSlot) { t.expireMs = expireMs; found = true; break; }
        }
        if (!found) owner_.tempEnchantTimersRef().push_back({.slot = enchSlot, .expireMs = expireMs});
        // No warnings: 0x006e6330 keeps the time for the buff frame's
        // enchant buttons and prints nothing. These were English chat lines
        // at five minutes and one minute.
    }
    LOG_DEBUG("SMSG_ITEM_ENCHANT_TIME_UPDATE: slot=", enchSlot, " dur=", durationSec, "s");
}

void SpellHandler::handleResumeCastBar(network::Packet& packet) {
    // WotLK: packed_guid caster + packed_guid target + uint32 spellId + uint32 remainingMs + uint32 totalMs + uint8 schoolMask
    // TBC/Classic: uint64 caster + uint64 target + ...
    const bool rcbTbc = isPreWotlk();
    auto remaining = [&]() { return packet.getRemainingSize(); };
    if (remaining() < (rcbTbc ? 8u : 1u)) return;
    uint64_t caster = rcbTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < (rcbTbc ? 8u : 1u)) return;
    if (rcbTbc) packet.readUInt64(); // target (discard)
    else (void)packet.readPackedGuid(); // target
    if (remaining() < 12) return;
    uint32_t spellId   = packet.readUInt32();
    uint32_t remainMs  = packet.readUInt32();
    uint32_t totalMs   = packet.readUInt32();
    // 0x008020c0: a spell Spell.dbc does not have is ignored; one with
    // AttributesEx 0x4 resumes as a channel (UNIT_SPELLCAST_CHANNEL_START),
    // anything else as a cast (UNIT_SPELLCAST_START).
    loadSpellNameCache();
    const auto entry = owner_.spellNameCacheRef().find(spellId);
    if (entry == owner_.spellNameCacheRef().end()) return;
    const bool channel = (entry->second.attrEx & 0x4u) != 0;
    if (totalMs > 0) {
        if (caster == owner_.getPlayerGuid()) {
            casting_            = true;
            castIsChannel_      = channel;
            currentCastSpellId_ = spellId;
            castTimeTotal_      = totalMs  / 1000.0f;
            castTimeRemaining_  = remainMs / 1000.0f;
        } else {
            auto& s = unitCastStates_[caster];
            s.casting       = true;
            s.isChannel     = channel;
            s.spellId       = spellId;
            s.timeTotal     = totalMs  / 1000.0f;
            s.timeRemaining = remainMs / 1000.0f;
        }
        LOG_DEBUG("SMSG_RESUME_CAST_BAR: caster=0x", std::hex, caster, std::dec,
                  " spell=", spellId, " remaining=", remainMs, "ms total=", totalMs, "ms",
                  channel ? " (channel)" : "");
        if (owner_.addonEventCallbackRef()) {
            const auto unitId = owner_.guidToUnitId(caster);
            if (!unitId.empty())
                owner_.fireAddonEvent(channel ? "UNIT_SPELLCAST_CHANNEL_START" : "UNIT_SPELLCAST_START",
                                      spellcastArgs(unitId, spellId));
        }
    }
}

void SpellHandler::handleChannelStart(network::Packet& packet) {
    // casterGuid + uint32 spellId + uint32 totalDurationMs
    const bool tbcOrClassic = isPreWotlk();
    uint64_t chanCaster = tbcOrClassic
        ? (packet.hasRemaining(8) ? packet.readUInt64() : 0)
        : packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;
    uint32_t chanSpellId = packet.readUInt32();
    uint32_t chanTotalMs = packet.readUInt32();
    if (chanTotalMs > 0 && chanCaster != 0) {
        if (chanCaster == owner_.getPlayerGuid()) {
            casting_            = true;
            castIsChannel_      = true;
            currentCastSpellId_ = chanSpellId;
            castTimeTotal_      = chanTotalMs / 1000.0f;
            castTimeRemaining_  = castTimeTotal_;
        } else {
            auto& s = unitCastStates_[chanCaster];
            s.casting        = true;
            s.isChannel      = true;
            s.spellId        = chanSpellId;
            s.timeTotal      = chanTotalMs / 1000.0f;
            s.timeRemaining  = s.timeTotal;
            s.interruptible  = owner_.isSpellInterruptible(chanSpellId);
        }
        LOG_DEBUG("MSG_CHANNEL_START: caster=0x", std::hex, chanCaster, std::dec,
                  " spell=", chanSpellId, " total=", chanTotalMs, "ms");

        // Play channeling animation (looping)
        // Channel packets don't carry targetGuid - use player's current target as hint
        SpellCastType chanType = SpellCastType::OMNI;
        if (chanCaster == owner_.getPlayerGuid() && owner_.getTargetGuid() != 0)
            chanType = SpellCastType::DIRECTED;
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(chanCaster, true, true, chanType);
        }

        // Fire UNIT_SPELLCAST_CHANNEL_START for Lua addons
        if (owner_.addonEventCallbackRef()) {
            auto unitId = owner_.guidToUnitId(chanCaster);
            if (!unitId.empty())
                owner_.fireAddonEvent("UNIT_SPELLCAST_CHANNEL_START", spellcastArgs(unitId, chanSpellId));
        }
    }
}

void SpellHandler::handleChannelUpdate(network::Packet& packet) {
    // casterGuid + uint32 remainingMs
    const bool tbcOrClassic2 = isPreWotlk();
    uint64_t chanCaster2 = tbcOrClassic2
        ? (packet.hasRemaining(8) ? packet.readUInt64() : 0)
        : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t chanRemainMs = packet.readUInt32();
    if (chanCaster2 == owner_.getPlayerGuid()) {
        castTimeRemaining_ = chanRemainMs / 1000.0f;
        if (chanRemainMs == 0) {
            casting_ = false;
            castIsChannel_ = false;
            currentCastSpellId_ = 0;
        }
    } else if (chanCaster2 != 0) {
        auto it = unitCastStates_.find(chanCaster2);
        if (it != unitCastStates_.end()) {
            it->second.timeRemaining = chanRemainMs / 1000.0f;
            if (chanRemainMs == 0) {
                unitCastStates_.erase(it);
                noteUnitCastEnd(chanCaster2, true);
            }
        }
    }
    LOG_DEBUG("MSG_CHANNEL_UPDATE: caster=0x", std::hex, chanCaster2, std::dec,
              " remaining=", chanRemainMs, "ms");
    // Every update, not only the last: a channel that reticks or is pushed
    // back moves its bar, and the bar is already on screen.
    if (const std::string chanUnit = owner_.guidToUnitId(chanCaster2); !chanUnit.empty()) {
        owner_.fireAddonEvent("UNIT_SPELLCAST_CHANNEL_UPDATE", {chanUnit});
    }
    // Fire UNIT_SPELLCAST_CHANNEL_STOP when channel ends
    if (chanRemainMs == 0) {
        // Stop channeling animation - return to idle
        if (owner_.spellCastAnimCallbackRef()) {
            owner_.spellCastAnimCallbackRef()(chanCaster2, false, true, SpellCastType::OMNI);
        }
        auto unitId = owner_.guidToUnitId(chanCaster2);
        if (!unitId.empty())
            owner_.fireAddonEvent("UNIT_SPELLCAST_CHANNEL_STOP", {unitId});
    }
}

// ============================================================
// Pet Stable
// ============================================================

void SpellHandler::requestStabledPetList() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || owner_.stableMasterGuidRef() == 0) return;
    auto pkt = ListStabledPetsPacket::build(owner_.stableMasterGuidRef());
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sent MSG_LIST_STABLED_PETS to npc=0x", std::hex, owner_.stableMasterGuidRef(), std::dec);
}

void SpellHandler::stablePet() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || owner_.stableMasterGuidRef() == 0) return;
    if (owner_.petGuidRef() == 0) {
        // Silent, as StablePet (0x005a19c0) is without a pet. This was English.
        return;
    }
    auto pkt = StablePetPacket::build(owner_.stableMasterGuidRef());
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_STABLE_PET");
}

// The stable master sells a fourth, fifth and sixth slot; the panel's buy
// button asks for one at a time and the server bills for whichever is next.
// CMSG_BUY_STABLE_SLOT carries nothing but who is being asked - HandleBuyStableSlot
// reads a guid and checks it is a stable master, and works the price out itself.
void SpellHandler::buyStableSlot() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() ||
        owner_.stableMasterGuidRef() == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_BUY_STABLE_SLOT));
    pkt.writeUInt64(owner_.stableMasterGuidRef());
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_BUY_STABLE_SLOT to npc=0x", std::hex,
             owner_.stableMasterGuidRef(), std::dec);
}

// Abandoning is not dismissing. Dismiss puts a hunter's pet away and it can be
// called back; abandon gives it up for good, which is why the interface asks
// first and why this is its own message rather than another pet action.
void SpellHandler::abandonPet() {
    if (owner_.petGuidRef() == 0 || owner_.getState() != WorldState::IN_WORLD ||
        !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PET_ABANDON));
    pkt.writeUInt64(owner_.petGuidRef());
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_PET_ABANDON for pet=0x", std::hex, owner_.petGuidRef(), std::dec);
}

void SpellHandler::unstablePet(uint32_t petNumber) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || owner_.stableMasterGuidRef() == 0 || petNumber == 0) return;
    auto pkt = UnstablePetPacket::build(owner_.stableMasterGuidRef(), petNumber);
    owner_.getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_UNSTABLE_PET: petNumber=", petNumber);
}

} // namespace game
} // namespace wowee

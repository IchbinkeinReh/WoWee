// lua_unit_api.cpp - Unit query, stats, party/raid, and player state Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "ui/portrait_claims.hpp"
#include "game/battleground_rules.hpp"
#include "game/movement_handler.hpp"
#include "game/shapeshift_forms.hpp"
#include "game/battleground_flag.hpp"
#include "game/item_slot_rules.hpp"
#include "game/chr_names.hpp"
#include "game/game_utils.hpp"
#include "game/grey_triviality.hpp"
#include "game/bg_score_defs.hpp"
#include "game/pvp_flags.hpp"
#include "rendering/selection_circle.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/object_type.hpp"
#include "addons/lua_taint.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/power_prediction.hpp"
#include "game/unit_level_display.hpp"
#include "game/known_titles.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <bit>
#include <array>
#include <algorithm>
#include <cctype>
#include "game/interface_text.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

namespace wowee::addons {

/// The name of an object that is not a unit, as 0x0060e740 finds it: an item
/// or a bag by its item record, a game object by its own; empty where it has
/// none yet or the object is not known.
static std::string objectName(game::GameHandler* gh, uint64_t guid) {
    const auto object = gh->getEntityManager().getEntity(guid);
    if (!object) return {};
    if (object->getType() == game::ObjectType::ITEM || object->getType() == game::ObjectType::CONTAINER) {
        const uint16_t entryField = game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY);
        const uint32_t entry = entryField != 0xFFFF ? object->getField(entryField) : 0;
        const auto* info = entry ? gh->getItemInfo(entry) : nullptr;
        return info && info->valid ? info->name : std::string{};
    }
    if (object->getType() == game::ObjectType::GAMEOBJECT)
        return std::static_pointer_cast<game::GameObject>(object)->getName();
    return {};
}

// UnitName(unit): the name and the realm (0x0060e740). A token that names
// nothing - "target" with no target - is nil and nil; a unit whose name the
// client does not know yet is UNKNOWNOBJECT. It said "Unknown" to both, so an
// addon testing `if UnitName("target")` found a target where there was none.
static int lua_UnitName(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    // No game object branch here either, for the reason in UnitExists: a name
    // is the one answer about an object that would have been right, and giving
    // it while UnitExists says no is a unit that half exists.
    // A Mirror Image (aura 279) answers its caster's name (0x0072a000).
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const std::string shown = unit && gh ? gh->shownUnitName(*unit) : unit ? unit->getName() : std::string{};
    if (unit && !shown.empty()) {
        lua_pushstring(L, shown.c_str());
    } else {
        // Fallback: party member name for out-of-range members. A member's
        // pet out of sight first: the name its owner's stats carried (pet
        // record +8, 0x0060e740 -> 0x00513cb0).
        const auto* pm = findRosterMember(gh, guid);
        const auto* petOwner = unit ? nullptr : findPartyMemberPet(gh, guid);
        if (petOwner && !petOwner->petName.empty()) {
            lua_pushstring(L, petOwner->petName.c_str());
        } else if (pm && !pm->name.empty()) {
            lua_pushstring(L, pm->name.c_str());
        } else if (gh && guid != 0) {
            // Try player name cache
            const std::string& cached = gh->lookupName(guid);
            if (!cached.empty()) {
                lua_pushstring(L, cached.c_str());
            } else {
                // The player's own name, from the list they picked it off.
                //
                // "Unknown" is a real answer for a stranger whose name query
                // has not come back, and a wrong one for the player: the
                // client knows who they are before it knows anything else -
                // world_loader reads this very name to set up per-character
                // SavedVariables, two lines before it loads the addons.
                //
                // Addons ask at load and keep the answer. Bagnon takes
                // UnitName('player') into a local at file scope and compares
                // every later answer against it to decide whether a bag is the
                // live one or a remembered one; given "Unknown" once, it spent
                // the session certain it was looking at an offline character
                // and drew its bags out of an empty database. No error, no
                // warning, an empty window.
                std::string own;
                if (guid == gh->getPlayerGuid()) {
                    for (const auto& c : gh->getCharacters()) {
                        if (c.guid == guid) { own = c.name; break; }
                    }
                }
                // An object that is not a unit is named by its kind
                // (0x0060e740, by the type mask at +8 of its fields): an item
                // by its item record (0x0067ca30 -> 0x004fd200), a game object
                // by its own (0x0070cdf0). "questnpc" of a quest an item or a
                // poster starts is that object, and QuestFrame's title said
                // "Unbekannt" (T354).
                if (own.empty()) own = objectName(gh, guid);
                if (own.empty()) {
                    lua_pushstring(L, game::interfaceString("UNKNOWNOBJECT", "Unknown").c_str());
                } else {
                    lua_pushstring(L, own.c_str());
                }
            }
        } else {
            lua_pushnil(L);
        }
    }
    // The other half of the pair: the server the player came from. It was
    // hardcoded nil, so a cross-realm member was named without the realm that
    // made the name theirs - and this is the only place that realm reaches
    // Lua at all, since the name query answer that carried it
    // (getCachedPlayerRealm, the +0x34 of 0x0072a000's record) had no other
    // reader among the bindings. 0x0060e740 answers two values and takes the
    // realm from that record, so a player on one's own server - whose query
    // answer names no realm - gets nil here, as they do there.
    //
    // The token "player" answers nil too, by a test that runs before any unit
    // is resolved: that function compares the argument's own text against
    // "player" and returns the local player's name with a nil second value.
    std::string realm;
    if (gh && uidStr != "player") realm = gh->getCachedPlayerRealm(guid);
    if (realm.empty()) lua_pushnil(L);
    else lua_pushstring(L, realm.c_str());
    return 2;
}


static bool isGroupMemberOrPet(game::GameHandler* gh, uint64_t guid);

/// A dead unit (UNIT_DYNAMIC_FLAGS 0x20) outside the player's group shows no
/// health and no power (0x0060eb60, 0x0060ed40 ask 0x00512a30).
static bool hidesItsBars(lua_State* L, const game::Unit& unit) {
    return (unit.getDynamicFlags() & game::UNIT_DYNFLAG_DEAD) &&
           !isGroupMemberOrPet(getGameHandler(L), unit.getGuid());
}

static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        // 0x0060eb60 -> 0x0071c2c0: the predicted health while the CVar
        // predictedHealth (registered "1") is on, else the field.
        const bool predicted = addons::storedCVarValue("predictedHealth", "1") != "0";
        const double health = predicted ? static_cast<double>(unit->getPredictedHealth())
                                        : static_cast<double>(unit->getHealth());
        lua_pushnumber(L, hidesItsBars(L, *unit) ? 0 : health);
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        // Not a member: a member's pet, from that member's stats (+0x5c).
        const auto* owner = pm ? nullptr : findPartyMemberPet(gh, guid);
        lua_pushnumber(L, pm ? pm->curHealth : owner ? owner->petCurHealth : 0);
    }
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitHealthMax(\"unit\")");   // 0x0060ec60
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxHealth());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        const auto* owner = pm ? nullptr : findPartyMemberPet(gh, guid);  // +0x60
        lua_pushnumber(L, pm ? pm->maxHealth : owner ? owner->petMaxHealth : 0);
    }
    return 1;
}

/// 0x007fde00: what a power is shown over - rage and runic power in tenths,
/// happiness in thousandths (table 0x00af5220).
static uint32_t powerDivisor(int type) {
    return type >= 0 && type < 7 ? kPowerDivisors[type] : 1;
}

/// UnitPower(unit, powerType) - the second argument names *which* bar.
///
/// Absent (or 7), it means the one the unit is using, which is what the main
/// power bar wants. Given, it names one of the seven, and alternatepowerbar.lua
/// is the caller that needs it: a druid in a form has energy or rage as its
/// current power and mana as an alternate, and that frame asks for the
/// alternate by index. Out of range, 0 (0x0060ed40).
///
/// A unit answers its own power, the player any, and nothing else; as it is
/// predicted (0x0071c2e0) and over the power's divisor. A party member out
/// of sight answers from its stats.
static int lua_UnitPower(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    int want = 7;
    if (lua_isnumber(L, 2)) {
        want = static_cast<int>(lua_tonumber(L, 2));
        if (want < 0 || want > 7) { lua_pushnumber(L, 0); return 1; }
    }
    auto* unit = resolveUnit(L, uid);
    auto* gh = getGameHandler(L);
    if (unit) {
        const int type = want == 7 ? unit->getPowerType() : want;
        const bool answers = gh && type < 7 && !hidesItsBars(L, *unit) &&
                             (type == unit->getPowerType() || unit->getGuid() == gh->getPlayerGuid());
        lua_pushnumber(L, answers ? static_cast<uint32_t>(gh->unitPower(*unit, type)) / powerDivisor(type) : 0);
    } else {
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        // Party stats carry one power and say which it is, so a request for a
        // different one is answered with nothing rather than with that.
        if (const auto* owner = pm ? nullptr : findPartyMemberPet(gh, guid)) {
            // A member's pet: its type +0x58, power +0x64 (0x0060ed40).
            const bool petSame = want == 7 || owner->petPowerType == want;
            lua_pushnumber(L, petSame ? owner->petCurPower / powerDivisor(owner->petPowerType) : 0);
            return 1;
        }
        const bool same = pm && (want == 7 || pm->powerType == want);
        lua_pushnumber(L, same ? pm->curPower / powerDivisor(pm->powerType) : 0);
    }
    return 1;
}

/// UnitPowerMax(unit, powerType) - the same, from the maximum field, of any
/// power (0x0060ef40); and the one that decides whether the alternate bar is
/// drawn at all: alternatepowerbar tests this against zero before showing
/// itself.
static int lua_UnitPowerMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    int want = 7;
    if (lua_isnumber(L, 2)) {
        want = static_cast<int>(lua_tonumber(L, 2));
        if (want < 0 || want > 7) { lua_pushnumber(L, 0); return 1; }
    }
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        const int type = want == 7 ? unit->getPowerType() : want;
        lua_pushnumber(L, unit->getMaxPowerByType(static_cast<uint8_t>(type)) / powerDivisor(type));
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        if (const auto* owner = pm ? nullptr : findPartyMemberPet(gh, guid)) {
            // A member's pet: its type +0x58, maximum +0x66 (0x0060ef40).
            const bool petSame = want == 7 || owner->petPowerType == want;
            lua_pushnumber(L, petSame ? owner->petMaxPower / powerDivisor(owner->petPowerType) : 0);
            return 1;
        }
        const bool same = pm && (want == 7 || pm->powerType == want);
        lua_pushnumber(L, same ? pm->maxPower / powerDivisor(pm->powerType) : 0);
    }
    return 1;
}

// UnitLevel(unit) (0x0060f9e0): the level, or -1 for the skull - a unit
// hostile to the player ten or more levels above them, or a boss
// (game::displayedUnitLevel).
static int lua_UnitLevel(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        auto* gh = getGameHandler(L);
        auto* player = gh ? resolveUnit(L, "player") : nullptr;
        unsigned typeFlags = 0;
        if (gh && unit->getType() == game::ObjectType::UNIT) {
            const auto& cache = gh->getCreatureInfoCache();
            const auto it = cache.find(unit->getEntry());
            if (it != cache.end()) typeFlags = it->second.typeFlags;
        }
        const int reaction = player ? gh->unitReaction(*unit, *player) : 0;
        // How drunk the player is - PLAYER_BYTES_3's byte 1 and the fake
        // inebriation (0x004f7290) - and whether the player may attack the
        // unit (0x00729740, the player attacking): a drunk player sees an
        // enemy lower than it is.
        int drunk = 0, fakeInebriation = 0;
        bool mayAttack = false;
        if (player) {
            const uint16_t b3 = game::fieldIndex(game::UF::PLAYER_BYTES_3);
            const uint16_t fake = game::fieldIndex(game::UF::PLAYER_FAKE_INEBRIATION);
            if (b3 != 0xFFFF) drunk = static_cast<int>((player->getField(b3) >> 8) & 0xFFu);
            if (fake != 0xFFFF) fakeInebriation = static_cast<int>(player->getField(fake));
            if (drunk != 0 || fakeInebriation != 0)
                mayAttack = gh->unitMayAttack(player->getGuid(), unit->getGuid());
        }
        lua_pushnumber(L, game::displayedUnitLevel(static_cast<int>(unit->getLevel()), player != nullptr,
                                                   player ? static_cast<int>(player->getLevel()) : 0,
                                                   reaction, typeFlags, drunk, fakeInebriation, mayAttack));
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->level : 0);
    }
    return 1;
}

static int lua_UnitExists(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "");
    auto* unit = resolveUnit(L, uid);
    // A game object is not a unit, and saying otherwise here does not stop at
    // the name. This client targets objects because its right-click path reads
    // the selection back, and its own target frame drew one greyed out and
    // asked nothing further. FrameXML asks a great deal further - dead, level,
    // classification, reaction - and every one of those reads the zero behind
    // an object that has no such field, so a targeted mailbox came up dead, at
    // skull level, with an attackable portrait. Retail never targets one at
    // all; the selection stays for the interaction path, and the interface is
    // told what it asked, which is that there is no unit here.
    if (unit) {
        luaPushOneOrNil(L, 1);
    } else {
        // Party members in other zones don't have entities but still "exist"
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        luaPushOneOrNil(L, guid != 0 && findRosterMember(gh, guid) != nullptr);
    }
    return 1;
}

/// UnitIsDead, UnitIsGhost and UnitIsDeadOrGhost (0x0060f480, 0x0060f580,
/// 0x0060f680). A visible unit: dead at no health, a ghost by PLAYER_FLAGS
/// 0x10 - and for the two that ask after death, also dead when its
/// UNIT_DYNAMIC_FLAGS carry 0x20 and it is not of the player's group (a
/// hunter feigning death looks dead to everyone else). A party member out of
/// sight: its roster status, 0x04 dead and 0x08 ghost. 1 or nil.
static int pushDeathState(lua_State* L, bool askDead, bool askGhost) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    auto* unit = resolveUnit(L, uid);
    bool yes = false;
    if (unit) {
        bool ghost = false;
        if (uidStr == "player" && gh) {
            ghost = gh->isPlayerGhost();
        } else if (unit->getType() == game::ObjectType::PLAYER) {
            constexpr uint32_t kPlayerFlagGhost = 0x10;
            ghost = (unit->getField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & kPlayerFlagGhost) != 0;
        }
        const bool dead = unit->getHealth() == 0;
        yes = (askDead && dead) || (askGhost && ghost);
        if (!yes && askDead && (unit->getDynamicFlags() & game::UNIT_DYNFLAG_DEAD) &&
            !isGroupMemberOrPet(gh, unit->getGuid()))
            yes = true;
    } else if (gh) {
        constexpr uint16_t kMemberDead = 0x04, kMemberGhost = 0x08;
        if (const auto* pm = findPartyMember(gh, resolveUnitGuid(gh, uidStr)))
            yes = (askDead && (pm->onlineStatus & kMemberDead)) || (askGhost && (pm->onlineStatus & kMemberGhost));
    }
    luaPushOneOrNil(L, yes);
    return 1;
}
static int lua_UnitIsDead(lua_State* L) { return pushDeathState(L, true, false); }

std::string localizedRaceName(game::GameHandler* gh, uint32_t raceId, int gender) {
    std::string name = game::chrRaceName(gh ? gh->services().assetManager : nullptr, raceId, gender).name;
    if (name.empty() && raceId > 0 && raceId < 12) name = kLuaRaces[raceId];
    return name;
}

std::string localizedClassName(game::GameHandler* gh, uint32_t classId, int gender) {
    std::string name = game::chrClassName(gh ? gh->services().assetManager : nullptr, classId, gender).name;
    if (name.empty() && classId < 12) name = kLuaClasses[classId];
    return name;
}

/// UNIT_FIELD_BYTES_0's gender byte for a unit token: true for female.
static bool unitIsFemale(game::GameHandler* gh, const std::string& lowerUid) {
    const uint64_t guid = gh ? resolveUnitGuid(gh, lowerUid) : 0;
    auto entity = guid != 0 ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (!entity) return false;
    return ((entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) >> 16) & 0xFFu) == 1;
}

/// The class of a unit the client can name (0x0060fec0, 0x00610040): the
/// player's own, a visible unit's UNIT_FIELD_BYTES_0, else the name cache.
/// 0 when none is known.
static uint8_t unitClassId(lua_State* L, game::GameHandler* gh, const std::string& uidStr) {
    if (!resolveUnit(L, uidStr.c_str())) return 0;
    if (uidStr == "player") return gh->getPlayerClass();
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    uint8_t classId = 0;
    if (auto entity = gh->getEntityManager().getEntity(guid)) {
        const uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
    }
    if (classId == 0) classId = gh->lookupPlayerClass(guid);
    return classId;
}

/// The class wears relics in the ranged place (ChrClasses flag 0x8).
static bool classWearsRelics(game::GameHandler* gh, uint8_t c) {
    auto* am = gh->services().assetManager;
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* layout = layouts ? layouts->getLayout("ChrClasses") : nullptr;
    const uint32_t flagsF = layout ? layout->tryField("Flags") : 0xFFFFFFFFu;
    auto dbc = am && am->isInitialized() ? am->loadDBCOptional("ChrClasses.dbc") : nullptr;
    bool relic = false;
    if (dbc && dbc->isLoaded() && flagsF < dbc->getFieldCount()) {
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
            if (dbc->getUInt32(r, 0) == c) { relic = (dbc->getUInt32(r, flagsF) & 0x8u) != 0; break; }
    }
    return relic;
}

/// UnitClass(unit) → the class name in the unit's gender, and its token;
/// UnitClassBase(unit) → the name without gender, and the token. Both nil
/// and nil for a unit whose class is not known - "UNKNOWN" would pass every
/// `if ( class ) then` guard written for it and then index nothing:
/// CLASS_ICON_TCOORDS has no such key, which took Blizzard_ArenaUI down at
/// load. The token is the second value, not the display name again - every
/// class-indexed table in FrameXML is keyed by it.
static int pushUnitClass(lua_State* L, bool base) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const uint8_t classId = gh ? unitClassId(L, gh, uidStr) : 0;
    const char* token = luaClassToken(classId);
    if (!token) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    // Gendered (0x007159e0) or the row's own name (+0x10).
    const std::string localized =
        game::chrClassName(gh->services().assetManager, classId, base ? 2 : (unitIsFemale(gh, uidStr) ? 1 : 0)).name;
    lua_pushstring(L, !localized.empty() ? localized.c_str() : kLuaClasses[classId]);
    lua_pushstring(L, token);
    return 2;
}
static int lua_UnitClass(lua_State* L) { return pushUnitClass(L, false); }
static int lua_UnitClassBase(lua_State* L) { return pushUnitClass(L, true); }

// UnitIsGhost(unit) - true if unit is in ghost form
static int lua_UnitIsGhost(lua_State* L) { return pushDeathState(L, false, true); }

// UnitIsDeadOrGhost(unit)
static int lua_UnitIsDeadOrGhost(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsDeadOrGhost(\"unit\")");   // 0x0060f680
    return pushDeathState(L, true, true);
}

// UnitIsAFK(unit), UnitIsDND(unit)
/// A unit predicate answered from one bit of PLAYER_FLAGS.
///
/// Answers 1 or nil, the way WoW answers a Unit predicate. bnet.lua tests
/// `UnitIsAFK("player") == 1`, which a boolean fails silently, and the two
/// chat sites test it for truth - 1 and nil satisfy both, where true/false
/// and 1/0 each break one of them.
///
/// PLAYER_FLAGS, not UNIT_FIELD_FLAGS: 0x01 there is UNIT_FLAG_SERVER_CONTROLLED
/// and has nothing to do with being away.
///
/// 0x0060cc30 (away) and 0x0060cd50 (busy) look the unit up as a player
/// object only. Without one, a group member's status byte answers (0x00513c30,
/// +8: 0x40 away, 0x80 busy). With one, the away test first answers nil for a
/// player the active player may attack (0x00729740), then 1 for the active
/// player while it has marked itself away (0x00bcefec, set by /afk before
/// the realm's flag comes back), and else PLAYER_FLAGS.
static int pushPlayerFlagPredicate(lua_State* L, uint32_t bit, uint16_t memberBit, bool away) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushnil(L); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::PLAYER) {
        for (const auto& m : gh->getPartyData().members) {
            if (m.guid == guid) { luaPushOneOrNil(L, (m.onlineStatus & memberBit) != 0); return 1; }
        }
        lua_pushnil(L);
        return 1;
    }
    if (away) {
        const uint64_t self = gh->getPlayerGuid();
        if (!gh->getEntityManager().getEntity(self)) { lua_pushnil(L); return 1; }
        if (gh->unitMayAttack(self, guid)) { lua_pushnil(L); return 1; }
        if (guid == self && gh->isAfk()) { lua_pushnumber(L, 1); return 1; }
    }
    const uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
    luaPushOneOrNil(L, (playerFlags & bit) != 0);
    return 1;
}

static int pushPvpPredicate(lua_State* L, bool freeForAll) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uid(lua_tostring(L, 1));
    toLowerInPlace(uid);
    const uint64_t guid = resolveUnitGuid(gh, uid);
    if (guid == 0) { return luaReturnNil(L); }
    const auto entity = gh->getEntityManager().getEntity(guid);
    if (entity && entity->isUnit()) {
        const uint32_t bytes2 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2));
        luaPushOneOrNil(L, ((bytes2 >> 8) & (freeForAll ? 0x04u : 0x01u)) != 0);
        return 1;
    }
    if (entity && !freeForAll && entity->getType() == game::ObjectType::CORPSE) {
        luaPushOneOrNil(L, (entity->getField(game::fieldIndex(game::UF::CORPSE_FIELD_FLAGS)) & 0x04u) != 0);
        return 1;
    }
    for (const auto& m : gh->getPartyData().members) {
        if (m.guid == guid) { luaPushOneOrNil(L, (m.onlineStatus & (freeForAll ? 0x10u : 0x02u)) != 0); return 1; }
    }
    return luaReturnNil(L);
}

// PLAYER_FLAGS 0x1 is the group leader, 0x2 away and 0x4 busy. These read 0x1
// and 0x2, one bit low, so a group leader answered as away and an away player
// as busy. The client's own away test reads 0x2 (0x0060cc30), and its chat
// path for the busy notices reads 0x4 (0x006dc640).
static int lua_UnitIsAFK(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsAFK(\"unit\")");   // 0x0060cc30
    return pushPlayerFlagPredicate(L, 0x02, 0x40, true);
}

static int lua_UnitIsDND(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsDND(\"unit\")");   // 0x0060cd50
    return pushPlayerFlagPredicate(L, 0x04, 0x80, false);
}

// UnitPlayerControlled(unit) - true for players and player-controlled pets
static int lua_UnitPlayerControlled(lua_State* L) {
    // UNIT_FLAG_PLAYER_CONTROLLED (0x8) on any unit (0x0060cba0).
    auto* unit = lua_isstring(L, 1) ? resolveUnit(L, lua_tostring(L, 1)) : nullptr;
    if (!unit) { return luaReturnNil(L); }
    luaPushOneOrNil(L, (unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x8u) != 0);
    return 1;
}

// UnitIsTapped(unit) - true if mob is tapped (tagged by any player)
static int lua_UnitIsTapped(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsTapped(\"unit\")");   // 0x00611130
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnNil(L); }
    luaPushOneOrNil(L, (unit->getDynamicFlags() & 0x0004) != 0); // UNIT_DYNFLAG_TAPPED_BY_PLAYER
    return 1;
}

// UnitIsTappedByPlayer(unit) - true if tapped by the local player (can loot)
static int lua_UnitIsTappedByPlayer(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsTappedByPlayer(\"unit\")");   // 0x006111b0
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    if (!unit) { return luaReturnNil(L); }
    // UNIT_DYNFLAG_TAPPED_BY_PLAYER (0x8).
    luaPushOneOrNil(L, (unit->getDynamicFlags() & 0x0008) != 0);
    return 1;
}

// UnitIsTappedByAllThreatList(unit) - true if shared-tag mob
static int lua_UnitIsTappedByAllThreatList(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsTappedByAllThreatList(\"unit\")");   // 0x00611230
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    if (!unit) { return luaReturnNil(L); }
    // UNIT_DYNFLAG_TAPPED_BY_ALL_THREAT_LIST (0x80).
    luaPushOneOrNil(L, (unit->getDynamicFlags() & 0x0080) != 0);
    return 1;
}

// IsReferAFriendLinked(unit) - the recruit-a-friend link (0x0051d1b0). The
// answer is 1.0 or nothing, never false, and the interface hides the
// "summon my friend" and "grant that one a level" menu entries on it
// (unitpopup.lua:787,791) before CanSummonFriend and CanGrantLevel grey them
// out. The client's predicate (0x006d7590) tries three places: the guid's own
// object when it names a player - the one a unit token can reach, and the one
// that answers with a unit field - and then two lists no unit token names, a
// four-slot table of records found by guid pair (0x0052bee0, stride 0xc4) and
// the guild's member records (0x005727b0, which hands one back at +0x10), both
// tested at the word 8 bytes into what they return and there at bit 0x100,
// with a Battle.net record behind a logged-in test last.
static int lua_IsReferAFriendLinked(lua_State* L) {
    auto* unit = resolveUnit(L, luaL_optstring(L, 1, ""));
    if (unit && dynamic_cast<game::Player*>(unit) &&
        (unit->getDynamicFlags() & game::UNIT_DYNFLAG_REFER_A_FRIEND) != 0)
        lua_pushnumber(L, 1.0);
    else
        lua_pushnil(L);
    return 1;
}

// UnitThreatSituation("unit" [, "mob"]) (0x00613a60): the unit's status on the
// mob's threat list, over every list it is on without a mob, less one; nothing
// off the lists. A unit the token reader cannot read is the usage line.
static int lua_UnitThreatSituation(lua_State* L) {
    if (!lua_isstring(L, 1) || !*lua_tostring(L, 1) || (lua_isstring(L, 2) && !*lua_tostring(L, 2)))
        return luaL_error(L, "Usage: UnitThreatSituation(\"unit\" [, \"mob\"])");
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto guidOf = [&](int arg) {
        std::string unit = lua_tostring(L, arg);
        toLowerInPlace(unit);
        return resolveUnitGuid(gh, unit);
    };
    const uint64_t unit = guidOf(1);
    if (unit == 0) return 0;
    const uint8_t status = lua_isstring(L, 2) ? gh->threatSituation(unit, guidOf(2)).standing.status
                                              : gh->threatStatus(unit);
    if (status == 0) return 0;
    lua_pushnumber(L, status - 1);
    return 1;
}

// UnitDetailedThreatSituation("unit", "mob") (0x00613b40): whether the mob's
// victim is the unit, its status, the percentage towards being it, that of the
// victim's threat, and the threat itself; nothing off the list.
static int lua_UnitDetailedThreatSituation(lua_State* L) {
    if (!lua_isstring(L, 1) || !*lua_tostring(L, 1) || !lua_isstring(L, 2) || !*lua_tostring(L, 2))
        return luaL_error(L, "Usage: UnitDetailedThreatSituation(\"unit\" [, \"mob\"])");
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto guidOf = [&](int arg) {
        std::string unit = lua_tostring(L, arg);
        toLowerInPlace(unit);
        return resolveUnitGuid(gh, unit);
    };
    const auto s = gh->threatSituation(guidOf(1), guidOf(2));
    if (s.standing.status == 0) return 0;
    luaPushOneOrNil(L, s.standing.victim);
    lua_pushnumber(L, s.standing.status - 1);
    lua_pushnumber(L, s.scaledPercent);
    lua_pushnumber(L, s.standing.percent);
    lua_pushnumber(L, s.standing.threat);
    return 5;
}

// CheckInteractDistance(unit, distIndex) → boolean
// distIndex: 1=inspect(28yd), 2=trade(11yd), 3=duel(10yd), 4=follow(28yd)
/// CheckInteractDistance("unit", distIndex) (0x0051b240): a string and a
/// number or the usage error; the index, rounded, picks the client's squared
/// distance (0x00ac8a6c, 0x00ac8a68, 0x00ac8a64, 0x00ac8a70: 30 yards to
/// inspect, 11.11 to trade, 10 to duel, 30 to follow), and the unit is within
/// it when the three-dimensional distance is strictly less. Any other index,
/// or a unit not known, is nil. WoWee used 28, 11.11, 9.9 and 28 yards.
static int lua_CheckInteractDistance(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isnumber(L, 2))
        return luaL_error(L, "Usage: CheckInteractDistance(\"unit\", distIndex)");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    static constexpr float kSquaredDistance[4] = {900.0f, 123.45678f, 100.0f, 900.0f};
    const long index = std::lround(lua_tonumber(L, 2));
    if (index < 1 || index > 4) { return luaReturnNil(L); }
    std::string uidStr(lua_tostring(L, 1));
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { return luaReturnNil(L); }
    const float dx = playerEnt->getX() - targetEnt->getX();
    const float dy = playerEnt->getY() - targetEnt->getY();
    const float dz = playerEnt->getZ() - targetEnt->getZ();
    luaPushOneOrNil(L, dx * dx + dy * dy + dz * dz < kSquaredDistance[index - 1]);
    return 1;
}

// UnitInRange(unit) → inRange, checkedOk
//
// The raid frames dim a member who has gone too far to help. Forty yards is
// what WoW means by it - the range most party-useful spells share - and the
// second return says whether the question could be answered at all: a member
// on another part of the map has no entity here, and dimming them for being
// out of range would be a guess dressed as a measurement.
// UnitInRange(unit) → 1 or nil (0x00612f10): a unit this client has that is
// the player, a member of the player's group or the pet of one (0x00512a30),
// at most forty yards from the unit the client moves (0x00ca1238, the active
// mover: the player, or what it controls), measured in three dimensions.
static int lua_UnitInRange(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitInRange(\"unit\")");
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    std::string uidStr(lua_tostring(L, 1));
    toLowerInPlace(uidStr);
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto other = guid != 0 && isGroupMemberOrPet(gh, guid) ? gh->getEntityManager().getEntity(guid) : nullptr;
    const uint64_t moverGuid = gh->activeMover() ? gh->activeMover() : gh->getPlayerGuid();
    auto mover = gh->getEntityManager().getEntity(moverGuid);
    if (!other || !other->isUnit() || !mover || !mover->isUnit()) return luaReturnNil(L);
    const float dx = mover->getX() - other->getX();
    const float dy = mover->getY() - other->getY();
    const float dz = mover->getZ() - other->getZ();
    luaPushOneOrNil(L, std::sqrt(dx * dx + dy * dy + dz * dz) <= 40.0f);
    return 1;
}

// IsRaidLeader() / IsPartyLeader-style check against the group's leader guid.
static int lua_IsRaidLeader(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const auto& group = gh->getPartyData();
    luaPushOneOrNil(L, group.leaderGuid != 0 &&
                       group.leaderGuid == gh->getPlayerGuid() ? 1 : 0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)
static int lua_UnitIsVisible(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "");
    auto* unit = resolveUnit(L, uid);
    luaPushOneOrNil(L, unit != nullptr);
    return 1;
}

/// UnitGroupRolesAssigned(unit) → isTank, isHealer, isDamage.
///
/// Three booleans, not a role name. Both callers in this interface are written
/// the same way:
///
///     local isTank, isHealer, isDamage = UnitGroupRolesAssigned(unit)
///     if ( isTank ) then ... elseif ( isHealer ) then ...
///
/// A single string filled the first of those and left the other two nil - and
/// every string is truthy in Lua, "NONE" included, so the first branch always
/// won. The player frame and every party member frame showed the tank icon
/// regardless of role, including for a player with no role and no party.
///
/// Nothing here reads the returned name, so there is no caller to keep happy:
/// playerframe.lua and partymemberframe.lua are the only two.
static int lua_UnitGroupRolesAssigned(lua_State* L) {
    auto pushRoles = [L](bool tank, bool healer, bool damage) {
        lua_pushboolean(L, tank   ? 1 : 0);
        lua_pushboolean(L, healer ? 1 : 0);
        lua_pushboolean(L, damage ? 1 : 0);
        return 3;
    };
    auto* gh = getGameHandler(L);
    if (!gh) return pushRoles(false, false, false);
    const char* uid = luaL_optstring(L, 1, "");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return pushRoles(false, false, false);
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid != guid) continue;
        // WotLK LFG roles bitmask (from SMSG_GROUP_LIST / SMSG_LFG_ROLE_CHECK_UPDATE).
        // Bit 0x01 = Leader (not a combat role), 0x02 = Tank, 0x04 = Healer, 0x08 = DPS.
        constexpr uint8_t kRoleTank    = 0x02;
        constexpr uint8_t kRoleHealer  = 0x04;
        constexpr uint8_t kRoleDamager = 0x08;
        return pushRoles((m.roles & kRoleTank)   != 0,
                         (m.roles & kRoleHealer) != 0,
                         (m.roles & kRoleDamager) != 0);
    }
    // The player is not among the members: its own role is the group list's
    // own byte (0x0060c810 -> 0x0052c860 answers 0x00bd198a for the player's
    // guid), which the player frame's role icon reads.
    if (guid == gh->getPlayerGuid())
        return pushRoles((pd.roles & 0x02) != 0, (pd.roles & 0x04) != 0, (pd.roles & 0x08) != 0);
    // Not in the party, or in no party at all: no role, and the icon is hidden
    // rather than guessed at.
    return pushRoles(false, false, false);
}

// UnitCanAttack(unit, otherUnit) → boolean
static int lua_UnitCanAttack(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitCanAttack(\"unit\", \"otherUnit\")");   // 0x0060d730
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == 0 || g2 == 0 || g1 == g2) { return luaReturnNil(L); }
    // 0x0060d730 asks 0x00729740 with unit1 attacking.
    luaPushOneOrNil(L, gh->unitMayAttack(g1, g2));
    return 1;
}

// UnitCanCooperate(unit, otherUnit) → 1 or nil (0x0060d530): the two of one
// side (0x00729b30, the first asking); else one is "player" and the other
// the player's, a member or a member's pet (0x00512a30).
static int lua_UnitCanCooperate(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string u1(luaL_checkstring(L, 1)), u2(luaL_checkstring(L, 2));
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    const uint64_t g1 = resolveUnitGuid(gh, u1);
    const uint64_t g2 = resolveUnitGuid(gh, u2);
    const bool yes = (g1 != 0 && g2 != 0 && gh->unitsCooperate(g1, g2)) ||
                     (u1 == "player" && gh->isPlayerOrGroupUnit(g2)) ||
                     (u2 == "player" && gh->isPlayerOrGroupUnit(g1));
    if (!yes) { return luaReturnNil(L); }
    lua_pushnumber(L, 1);
    return 1;
}

// UnitCreatureFamily(unit) → familyName or nil
// UnitCreatureFamily(unit) (0x00611820) → CreatureFamily.dbc's localized
// Name for a creature's template family (0x007153e0), nil without one - a
// player has no template, and a creature of no family has none to name.
static int lua_UnitCreatureFamily(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string uidStr(luaL_checkstring(L, 1));
    toLowerInPlace(uidStr);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    auto entity = guid != 0 ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (!entity || entity->getType() == game::ObjectType::PLAYER) return luaReturnNil(L);
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) return luaReturnNil(L);
    const uint32_t family = gh->getCreatureFamily(unit->getEntry());
    if (family == 0) return luaReturnNil(L);
    if (const std::string name = game::creatureFamilyName(gh->services().assetManager, family); !name.empty()) {
        lua_pushstring(L, name.c_str());
        return 1;
    }
    // Without the file, the English names.
    static constexpr const char* kFamilies[] = {
        "", "Wolf", "Cat", "Spider", "Bear", "Boar", "Crocolisk", "Carrion Bird",
        "Crab", "Gorilla", "Raptor", "", "Tallstrider", "", "", "Felhunter",
        "Voidwalker", "Succubus", "", "Doomguard", "Scorpid", "Turtle", "",
        "Imp", "Bat", "Hyena", "Bird of Prey", "Wind Serpent", "", "Dragonhawk",
        "Ravager", "Warp Stalker", "Sporebat", "Nether Ray", "Serpent", "Moth",
        "Chimaera", "Devilsaur", "Ghoul", "Silithid", "Worm", "Rhino", "Wasp",
        "Core Hound", "Spirit Beast"
    };
    if (family >= std::size(kFamilies) || !kFamilies[family][0]) return luaReturnNil(L);
    lua_pushstring(L, kFamilies[family]);
    return 1;
}

// UnitOnTaxi(unit) → boolean (true if on a flight path)
static int lua_UnitOnTaxi(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitOnTaxi(\"unit\")");   // 0x0060f350
    // UNIT_FLAG_TAXI_FLIGHT (0x100000) on any unit.
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    if (!unit) { return luaReturnNil(L); }
    luaPushOneOrNil(L, (unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x00100000u) != 0);
    return 1;
}

// UnitSex(unit) → 1=unknown, 2=male, 3=female
static int lua_UnitSex(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // Gender is byte 2 of UNIT_FIELD_BYTES_0 (0=male, 1=female)
            uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
            uint8_t gender = static_cast<uint8_t>((bytes0 >> 16) & 0xFF);
            lua_pushnumber(L, gender == 0 ? 2 : (gender == 1 ? 3 : 1)); // WoW: 2=male, 3=female
            return 1;
        }
        // Out of sight, the name cache's gender (0x0060f8e0 -> 0x0067d770).
        if (const uint8_t gender = gh->lookupPlayerGender(guid); gender <= 1) {
            lua_pushnumber(L, gender == 0 ? 2 : 3);
            return 1;
        }
    }
    lua_pushnumber(L, 1); // unknown
    return 1;
}

// --- Player/Game API ---

/// One of the stat fields, as the realm wrote it. A field this expansion's
/// table does not name reads as nothing, which sends the caller to its cache.
static int32_t statField(game::Unit* unit, game::UF field) {
    const uint16_t idx = game::fieldIndex(field);
    if (!unit || idx == 0xFFFF) return 0;
    return static_cast<int32_t>(unit->getField(idx));
}

/// The three runs UnitStat reads, in the order the API's index counts them.
///
/// Spelled out rather than derived from UNIT_FIELD_STAT0 plus an offset,
/// because the runs are consecutive only in the WotLK layout: the other three
/// put the stats at 150 and 159, where 89 through 98 are some other field
/// entirely and an offset would read it with no complaint.
static constexpr game::UF kStatFields[5] = {game::UF::UNIT_FIELD_STAT0,
                                            game::UF::UNIT_FIELD_STAT1,
                                            game::UF::UNIT_FIELD_STAT2,
                                            game::UF::UNIT_FIELD_STAT3,
                                            game::UF::UNIT_FIELD_STAT4};
static constexpr game::UF kPosStatFields[5] = {game::UF::UNIT_FIELD_POSSTAT0,
                                               game::UF::UNIT_FIELD_POSSTAT1,
                                               game::UF::UNIT_FIELD_POSSTAT2,
                                               game::UF::UNIT_FIELD_POSSTAT3,
                                               game::UF::UNIT_FIELD_POSSTAT4};
static constexpr game::UF kNegStatFields[5] = {game::UF::UNIT_FIELD_NEGSTAT0,
                                               game::UF::UNIT_FIELD_NEGSTAT1,
                                               game::UF::UNIT_FIELD_NEGSTAT2,
                                               game::UF::UNIT_FIELD_NEGSTAT3,
                                               game::UF::UNIT_FIELD_NEGSTAT4};

/// UnitStat(unit, statIndex) → stat, effectiveStat, posBuff, negBuff.
///
/// The client answers the last three from the unit's own fields (0x00610300):
/// STAT0..4 as the realm wrote them, POSSTAT0..4 (89-93) and NEGSTAT0..4
/// (94-98). The second answer is the same stat dword with a negative clamped
/// away - 0x005774b0 is the `x & ((int)x < 0) - 1` mask, and it is the figure
/// the sheet prints while the first stays raw. WoWee had neither buff run, so it
/// pushed its total four times and the tooltip was the loser: paperdollframe.lua
/// builds "total (base +x -y)" by subtracting both halves out of the first
/// answer, colours the number red when a debuff is in play, and here always
/// found no halves to find. So STAT is the total, not the base.
///
/// The same gate as the attack-power bindings: the client answers for any unit
/// (0x00610300 tests OBJECT_FIELD_TYPE & TYPEMASK_UNIT, bit 3 of the type
/// field, not a unit flag) - a creature's fields simply arrive as zero, a
/// vehicle's or a charmed creature's do not. WoWee keeps its cached total for the two tokens it has one for
/// when the unit itself cannot be reached, which is what answers a pet whose
/// entity is out of hand; the cache holds only the total, so no buff shows.
static int lua_UnitStat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 4; }
    int statIdx = static_cast<int>(luaL_checknumber(L, 2)) - 1; // WoW API is 1-indexed
    // Whose stat. This ignored the unit and answered from the player, so the
    // paperdoll's pet tab listed the hunter's own Strength as the pet's.
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);

    int32_t stat = 0, positive = 0, negative = 0;
    bool fromUnit = false;
    if (statIdx >= 0 && statIdx < 5) {
        auto* unit = resolveUnit(L, who.c_str());
        if (unit) {
            stat = statField(unit, kStatFields[static_cast<size_t>(statIdx)]);
            positive = statField(unit, kPosStatFields[static_cast<size_t>(statIdx)]);
            negative = statField(unit, kNegStatFields[static_cast<size_t>(statIdx)]);
            fromUnit = true;
        }
    }
    if (!fromUnit) {
        if (who == "pet") {
            if (statIdx >= 0 && statIdx < 5) stat = gh->getPetStats()[static_cast<size_t>(statIdx)];
        } else if (who == "player") {
            stat = gh->getPlayerStat(statIdx);
        }
        // The cache is -1 until the realm says otherwise, and a stat the client
        // has no value for shows as nothing rather than as minus one.
        if (stat < 0) stat = 0;
    }
    lua_pushnumber(L, stat);      // raw, debuff and all
    lua_pushnumber(L, stat < 0 ? 0 : stat); // what the sheet prints
    lua_pushnumber(L, positive);  // the buff's positive half
    lua_pushnumber(L, negative);  // and its negative one
    return 4;
}

/// Armour and the six schools are one array of seven, and each school has two
/// more arrays beside it: the positive part at RESISTANCEBUFFMODSPOSITIVE, the
/// negative at …NEGATIVE. 0x004f54d0 is the whole of the arithmetic both
/// bindings use - effective = RESISTANCES as the realm wrote it, base = that
/// minus both parts, and only the two effective copies are clamped when the
/// total goes negative.
///
/// `withMods` is false for the one path that reads no parts at all:
/// UnitResistance (0x006101a0) asks 0x004f54d0 only about the local player, and
/// for any other player-controlled unit - a hunter's pet - it takes the
/// resistance dword, clamps it, and pushes it twice with two zeros beside it.
/// UnitArmor (0x00610ec0) has no such branch and takes the breakdown for
/// whoever asks. The two bindings disagree about the pet; that is the client's
/// own behaviour, so it is mirrored rather than tidied up.
struct ResistAnswer {
    int32_t base = 0, effective = 0, positive = 0, negative = 0;
};

static ResistAnswer resistanceAnswer(game::Unit* unit, int school, bool withMods) {
    const uint16_t resIdx = game::fieldIndex(game::UF::UNIT_FIELD_RESISTANCES);
    if (!unit || resIdx == 0xFFFF || school < 0 || school > 6) return {};
    const int32_t raw =
        static_cast<int32_t>(unit->getField(resIdx + static_cast<uint16_t>(school)));
    ResistAnswer a;
    if (!withMods) {
        a.base = a.effective = (raw < 0) ? 0 : raw;
        return a;
    }
    // A run read by offset rather than by fifteen names, because the client
    // names each run once; the table test pins both ends of it.
    const uint16_t posIdx = game::fieldIndex(game::UF::UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE);
    const uint16_t negIdx = game::fieldIndex(game::UF::UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE);
    if (posIdx != 0xFFFF) a.positive = static_cast<int32_t>(unit->getField(posIdx + static_cast<uint16_t>(school)));
    if (negIdx != 0xFFFF) a.negative = static_cast<int32_t>(unit->getField(negIdx + static_cast<uint16_t>(school)));
    a.base = raw - a.positive - a.negative;
    a.effective = (raw < 0) ? 0 : raw;
    return a;
}

/// UnitArmor(unit) → baseArmor, effectiveArmor, armor, posBuff, negBuff.
///
/// School zero of the block above, taken in full: the client answers for any
/// unit (OBJECT_FIELD_TYPE & TYPEMASK_UNIT) and lets 0x004f54d0 work out the
/// three arrays at index zero. paperdollframe.lua:430 hands all five answers to
/// PaperDollFormatStat, which colours the number by the parts and builds the
/// "base +pos -neg" tooltip beside it - WoWee pushed the effective figure three
/// times with no parts, so a geared character's armour line sat there white
/// with no breakdown, and the same went for the pet sheet's armour.
static int lua_UnitArmor(lua_State* L) {
    auto* gh = getGameHandler(L);
    // PaperDollFrame_SetArmor is shared between the sheets and the pet tab
    // calls it with "Pet" - capitalised, which is why this compares lowered.
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    ResistAnswer a;
    auto* unit = resolveUnit(L, who.c_str());
    if (unit) {
        a = resistanceAnswer(unit, 0, true);
    } else if (gh) {
        const int32_t armor = (who == "pet") ? gh->getPetResistances()[0] : gh->getArmorRating();
        a.base = a.effective = (armor < 0) ? 0 : armor;
    }
    lua_pushnumber(L, a.base);
    lua_pushnumber(L, a.effective);
    lua_pushnumber(L, a.effective);   // the compat slot, same figure again
    lua_pushnumber(L, a.positive);
    lua_pushnumber(L, a.negative);
    return 5;
}

/// UnitResistance(unit, school) → base, resistance, positive, negative.
///
/// The school is 0-based here, unlike UnitStat: the client checks
/// `if (6 < index)` with no subtraction, and armour is school zero of the same
/// array. Only the local player gets the parts (see resistanceAnswer); every
/// other unit gets the clamped dword twice with two zeros after it - the
/// client answers any unit (TYPEMASK_UNIT), and a creature's resistances
/// simply arrive as zero.
static int lua_UnitResistance(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int school = static_cast<int>(luaL_optnumber(L, 2, 0));
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);

    ResistAnswer a;
    bool answered = false;
    if (auto* unit = resolveUnit(L, who.c_str())) {
        const bool self = gh && unit->getGuid() == gh->getPlayerGuid();
        a = resistanceAnswer(unit, school, self);
        answered = true;
    }
    if (!answered && gh && school >= 0 && school <= 6) {
        // The cached figures, for a unit this cannot reach: the pet's own
        // block, or the player's - where the realm's armour dword is school
        // zero and the six schools follow it. Either way the cache holds only
        // the total, so no parts show.
        int32_t val = 0;
        if (who == "pet") val = gh->getPetResistances()[static_cast<size_t>(school)];
        else if (who == "player") val = (school == 0) ? gh->getArmorRating() : gh->getResistance(school);
        a.base = a.effective = (val < 0) ? 0 : val;
    }
    lua_pushnumber(L, a.base);
    lua_pushnumber(L, a.effective);
    lua_pushnumber(L, a.positive);
    lua_pushnumber(L, a.negative);
    return 4;
}

// GetDodgeChance() → percent
static int lua_GetDodgeChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getDodgePct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetParryChance() → percent
static int lua_GetParryChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getParryPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetBlockChance() → percent
static int lua_GetBlockChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getBlockPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetShieldBlock() → the damage a block takes off (0x0060dff0).  Straight out
// of PLAYER_SHIELD_BLOCK, +0xdec into the field blob the player object keeps at
// +0x1008; the sheet prints it as the last number of the block tooltip
// (paperdollframe.lua:479, next to GetBlockChance).  The client asks for the
// *player* object by the local player's guid and answers 0 when there is none,
// and it reads the dword unsigned - it adds 2^32 back when the int comes out
// negative - so a block value the server sent above 2^31 reads as the size of
// it, not as a negative.  Was a hardcoded zero here.
// The local player's object - what a client binding means by "the player":
// 0x004d3790's guid, asked for as a player (0x004d4db0 with type 0x10), which
// answers nothing when there is none and the binding then pushes zero.  The
// second half of the guard is ours, not the client's: an expansion whose layout
// does not name the field cannot be asked for it.
static std::shared_ptr<game::Entity> localPlayerObject(lua_State* L, uint16_t first,
                                                       uint16_t second = 0xFFFF) {
    if (first == 0xFFFF && second == 0xFFFF) return nullptr;
    auto* gh = getGameHandler(L);
    return gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
}

static int lua_GetShieldBlock(lua_State* L) {
    const uint16_t index = game::fieldIndex(game::UF::PLAYER_SHIELD_BLOCK);
    const auto player = localPlayerObject(L, index);
    lua_pushnumber(L, player ? player->getField(index) : 0);
    return 1;
}

// GetSpellPenetration() → what the player's gear takes off the target's
// resistances (0x0060e470).  Also a field, also summed by the server: the same
// player object's blob at +0x105c - dword 1047, PLAYER_FIELD_MOD_TARGET_
// RESISTANCE - pushed as its negation, because the field holds the resistance
// *change* and penetration is a decrease.  The sheet prints it as its own stat
// (paperdollframe.lua:988) and twice inside the spell-hit rating's tooltip
// (paperdollframe.lua:342).  Negated in double, so the field's most negative
// value cannot wrap the way the client's own int negation would.
static int lua_GetSpellPenetration(lua_State* L) {
    const uint16_t index = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_TARGET_RESISTANCE);
    const auto player = localPlayerObject(L, index);
    // Negated as the integer it is, so that none is 0 and not -0.
    lua_pushnumber(L, player ? static_cast<double>(-static_cast<int64_t>(static_cast<int32_t>(player->getField(index)))) : 0.0);
    return 1;
}

// The honour counters the PvP panel and the honour frame read (pvpframe.lua:508
// and honourframe.lua:21-41), each a player field the server sends already
// summed: GetPVPSessionStats (0x00611a20), GetPVPYesterdayStats (0x00611ad0)
// and GetPVPLifetimeStats (0x00611b80) all take the local player's player
// object and read it, and push zeros when there is no player at all.
//
// PLAYER_FIELD_KILLS is one dword carrying two 16-bit counters - this session's
// kills in the low half, yesterday's in the high - which is why the client
// reads it as a ushort twice, at +0x10d4 and at +0x10d6.  The contributions are
// whole dwords, unsigned: the client adds 2^32 back when the int it loaded came
// out negative, so the counters go past two billion rather than wrapping.
static int lua_GetPVPSessionStats(lua_State* L) {
    const uint16_t killsIndex = game::fieldIndex(game::UF::PLAYER_FIELD_KILLS);
    const auto player = localPlayerObject(L, killsIndex);
    const uint32_t kills = player ? player->getField(killsIndex) : 0;
    const uint16_t contributionIndex = game::fieldIndex(game::UF::PLAYER_FIELD_TODAY_CONTRIBUTION);
    const auto withContribution = localPlayerObject(L, contributionIndex);
    lua_pushnumber(L, kills & 0xFFFFu);
    lua_pushnumber(L, withContribution ? withContribution->getField(contributionIndex) : 0);
    return 2;
}

static int lua_GetPVPYesterdayStats(lua_State* L) {
    const uint16_t killsIndex = game::fieldIndex(game::UF::PLAYER_FIELD_KILLS);
    const auto player = localPlayerObject(L, killsIndex);
    const uint32_t kills = player ? player->getField(killsIndex) : 0;
    const uint16_t contributionIndex = game::fieldIndex(game::UF::PLAYER_FIELD_YESTERDAY_CONTRIBUTION);
    const auto withContribution = localPlayerObject(L, contributionIndex);
    lua_pushnumber(L, kills >> 16);
    lua_pushnumber(L, withContribution ? withContribution->getField(contributionIndex) : 0);
    return 2;
}

// GetPVPLifetimeStats answers two values, not three - the lifetime honorable
// kills, then the highest PvP rank (honorframe.lua:41 `hk, highestRank`) - and
// the second is a byte, not a dword: byte 3 of PLAYER_FIELD_BYTES (+0x1067),
// read as zero below 5. That floor is the client's, and it is what the honour
// frame's "Rank" line depends on.
static int lua_GetPVPLifetimeStats(lua_State* L) {
    const uint16_t killsIndex = game::fieldIndex(game::UF::PLAYER_FIELD_LIFETIME_HONORBALE_KILLS);
    const auto player = localPlayerObject(L, killsIndex);
    lua_pushnumber(L, player ? player->getField(killsIndex) : 0);
    const uint16_t bytesIndex = game::fieldIndex(game::UF::PLAYER_FIELD_BYTES);
    const auto withBytes = localPlayerObject(L, bytesIndex);
    const uint32_t dishonorable = withBytes ? (withBytes->getField(bytesIndex) >> 24) & 0xFFu : 0;
    lua_pushnumber(L, dishonorable < 5 ? 0 : dishonorable);
    return 2;
}

// GetCritChance() → percent (melee crit)
static int lua_GetCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetRangedCritChance() → percent
static int lua_GetRangedCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getRangedCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetSpellCritChance(school) → percent  (1=Physical,2=Holy,3=Fire,4=Nature,5=Frost,6=Shadow,7=Arcane)
// The client (0x0060e290) reads PLAYER_SPELL_CRIT_PERCENTAGE1 + (school - 1):
// the argument is one-based, the field run zero-based. Passing the school
// straight through read the next school's field and answered 0 for Arcane
// (7), which is past the run - and the character sheet shows the minimum
// over schools 2..7, so its spell crit read 0.00% for every caster.
static int lua_GetSpellCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    // 0x0060e290: the school is truncated from whatever the argument reads as,
    // and anything outside 1..7 - no argument among it - is the usage line.
    const int school = static_cast<int>(lua_tonumber(L, 1));
    if (school < 1 || school > 7) return luaL_error(L, "Usage: GetSpellCritChance(school)");
    // School 1 is physical, the first of the seven crit fields.
    float v = gh ? gh->getSpellCritPct(school - 1) : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCombatRating(ratingIndex) → value.
// The FrameXML CR_* constants are 1-based (CR_WEAPON_SKILL=1 … CR_ARMOR_PENETRATION=25),
// but the player-field array and the combat game tables are 0-based (AzerothCore's
// CombatRating enum, weapon skill at 0). Convert at the boundary or every rating reads
// its neighbour's slot.
static int lua_GetCombatRating(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetCombatRating(ratingIndex)");   // 0x0060ddb0
    if (const long r = std::lround(lua_tonumber(L, 1)); r < 1 || r > 25)
        return luaL_error(L, "ratingIndex is in the range 1 .. %d", 25);
    auto* gh = getGameHandler(L);
    int cr = static_cast<int>(std::lround(lua_tonumber(L, 1))) - 1;   // rounded (0x0060ddb0)
    int32_t v = gh ? gh->getCombatRating(cr) : 0;
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetSpellBonusDamage(school) → that school's spell damage bonus (0x0060e310).
// It is not one number, and the school is the index: the client checks the
// argument lands in 1..7 - the field run is seven wide, one per school in the
// order Physical, Holy, Fire, Nature, Frost, Shadow and Arcane, the argument
// less one indexing it (FrameXML's holySchool is 2) - and raises otherwise,
// returning nothing.  Then it adds
// **two** arrays read off the local player's player object (0x00578210 and
// 0x00578250): PLAYER_FIELD_MOD_DAMAGE_DONE_POS at +0xffc into the field blob
// the object keeps at +0x1008, and PLAYER_FIELD_MOD_DAMAGE_DONE_NEG at +0x1018.
// The second is a damage *reduction* and comes from the server already
// negative, so the sum goes below zero for a cursed or debuffed caster, and the
// client does not clamp that away.  wowee answered its cached maximum across the
// six magic schools, clamped at zero, whatever school was asked for - which is
// the same number for every row of the sheet's spell damage list.
static int lua_GetSpellBonusDamage(lua_State* L) {
    // 0x0060e310: the school truncated, 1..7, else its usage line.
    const int school = static_cast<int>(lua_tonumber(L, 1));
    if (school < 1 || school > 7) return luaL_error(L, "Usage: GetSpellBonusDamage(school)");
    const uint16_t posIndex = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS);
    const uint16_t negIndex = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_NEG);
    const auto player = localPlayerObject(L, posIndex, negIndex);
    int32_t total = 0;
    if (player) {
        if (posIndex != 0xFFFF)
            total += static_cast<int32_t>(player->getField(static_cast<uint16_t>(posIndex + school - 1)));
        if (negIndex != 0xFFFF)
            total += static_cast<int32_t>(player->getField(static_cast<uint16_t>(negIndex + school - 1)));
    }
    lua_pushnumber(L, total);
    return 1;
}

// GetSpellBonusHealing() → value (0x0060e3b0).  One dword, no school: the client
// reads PLAYER_FIELD_MOD_HEALING_DONE_POS straight out of the player object at
// +0x1050 - blob dword 1044 - with no second run to add the way the damage side
// has, and pushes it as a signed int, so a healing reduction shows as a negative
// row on the sheet (paperdollframe.lua:978).  The server has already summed the
// item mods into it; the only thing missing here was the clamp that turned a
// negative into a zero.
static int lua_GetSpellBonusHealing(lua_State* L) {
    const uint16_t index = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_HEALING_DONE_POS);
    const auto player = localPlayerObject(L, index);
    lua_pushnumber(L, player ? static_cast<int32_t>(player->getField(index)) : 0);
    return 1;
}

// GetAttackPowerForStat(stat, value) → the attack power that much of a stat
// gives the player (0x0060e560 -> 0x006d7070). Strength (1) gives two for
// each point past ten, or one for a class whose ChrClasses field 1 is 1
// (hunter, rogue, shaman); agility (2) gives one for each point past ten to
// those classes only, and all of it in a form with SpellShapeshiftForm flag
// 0x20 (cat, bear). Any other stat gives none.
static int lua_GetAttackPowerForStat(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int stat = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const int value = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh) return luaReturnZero(L);
    // ChrClasses field 1, by class id: 1 where agility is the class's
    // attack power.
    static std::unordered_map<uint32_t, uint32_t> agilityClass;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh->services().assetManager;
        if (am && am->isInitialized()) {
            loaded = true;
            if (auto dbc = am->loadDBCOptional("ChrClasses.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() > 1)
                for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
                    agilityClass[dbc->getUInt32(r, 0)] = dbc->getUInt32(r, 1);
        }
    }
    const auto it = agilityClass.find(gh->getPlayerClass());
    if (it == agilityClass.end()) return luaReturnZero(L);
    const bool agile = it->second == 1;
    const int past = value - 10 > 0 ? value - 10 : 0;
    int ap = 0;
    if (stat == 0) {
        ap = agile ? past : past * 2;
    } else if (stat == 1) {
        if (agile) ap = past;
        constexpr uint32_t kFormAgilityAttackPower = 0x20;
        if (const auto* row = gh->shapeshiftFormRow(gh->getShapeshiftFormId());
            row && (row->flags & kFormAgilityAttackPower))
            ap += value;
    }
    lua_pushnumber(L, ap);
    return 1;
}

// UnitRace(unit) (0x0060fd40) → the race's name in the unit's gender and its
// file name (ChrRaces ClientFileString), from the unit or, out of sight, the
// name cache; nil and nil when neither knows it.
static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    const uint8_t raceId = gh ? unitRaceOf(gh, uid) : 0;
    if (raceId == 0 || raceId >= 12) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const std::string localized = game::chrRaceName(gh->services().assetManager, raceId, unitIsFemale(gh, uid) ? 1 : 0).name;
    lua_pushstring(L, !localized.empty() ? localized.c_str() : kLuaRaces[raceId]);
    lua_pushstring(L, kLuaRaceFileNames[raceId]);   // spliced into asset paths
    return 2;
}

namespace {
/// Vehicle.dbc's Flags and PowerDisplayID[3] (fields 1 and 37-39), by id.
struct VehiclePowerRow { uint32_t flags = 0; std::array<uint32_t, 3> powerDisplay{}; };
const VehiclePowerRow* vehiclePowerRow(game::GameHandler* gh, uint32_t vehicleId) {
    static std::unordered_map<uint32_t, VehiclePowerRow> rows;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh ? gh->services().assetManager : nullptr;
        if (!am || !am->isInitialized()) return nullptr;
        loaded = true;
        if (auto dbc = am->loadDBCOptional("Vehicle.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 40)
            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r)
                rows[dbc->getUInt32(r, 0)] = {dbc->getUInt32(r, 1),
                                              {dbc->getUInt32(r, 37), dbc->getUInt32(r, 38), dbc->getUInt32(r, 39)}};
    }
    const auto it = rows.find(vehicleId);
    return it != rows.end() ? &it->second : nullptr;
}
}  // namespace
std::optional<PowerDisplayRow> powerDisplayRow(game::GameHandler* gh, uint32_t id) {
    auto* am = gh ? gh->services().assetManager : nullptr;
    auto dbc = am && am->isInitialized() ? am->loadDBCOptional("PowerDisplay.dbc") : nullptr;
    if (!dbc || !dbc->isLoaded() || dbc->getRecordSize() < 15) return std::nullopt;
    const int32_t row = dbc->findRecordById(id);
    if (row < 0) return std::nullopt;
    PowerDisplayRow out;
    out.actualType = dbc->getUInt32(static_cast<uint32_t>(row), 1);
    out.token = dbc->getString(static_cast<uint32_t>(row), 2);
    const uint8_t* rec = dbc->getRecord(static_cast<uint32_t>(row));
    for (int i = 0; i < 3; ++i) out.rgb[i] = rec[12 + i];
    return out;
}

// UnitPowerType(unit[, index]) (0x0060f100): the power type and its token.
// A visible unit in a vehicle whose row has flag 0x100000 shows its power the
// vehicle's way - the index-th of the row's PowerDisplay ids, when that
// display is of the unit's own type - and then answers five: type, the
// display's token and its colour. A unit known nowhere is 0 and "".
static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    const int index = lua_isnumber(L, 2) ? static_cast<int>(lua_tonumber(L, 2)) - 1 : 0;

    if (unit) {
        uint8_t pt = unit->getPowerType();
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        constexpr uint32_t kVehiclePowerDisplay = 0x100000;
        if (const auto* veh = guid ? vehiclePowerRow(gh, gh->vehicleRoster().vehicleId(guid)) : nullptr;
            veh && (veh->flags & kVehiclePowerDisplay) && index >= 0 && index < 3) {
            if (const auto shown = powerDisplayRow(gh, veh->powerDisplay[static_cast<size_t>(index)]);
                shown && shown->actualType == pt) {
                lua_pushnumber(L, shown->actualType);
                lua_pushstring(L, shown->token.c_str());
                for (uint8_t c : shown->rgb) lua_pushnumber(L, c / 255.0);
                return 5;
            }
        }
        // Any index but the first that the vehicle's display does not answer
        // is 0 and "" (0x0060f100).
        if (index != 0) {
            lua_pushnumber(L, 0);
            lua_pushstring(L, "");
            return 2;
        }
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    // Fallback: party member stats for out-of-range members
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const auto* pm = findPartyMember(gh, guid);
    // Not a member: a member's pet, its type at +0x58 of the record (0x0060f100).
    const auto* owner = pm ? nullptr : findPartyMemberPet(gh, guid);
    if ((pm || owner) && index == 0) {
        uint8_t pt = pm ? pm->powerType : owner->petPowerType;
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushstring(L, "");
    return 2;
}

static int lua_UnitGUID(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitGUID(\"unit\")");   // 0x0060e630
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    // A creature or vehicle whose template has type flag 0x4000 shows only
    // the top byte of its low word (0x0060e630), as the combat log hides it.
    const uint32_t high = static_cast<uint32_t>(guid >> 32);
    if ((high & 0xF0F00000u) == 0xF0300000u || (high & 0xF0F00000u) == 0xF0500000u) {
        const uint32_t entry = (static_cast<uint32_t>(guid) >> 24) | ((high & 0xFFFFFu) << 8);
        const auto& cache = gh->getCreatureInfoCache();
        const auto it = cache.find(entry);
        if (it != cache.end() && (it->second.typeFlags & 0x4000u))
            guid &= 0xFFFFFFFFFF000000ull;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)guid);
    lua_pushstring(L, buf);
    return 1;
}

/// UnitPVPName(unit) → the name with the player's title around it, or just
/// the name when there is no title.
///
/// The comment here used to say titles were not tracked. They are - the known
/// bits and the chosen one both come off the player - so the character sheet
/// showed a bare name for someone wearing a title.
///
/// Only for the player: a title is a bit index on the unit's own fields and
/// this client reads that for itself alone, so anyone else answers their name,
/// which is what WoW answers for a character wearing none.
static int lua_UnitPVPName(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitPVPName(\"unit\")");   // 0x0060e9a0
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (gh && uid) {
        std::string id(uid);
        toLowerInPlace(id);
        if (id == "player" && gh->getChosenTitleBit() > 0) {
            const std::string titled =
                gh->getFormattedTitle(static_cast<uint32_t>(gh->getChosenTitleBit()));
            if (!titled.empty()) { lua_pushstring(L, titled.c_str()); return 1; }
        }
    }
    // One value, the name, where UnitName gives the realm beside it.
    lua_UnitName(L);
    lua_pop(L, 1);
    return 1;
}

/// The unit named by the first argument, 0 for none.
static uint64_t namedUnitGuid(lua_State* L, game::GameHandler* gh) {
    const char* uid = lua_tostring(L, 1);
    if (!gh || !uid) return 0;
    std::string unit(uid);
    toLowerInPlace(unit);
    return resolveUnitGuid(gh, unit);
}

/// 0x0052c680.
static bool inPlayersParty(const game::GameHandler* gh, uint64_t guid) {
    if (guid == 0) return false;
    if (guid == gh->getPlayerGuid()) return true;
    for (int slot = 1; slot <= 4; ++slot) {
        if (game::partySlotGuid(gh->getPartyData(), gh->getPlayerGuid(), slot) == guid) return true;
    }
    return false;
}

/// 0x005726f0.
static bool inPlayersRaid(const game::GameHandler* gh, uint64_t guid) { return gh->raidRosterIndex(guid) != 0; }

// UnitIsPlayer(unit) (0x0060c4b0): a player in view, or one of the party or
// the raid wherever it is (0x00512a00).
static int lua_UnitIsPlayer(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = namedUnitGuid(L, gh);
    if (guid == 0) { return luaReturnNil(L); }
    const auto entity = gh->getEntityManager().getEntity(guid);
    luaPushOneOrNil(L, (entity && entity->getType() == game::ObjectType::PLAYER) || inPlayersParty(gh, guid) ||
                           inPlayersRaid(gh, guid));
    return 1;
}

// Vehicles, as ScriptEvents.cpp answers for a unit: its passenger state
// (+0xf60 +0x14) and the VehicleSeat row of the seat it is in (0x00613600:
// its transport's vehicle row's column for its seat byte).
static uint64_t resolveUnitArgGuid(lua_State* L, game::GameHandler* gh) {
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    return gh ? resolveUnitGuid(gh, uid) : 0;
}

/// A unit out of view: its record's status (0x00513c30 -> 0x0052bee0, the
/// party's stats, +8) says 0x200 while it sits in a vehicle - so 0x006133d0
/// and 0x006134a0 answer 1 for a member far away (found by W in code; it
/// answered nil for any unit out of view).
static bool outOfViewInVehicle(game::GameHandler* gh, uint64_t guid) {
    if (!gh || guid == 0 || gh->getEntityManager().getEntity(guid)) return false;
    for (const auto& m : gh->getPartyData().members)
        if (m.guid == guid) return (m.onlineStatus & 0x0200u) != 0;
    return false;
}

/// UnitHasVehicleUI(unit): the seat's Flags 0x20000000 (0x00613740).
static int lua_UnitHasVehicleUI(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const game::vehicle::SeatRecord* seat = gh && guid ? gh->vehicleRoster().seatOf(guid) : nullptr;
    lua_pushboolean(L, seat && (seat->flags & game::vehicle::kSeatFlagHasVehicleUI) != 0 ? 1 : 0);
    return 1;
}
/// The vehicle a unit's seats are counted from (0x0074c650 with none to
/// stop at): up its transports, or itself where it is a vehicle.
static uint64_t vehicleRootOf(game::GameHandler* gh, uint64_t guid, uint64_t stopAt = 0) {
    return gh && guid ? gh->vehicleRoster().rootVehicle(guid, stopAt, vehicleKnown(gh)) : 0;
}
/// UnitVehicleSeatCount(unit): the seats of the vehicle its seats count
/// from, 0 for none (0x00613830 -> 0x00757470).
static int lua_UnitVehicleSeatCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t root = vehicleRootOf(gh, resolveUnitArgGuid(L, gh));
    lua_pushnumber(L, root ? static_cast<double>(gh->vehicleRoster().uiSeatCount(root, vehicleIsPlayer(gh))) : 0.0);
    return 1;
}
/// UnitVehicleSeatInfo(unit, seat) -> controlType, occupantName, serverName,
/// ejectable, canSwitchSeats (0x006138c0 -> 0x007579e0): the seat-th of the
/// vehicle its seats count from. controlType is "Root" or "Child" for a
/// control seat (Flags 0x800) of that vehicle or of one riding it, else
/// "None"; the occupant's name or nil; no server name; FlagsB 0x20 and
/// Flags 0x4000000. Nothing for no such seat.
static int lua_UnitVehicleSeatInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    const uint64_t root = vehicleRootOf(gh, guid);
    if (!root || index < 1) return 0;
    const auto info = game::vehicle::uiSeatInfo(gh->vehicleRoster(), root, static_cast<uint32_t>(index - 1),
                                                vehicleIsPlayer(gh));
    if (!info) return 0;
    lua_pushstring(L, info->controlType);
    std::string name;
    if (info->occupant) {
        auto entity = gh->getEntityManager().getEntity(info->occupant);
        if (auto* unit = dynamic_cast<game::Unit*>(entity.get())) name = gh->shownUnitName(*unit);  // 0x0072a000
        if (name.empty()) name = gh->lookupName(info->occupant);
        if (name.empty()) name = "Unknown Being";
    }
    if (info->occupant) lua_pushstring(L, name.c_str());
    else lua_pushnil(L);
    lua_pushnil(L);
    lua_pushboolean(L, info->ejectable ? 1 : 0);
    lua_pushboolean(L, info->canSwitchSeats ? 1 : 0);
    return 5;
}
/// UnitSwitchToVehicleSeat(unit, seat) (0x006139b0 -> 0x0074ca90): for the
/// player only, in a passenger state; the seat-th of the vehicle its seats
/// count from, where it is empty and both it and the player's own seat have
/// Flags 0x4000000 - CMSG_REQUEST_VEHICLE_SWITCH_SEAT with that vehicle and
/// seat.
static int lua_UnitSwitchToVehicleSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || !guid || guid != gh->getPlayerGuid() || gh->vehicleRoster().seatState(guid) == 0) return 0;
    const auto p = gh->vehicleRoster().passenger(guid);
    const uint64_t root = p ? vehicleRootOf(gh, p->vehicleGuid) : 0;
    if (!root || index < 1) return 0;
    const auto& roster = gh->vehicleRoster();
    const auto at = roster.uiSeat(root, static_cast<uint32_t>(index - 1), vehicleIsPlayer(gh));
    if (!at || !roster.tables()) return 0;
    const game::vehicle::SeatRecord* seat = roster.tables()->seat(roster.vehicleId(at->vehicleGuid), at->seat);
    if (!seat || roster.occupant(at->vehicleGuid, at->seat) != 0 ||
        (seat->flags & game::vehicle::kSeatFlagCanSwitch) == 0 ||
        !playerVehicleSeatHasFlag(gh, game::vehicle::kSeatFlagCanSwitch))
        return 0;
    gh->sendRequestVehicleSwitchSeat(at->vehicleGuid, at->seat);
    return 0;
}
/// CanSwitchVehicleSeat(): whether the player's seat has Flags 0x4000000,
/// whatever is asked of it (0x00608580).
static int lua_CanSwitchVehicleSeat(lua_State* L) {
    lua_pushboolean(L, playerVehicleSeatHasFlag(getGameHandler(L), game::vehicle::kSeatFlagCanSwitch) ? 1 : 0);
    return 1;
}
/// GetVehicleUIIndicator(id) -> backgroundTexture, numSeats; nothing for
/// no such indicator (0x00614e60 -> 0x00614d90).
static const game::vehicle::UIIndicator* uiIndicator(lua_State* L, game::GameHandler* gh, int arg) {
    const auto* tables = gh ? gh->vehicleRoster().tables() : nullptr;
    return tables ? tables->indicator(static_cast<uint32_t>(lua_tonumber(L, arg))) : nullptr;
}
static int lua_GetVehicleUIIndicator(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetVehicleUIIndicator(indicatorID)");
    const auto* ind = uiIndicator(L, getGameHandler(L), 1);
    if (!ind) return 0;
    lua_pushstring(L, ind->backgroundTexture.c_str());
    lua_pushnumber(L, static_cast<double>(ind->seats.size()));
    return 2;
}
/// GetVehicleUIIndicatorSeat(id, i) -> virtualSeatIndex, xPos, yPos: the
/// indicator's i-th seat in id order (0x00614ef0).
static int lua_GetVehicleUIIndicatorSeat(lua_State* L) {
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
        return luaL_error(L, "Usage: GetVehicleUIIndicatorSeat(indicatorID, indicatorSeatIndex)");
    const auto* ind = uiIndicator(L, getGameHandler(L), 1);
    const auto i = static_cast<uint32_t>(static_cast<int>(lua_tonumber(L, 2)) - 1);
    if (!ind || i >= ind->seats.size()) return 0;
    lua_pushnumber(L, static_cast<double>(ind->seats[i].virtualSeatIndex));
    lua_pushnumber(L, ind->seats[i].x);
    lua_pushnumber(L, ind->seats[i].y);
    return 3;
}
/// The passenger in the player's seat-th of the vehicle its seats count
/// from, where the player carries it - is that vehicle, or one it rides on
/// (0x00613d20, 0x00613e10). The seat's row too.
struct EjectTarget {
    uint64_t occupant = 0;
    const game::vehicle::SeatRecord* seat = nullptr;
};
static std::optional<EjectTarget> ejectTarget(game::GameHandler* gh, int index) {
    if (!gh || index < 1) return std::nullopt;
    const uint64_t player = gh->getPlayerGuid();
    const uint64_t root = vehicleRootOf(gh, player);
    if (!root) return std::nullopt;
    const auto& roster = gh->vehicleRoster();
    const auto at = roster.uiSeat(root, static_cast<uint32_t>(index - 1), vehicleIsPlayer(gh));
    if (!at) return std::nullopt;
    EjectTarget out;
    out.occupant = roster.occupant(at->vehicleGuid, at->seat);
    if (!out.occupant) return std::nullopt;
    out.seat = roster.tables() ? roster.tables()->seat(roster.vehicleId(at->vehicleGuid), at->seat) : nullptr;
    return out;
}
/// CanEjectPassengerFromSeat(seat): the player carries its passenger and the
/// seat has FlagsB 0x20 (0x00613d20).
static int lua_CanEjectPassengerFromSeat(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CanEjectPassengerFromSeat(seatIndex)");
    auto* gh = getGameHandler(L);
    const auto t = ejectTarget(gh, static_cast<int>(lua_tonumber(L, 1)));
    const bool can = t && vehicleRootOf(gh, t->occupant, gh->getPlayerGuid()) == gh->getPlayerGuid() && t->seat &&
                     (t->seat->flagsB & game::vehicle::kSeatFlagBEjectable) != 0;
    lua_pushboolean(L, can ? 1 : 0);
    return 1;
}
/// EjectPassengerFromSeat(seat): CMSG_EJECT_PASSENGER for whoever sits
/// there; the server judges (0x00613e10 -> 0x00757200).
static int lua_EjectPassengerFromSeat(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: EjectPassengerFromSeat(seatIndex)");
    auto* gh = getGameHandler(L);
    if (const auto t = ejectTarget(gh, static_cast<int>(lua_tonumber(L, 1)))) gh->sendEjectPassenger(t->occupant);
    return 0;
}
/// UnitInVehicle(unit): 1 while seated (state 3), else nil (0x006133d0).
static int lua_UnitInVehicle(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    if (gh && guid && (gh->vehicleRoster().seatState(guid) == 3 || outOfViewInVehicle(gh, guid)))
        lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
/// UnitControllingVehicle(unit): seated, and charming the vehicle it is
/// seated in - its UNIT_FIELD_CHARM its transport (0x00613570).
static uint64_t guidField(game::GameHandler* gh, uint64_t unitGuid, game::UF field);
static int lua_UnitControllingVehicle(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    bool controlling = false;
    if (gh && guid && gh->vehicleRoster().seatState(guid) == 3) {
        if (const auto p = gh->vehicleRoster().passenger(guid)) {
            controlling = guidField(gh, guid, game::UF::UNIT_FIELD_CHARM) == p->vehicleGuid;
        }
    }
    lua_pushboolean(L, controlling ? 1 : 0);
    return 1;
}
/// UnitInVehicleControlSeat(unit): its seat's Flags 0x800 (0x00613700).
static int lua_UnitInVehicleControlSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const game::vehicle::SeatRecord* seat = gh && guid ? gh->vehicleRoster().seatOf(guid) : nullptr;
    lua_pushboolean(L, seat && (seat->flags & 0x800) != 0 ? 1 : 0);
    return 1;
}
/// UnitVehicleSkin(unit): its seat's UiSkin, "Natural" or "Mechanical",
/// "" past them, nothing without a seat (0x006137d0).
static int lua_UnitVehicleSkin(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const game::vehicle::SeatRecord* seat = gh && guid ? gh->vehicleRoster().seatOf(guid) : nullptr;
    if (!seat) return 0;
    static constexpr const char* kSkins[2] = {"Natural", "Mechanical"};
    lua_pushstring(L, seat->uiSkin < 2 ? kSkins[seat->uiSkin] : "");
    return 1;
}
/// The guid held in a two-word unit field, or zero.
///
/// Absent on an expansion whose table does not name the field, which is every
/// one but WotLK for these two - an index that is not known is not guessed at.
static uint64_t guidField(game::GameHandler* gh, uint64_t unitGuid, game::UF field) {
    if (!gh) return 0;
    const uint16_t idx = game::fieldIndex(field);
    if (idx == 0xFFFF) return 0;
    auto entity = gh->getEntityManager().getEntity(unitGuid);
    if (!entity) return 0;
    const uint64_t lo = entity->getField(idx);
    const uint64_t hi = entity->getField(static_cast<uint16_t>(idx + 1));
    return (hi << 32) | lo;
}

/// Whether this unit is driving something else.
///
/// UNIT_FIELD_CHARM names what it controls, and it was not read because the
/// field was not in the table. Both these answered a flat false and said so.
static int lua_UnitIsPossessed(lua_State* L) {
    // UNIT_FLAG_POSSESSED (0x01000000) on the unit (0x0060d860).
    auto* unit = lua_isstring(L, 1) ? resolveUnit(L, lua_tostring(L, 1)) : nullptr;
    luaPushOneOrNil(L, unit && (unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x01000000u));
    return 1;
}

/// Whether something else is driving this unit - the other end of the same
/// relationship, in UNIT_FIELD_CHARMEDBY.
///
/// Bound rather than left out because ToggleGameMenu reads it, and it used to
/// survive there only by a short circuit: `ClearTarget() and (not
/// UnitIsCharmed(...))` never reaches the second half because ClearTarget
/// returns nothing. That is a fact about a neighbouring binding's return
/// count, not about this one, and pressing Escape is not where anyone wants to
/// find that out.
static int lua_UnitIsCharmed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    luaPushOneOrNil(L, guidField(gh, guid, game::UF::UNIT_FIELD_CHARMEDBY) != 0);
    return 1;
}
/// Whether this unit is driving something (0x00613c90).
///
/// The pair it reads is UNIT_FIELD_CHARM, and when that pair is empty
/// UNIT_FIELD_SUMMON - the same charm-or-summon pair the reaction code and
/// lua_UnitPlayerOrPetInParty walk. A unit with either naming something answers
/// **1.0**; anything else answers nil, and false is not an answer this binding
/// gives. The argument has no default either: the client reads it as a string
/// that is nil when absent, which resolves to no unit and lands on that nil.
///
/// The three places it is reached from are the OnCancel of each resurrect
/// popup (staticpopup.lua:1205,1231,1255): declining a raise re-opens the death
/// dialog unless the player is controlling something - and a hunter whose pet is
/// still fighting is, as is a passenger, whose CHARM names the transport (see
/// the vehicle note above). Answering false here, as this row did, re-opened the
/// death dialog for exactly those players.
static int lua_UnitIsControlling(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    uint64_t driven = guidField(gh, guid, game::UF::UNIT_FIELD_CHARM);
    if (!driven) driven = guidField(gh, guid, game::UF::UNIT_FIELD_SUMMON);
    if (driven) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
static int lua_UnitIsTalking(lua_State* L) { return luaReturnNil(L); }
// UnitInBattleground(unit) (0x0060caa0) → the unit's place in the battle's
// scoreboard, from 0, while the player is in a battleground or an arena
// (instance type 3 or 4, 0x00572f50); nil otherwise.
static int lua_UnitInBattleground(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = lua_tostring(L, 1);
    if (!gh || !uid) return luaReturnNil(L);
    std::string unit(uid);
    toLowerInPlace(unit);
    const uint64_t guid = resolveUnitGuid(gh, unit);
    const int type = gh->currentMapInstanceType();
    const auto* board = gh->getBgScoreboard();
    if (guid == 0 || !board || (type != 3 && type != 4)) return luaReturnNil(L);
    for (size_t i = 0; i < board->players.size(); ++i) {
        if (board->players[i].guid != guid) continue;
        lua_pushnumber(L, static_cast<double>(i));
        return 1;
    }
    return luaReturnNil(L);
}

// UnitPlayerOrPetInParty(unit) (0x0060c9a0 -> 0x0052c8c0): one of the four in
// the party slots - not the player - or what one of them charms, else its
// summon; of one out of view the pet the group's stats name.
static int lua_UnitPlayerOrPetInParty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = namedUnitGuid(L, gh);
    if (guid == 0) { return luaReturnNil(L); }
    for (int slot = 1; slot <= 4; ++slot) {
        const uint64_t member = game::partySlotGuid(gh->getPartyData(), gh->getPlayerGuid(), slot);
        if (member != 0 && (member == guid || resolveUnitGuid(gh, "partypet" + std::to_string(slot)) == guid)) {
            lua_pushnumber(L, 1.0);
            return 1;
        }
    }
    return luaReturnNil(L);
}

// UnitPlayerOrPetInRaid(unit) (0x0060cb20 -> 0x00573200): the same of the
// raid's roster, the player among it.
static int lua_UnitPlayerOrPetInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = namedUnitGuid(L, gh);
    if (guid == 0 || !game::groupIsRaid(gh->getPartyData().groupType)) { return luaReturnNil(L); }
    for (int n = 1; n <= 40; ++n) {
        const std::string number = std::to_string(n);
        const uint64_t member = resolveUnitGuid(gh, "raid" + number);
        if (member != 0 && (member == guid || resolveUnitGuid(gh, "raidpet" + number) == guid)) {
            lua_pushnumber(L, 1.0);
            return 1;
        }
    }
    return luaReturnNil(L);
}

// UnitCharacterPoints(unit) → the two points fields, talent points then skill points
//
/// 0x00610fb0 answers nothing but two dwords of the player's own field blob:
/// 0xda0 and 0xda4, the pair the client's descriptor table names
/// PLAYER_CHARACTER_POINTS1 and PLAYER_CHARACTER_POINTS2 and 0x005e8470 puts
/// one eight-byte watch over (see the watcher in entity_controller, which is
/// the only thing in the client that raises CHARACTER_POINTS_CHANGED). Both are
/// read through the extended-field pointer at object+0x1008 as **signed** ints -
/// `*(int *)` - and pushed as numbers, and the loop runs `0xda0 → < 0xda8`, so
/// there are always exactly two values once the argument test is passed.
///
/// Two tests decide whether either is anything but 0: the unit has to be
/// player-typed - `*(object+8 dword 2) >> 4 & 1`, OBJECT_FIELD_TYPE's 0x10 bit,
/// the same test 0x005ce540 makes through Player_C.h directly - and its guid,
/// the qword at the head of that same object blob, has to be the local player's.
/// A player who is not the local player fails the second test and reads 0 for
/// each value on its own; anything else fails the first and skips the loop, and
/// both answers are the same two zeroes.
///
/// With no argument at all the client prints "Usage: UnitCharacterPoints(
/// \"unit\")" and leaves **no** values on the stack, as with the other unit
/// calls that have no default.
///
/// WoWee answered this from two of its own tallies: the talent store the talent
/// packet fills, and a count of primary professions still learnable (SkillLine
/// .category 11 against a limit of two). The second is not a field at all, and
/// the first is the wrong one of two stores the client keeps apart - the packet
/// feeds its talent frame, the fields feed this call. Both readers of the
/// second value inside WoWee already disagreed with each other:
/// GameHandler::getSkillPoints() (game_handler_skill_ups.cpp:22) answers that
/// same 0xda4 dword, and GetAdjustedSkillPoints - 0x005ce540, which reads the
/// field here and subtracts the pending trainer cost - subtracts the pending
/// cost from *that*, so the skill window's bar and the number beside it came
/// from the field while the call above them came from a tally. What the
/// profession limit was wanted for is blizzard_trainerui.lua:29,
/// `if ( cp2 < MAX_LEARNABLE_PROFESSIONS )` picking between
/// PROFESSION_CONFIRMATION1 and 2; MAX_LEARNABLE_PROFESSIONS is 2, and the
/// tally sat in the same range as a field the realm sends, which is why the
/// confirmation never looked wrong. chatframe.lua:2607 is the other reader of
/// the second value, printing LEVEL_UP_SKILL_POINTS with it after the event
/// says points were gained - the field again, and the same one the watcher
/// named in its second argument.
static int lua_UnitCharacterPoints(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return 0;  // the client's usage line: no values at all
    auto* gh = getGameHandler(L);

    uint32_t talentPoints = 0;
    uint32_t skillPoints = 0;
    if (gh) {
        std::string uid(name);
        toLowerInPlace(uid);
        const uint64_t who = resolveUnitGuid(gh, uid);
        // The local player, and player-typed: both tests, one answer.  A name
        // that resolves to another player gets past the first and fails the
        // second, which is the same pair of zeroes either way.
        if (who != 0 && who == gh->getPlayerGuid()) {
            if (auto* unit = resolveUnit(L, name)) {
                if (const uint16_t idx = game::fieldIndex(game::UF::PLAYER_CHARACTER_POINTS1);
                    idx != 0xFFFF)
                    talentPoints = unit->getField(idx);
                if (const uint16_t idx = game::fieldIndex(game::UF::PLAYER_CHARACTER_POINTS2);
                    idx != 0xFFFF)
                    skillPoints = unit->getField(idx);
            }
        }
    }
    // Signed, as the client reads them: a negative points value is a real
    // number there, not a wrapped one.
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<int32_t>(talentPoints)));
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<int32_t>(skillPoints)));
    return 2;
}

static int lua_PetHasActionBar(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && gh->petHasActionBar());
    return 1;
}
static int lua_PetCanBeDismissed(lua_State* L) {
    // 0x005d3820: 1, except for the player's own summon whose creature
    // template has type flag 0x20.
    auto* gh = getGameHandler(L);
    const auto pet = gh && gh->hasPet() ? gh->getEntityManager().getEntity(gh->getPetGuid()) : nullptr;
    bool can = true;
    if (pet && pet->getType() == game::ObjectType::UNIT) {
        const auto* unit = static_cast<const game::Unit*>(pet.get());
        const uint16_t lo = game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_LO);
        const uint16_t hi = game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_HI);
        const uint64_t summoner = (lo != 0xFFFF && hi != 0xFFFF)
            ? (static_cast<uint64_t>(pet->getField(hi)) << 32) | pet->getField(lo) : 0;
        if (summoner == gh->getPlayerGuid()) {
            const auto& cache = gh->getCreatureInfoCache();
            const auto it = cache.find(unit->getEntry());
            if (it != cache.end() && (it->second.typeFlags & 0x20u)) can = false;
        }
    }
    luaPushOneOrNil(L, can);
    return 1;
}
// Abandoning is a hunter's pet, and nothing here knows which kind it has yet.
// PetCanBeAbandoned() (0x005d3780): 1 for a pet the player summoned
// (UNIT_FIELD_SUMMONEDBY) whose UNIT_FIELD_BYTES_2 third byte carries
// UNIT_CAN_BE_ABANDONED (0x2); nil otherwise.
static int lua_PetCanBeAbandoned(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto pet = gh && gh->getPetGuid() ? gh->getEntityManager().getEntity(gh->getPetGuid()) : nullptr;
    if (pet) {
        const uint16_t lo = game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_LO);
        const uint16_t hi = game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_HI);
        const uint16_t b2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
        const uint64_t summoner = lo == 0xFFFF || hi == 0xFFFF ? 0
            : pet->getField(lo) | (static_cast<uint64_t>(pet->getField(hi)) << 32);
        const uint32_t bytes2 = b2 == 0xFFFF ? 0 : pet->getField(b2);
        if (summoner == gh->getPlayerGuid() && ((bytes2 >> 16) & 0x2u) != 0) {
            lua_pushnumber(L, 1);
            return 1;
        }
    }
    return luaReturnNil(L);
}

// InCombatLockdown() (0x00511cc0): 1 while CGGameUI +0x124c is clear, nil
// otherwise. Not the same question as being in combat: the lockdown starts
// only once PLAYER_REGEN_DISABLED has been heard and ends before
// PLAYER_REGEN_ENABLED goes out (0x00524600, see LuaEngine::fireEvent).
static int lua_InCombatLockdown(lua_State* L) {
    if (taint::combatLockdown()) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

static int lua_IsMounted(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && gh->isMounted());
    return 1;
}

static int lua_IsFlying(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && gh->isPlayerFlying());
    return 1;
}

static int lua_IsSwimming(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && gh->isSwimming());
    return 1;
}

static int lua_IsResting(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && gh->isPlayerResting());
    return 1;
}

// IsFalling() (0x00612430) -> 1 while the player falls, else nil: the
// FALLING flag (0x1000) without ROOT (0x800). It tested 0x2000, FALLINGFAR,
// which a jump never sets, so it answered false through every jump and fall
// this client made.
static int lua_IsFalling(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && game::isFallingForInterface(gh->getMovementInfo().flags)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}

/// IsStealthed() (0x00612610): the player's own UNIT_FIELD_BYTES_1 third byte,
/// 0x2. The client names no spell and takes no unit argument - it asserts the
/// local player (Player_C.h 0xa0) and asks the field, because stealth is the
/// server's to say: 0x0073c830 watches that same bit. Reading it here rather
/// than matching Stealth (1784), Prowl (5215) and their ranks against the
/// player's auras also keeps the answer the server's: a stealth the server
/// took away by the field is gone here the moment it is gone, and a spell this
/// list did not know - any disguise the list never carried - did not have to
/// be added to it. 1 or nil, as the client's binding returns.
static int lua_IsStealthed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const game::Unit* player = gh ? gh->getUnitByGuid(gh->getPlayerGuid()) : nullptr;
    const uint8_t visibility = player ? player->getVisibilityFlags() : 0;
    if ((visibility & 0x2) != 0) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

static int lua_GetUnitSpeed(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetUnitSpeed(\"unit\")");   // 0x00613290
    // The unit's current speed from its movement state, 0 standing - any
    // unit (CMovement::GetCurrentSpeed, +0x8c).
    auto* gh = getGameHandler(L);
    std::string uid(lua_tostring(L, 1));
    toLowerInPlace(uid);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    const auto e = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (!e) { lua_pushnumber(L, 0); return 1; }
    uint32_t flags = e->getReportedMoveFlags();
    game::MovementSpeeds speeds = e->getMovementSpeeds();
    if (guid == gh->getPlayerGuid()) {
        flags = gh->getMovementInfo().flags;
        speeds.run = gh->getServerRunSpeed();
        speeds.walk = gh->getServerWalkSpeed();
        speeds.runBack = gh->getServerRunBackSpeed();
        speeds.swim = gh->getServerSwimSpeed();
        speeds.flight = gh->getServerFlightSpeed();
    }
    lua_pushnumber(L, game::Entity::speedForFlags(speeds, flags));
    return 1;
}

// --- Container/Bag API ---
// WoW bags: container 0 = backpack (16 slots), containers 1-4 = equipped bags


static int lua_UnitXP(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitXP(\"unit\")");   // 0x0060ea60
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") lua_pushnumber(L, gh->getPlayerXp());
    else lua_pushnumber(L, 0);
    return 1;
}

static int lua_UnitXPMax(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitXPMax(\"unit\")");   // 0x0060eae0
    // The active player's PLAYER_NEXT_LEVEL_XP as it is (0 at the cap), 0 for
    // any other unit (0x0060a600) - T389.
    auto* gh = getGameHandler(L);
    std::string u(lua_tostring(L, 1));
    toLowerInPlace(u);
    const bool me = gh && gh->getPlayerGuid() != 0 && resolveUnitGuid(gh, u) == gh->getPlayerGuid();
    lua_pushnumber(L, me ? gh->getPlayerNextLevelXp() : 0);
    return 1;
}

/// An Exhaustion.dbc row (the table at 0x00ad3838): its name, localized,
/// and its experience factor.
struct ExhaustionRow { bool found = false; std::string name; float factor = 1.0f; };
static ExhaustionRow exhaustionRow(game::GameHandler* gh, int id) {
    static std::unordered_map<int, ExhaustionRow> rows;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh ? gh->services().assetManager : nullptr;
        if (am && am->isInitialized()) {
            loaded = true;
            if (auto dbc = am->loadDBC("Exhaustion.dbc"); dbc && dbc->isLoaded()) {
                // ID, XP, Factor, OutdoorHours, InnHours, Name_lang.
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    ExhaustionRow r;
                    r.found = true;
                    r.factor = dbc->getFloat(i, 2);
                    r.name = dbc->getString(i, 5);
                    rows[static_cast<int>(dbc->getUInt32(i, 0))] = std::move(r);
                }
            }
        }
    }
    auto it = rows.find(id);
    return it == rows.end() ? ExhaustionRow{} : it->second;
}

/// GetXPExhaustion() (0x0051b960): only while the rest state is rested (1),
/// the rested-experience field times the rested row's factor; nil otherwise.
static int lua_GetXPExhaustion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || gh->getPlayerRestState() != 1) { return luaReturnNil(L); }
    const ExhaustionRow rested = exhaustionRow(gh, 1);
    lua_pushnumber(L, static_cast<double>(gh->getPlayerRestedXp()) *
                          static_cast<double>(rested.found ? rested.factor : 1.0f));
    return 1;
}

/// GetWatchedFactionInfo() → name, standingID, barMin, barMax, barValue.
///
/// The reputation bar under the experience bar, which draws nothing at all
/// without this.
///
/// Five answers, and nil plus four zeros when nothing is watched: that is what
/// 0x005d1240 pushes on both paths, and reputationframe.lua:324 names five.
///
/// Which faction that is comes from the realm - PLAYER_FIELD_WATCHED_FACTION
/// INDEX holds a reputation list index, which the client bounds to 0..0x7f and
/// turns into a faction, so the choice survives a relog and travels with the
/// character.  It is not a local selection, and nothing here picks one.
///
/// The bounds are the rank's own, not the whole scale: WoW's bar fills from
/// the bottom of the current rank to the top of it, so a character halfway
/// through Honored shows a half-full bar rather than one two-thirds along.
/// Those bounds are the client's own nine at 0x00a2d2fc - -42000, -6000, -3000,
/// 0, 3000, 9000, 21000, 42000, 43000 - read as min[i] and max[i] of one array,
/// which is why the two look four bytes apart.
static int lua_GetWatchedFactionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t factionId = gh ? gh->getWatchedFactionId() : 0u;
    int32_t standing = 0;
    bool watched = false;
    if (gh && factionId != 0) {
        const auto& standings = gh->getFactionStandings();
        if (auto it = standings.find(factionId); it != standings.end()) {
            standing = it->second;
            watched = true;
        }
    }
    if (!watched) {
        lua_pushnil(L);
        for (int i = 0; i < 4; ++i) lua_pushnumber(L, 0);
        return 5;
    }

    // Blizzard's own thresholds, and the standing id that goes with each. Hated
    // is 1 and Exalted is 8, which is what the interface indexes FACTION_BAR_
    // COLORS and the standing names by.
    static const struct { int32_t min, max; } kRanks[8] = {
        {-42000, -6000}, {-6000, -3000}, {-3000, 0}, {0, 3000},
        {3000, 9000}, {9000, 21000}, {21000, 42000}, {42000, 43000},
    };
    int rank = 7;
    for (int i = 0; i < 8; ++i) {
        if (standing < kRanks[i].max) { rank = i; break; }
    }

    lua_pushstring(L, gh->getFactionNamePublic(factionId).c_str());
    lua_pushnumber(L, rank + 1);
    lua_pushnumber(L, kRanks[rank].min);
    lua_pushnumber(L, kRanks[rank].max);
    lua_pushnumber(L, standing);
    // Five, as the client's are: the faction id is not one of its answers, and
    // the interface gets the id it asked for back from GetSelectedFaction.
    return 5;
}

/// GetNumBagSlots() → the four bags beside the backpack. A constant in every
/// expansion this client speaks, but the interface reads it rather than
/// assuming, and arithmetic on nil is what it gets otherwise.
static int lua_GetNumBagSlots(lua_State* L) {
    lua_pushnumber(L, 4);
    return 1;
}

/// GetMirrorTimerProgress(timer) → the timer's value now, in milliseconds.
///
/// It answered a constant zero, and MirrorTimerFrame_OnUpdate divides it by a
/// thousand and calls SetValue with the result on every frame - so the breath
/// bar was reset to empty as fast as it could be drawn. An empty frame with a
/// label was the whole of what it could ever show.
///
/// Named, not numbered: WoW passes the timer's name through the event and
/// FrameXML hands the same string back here.
static int lua_GetMirrorTimerProgress(lua_State* L) {
    // 0x00517aa0, whose usage line names GetMirrorTimerInfo. The name is
    // matched whole and in any case (0x00513e40).
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetMirrorTimerInfo(\"timer\")");
    const char* name = lua_tostring(L, 1);
    int type = 0;
    while (type < 3 && !sameTypeName(name, game::GameHandler::kMirrorTimerNames[type])) ++type;
    if (type >= 3) return luaL_error(L, "ERROR: Invalid timer (Ex: \"BREATH\"");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    // (now - start) * scale + value, unclamped and with no regard for a pause.
    const auto& t = gh->getMirrorTimer(type);
    lua_pushnumber(L, static_cast<double>(t.progress(std::llround(luaGetTimeNow() * 1000.0))));
    return 1;
}

/// GetTimeToWellRested() → seconds until fully rested, or nil.
///
/// Confirmed called rather than guessed at: it turned up in the missing-API
/// report from a real session, read by the experience bar's tooltip. Rest
/// accrual is not modelled, and nil is what WoW itself answers when there is
/// no timer to report - which is the branch the tooltip already handles.
static int lua_GetTimeToWellRested(lua_State* L) {
    return luaReturnNil(L);
}


// ── Character sheet ────────────────────────────────────────────────────────
//
// PaperDollFrame reads about forty functions and does arithmetic with nearly
// all of them, so a missing one is not a blank field - it is "attempt to
// perform arithmetic on a nil value" and the rest of the panel never draws.
// The server sends most of what these want; where it does not, the honest
// answer is a zero of the right shape rather than nothing.

/// The three attack-power answers the client works out for itself (0x00610b60
/// melee, 0x00610ca0 ranged): each one is (multiplier + 1) times a field, with
/// the mods dword split into its two signed halves for the positive and the
/// negative buff. Rounded to a whole number, because that is what the client
/// casts to before pushing. A mod or multiplier field the expansion does not
/// name reads as no mod, which leaves the base alone.
struct AttackPowerAnswer {
    long long base = 0, positive = 0, negative = 0;
};
static AttackPowerAnswer attackPowerAnswer(game::Unit* unit, game::UF baseField,
                                           game::UF modsField, game::UF multField) {
    const uint16_t baseIdx = game::fieldIndex(baseField);
    const uint16_t modsIdx = game::fieldIndex(modsField);
    const uint16_t multIdx = game::fieldIndex(multField);
    if (!unit || baseIdx == 0xFFFF) return {};
    const uint32_t mods = (modsIdx == 0xFFFF) ? 0u : unit->getField(modsIdx);
    const double scale = (multIdx == 0xFFFF)
                             ? 1.0
                             : static_cast<double>(std::bit_cast<float>(unit->getField(multIdx))) + 1.0;
    return {
        std::llround(scale * static_cast<int32_t>(unit->getField(baseIdx))),
        std::llround(scale * static_cast<int16_t>(mods & 0xFFFF)),
        std::llround(scale * static_cast<int16_t>(mods >> 16)),
    };
}
static int pushAttackPower(lua_State* L, const AttackPowerAnswer& a) {
    lua_pushnumber(L, static_cast<double>(a.base));
    lua_pushnumber(L, static_cast<double>(a.positive));
    lua_pushnumber(L, static_cast<double>(a.negative));
    return 3;
}

/// UnitAttackPower(unit) → base, positive buff, negative buff.
///
/// Three fields answer this and the client does the arithmetic itself:
/// 0x00610b60 takes the attack power at 123, the mods dword at 124 - one field
/// holding two signed halves, positive low and negative high - and a float
/// multiplier at 125, then multiplies all three answers by (1 + multiplier).
/// WoWee had the base alone, so it claimed there were no buffs when the realm
/// had just named them, and an aura that writes the multiplier field - the
/// fraction-of-attack-power kind rather than the flat kind - left the sheet
/// quoting the unscaled figure.
///
/// The client answers for any unit: 0x00610b60 tests OBJECT_FIELD_TYPE &
/// TYPEMASK_UNIT (bit 3 of the object's type field), not a unit flag, and a
/// creature's attack power simply arrives as zero. PaperDollFrame_SetAttackPower is
/// shared and the pet tab calls it with "pet", which is how the pet's line came
/// to print its owner's attack power - the binding knew only the player's
/// number and the handler's cached pet figure, and the cache is left here as
/// the answer for a pet whose entity this cannot reach.
static int lua_UnitAttackPower(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitAttackPower(\"unit\")");   // 0x00610b60
    auto* gh = getGameHandler(L);
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    auto* unit = resolveUnit(L, who.c_str());
    if (unit) {
        return pushAttackPower(L, attackPowerAnswer(unit, game::UF::UNIT_FIELD_ATTACK_POWER,
                                                    game::UF::UNIT_FIELD_ATTACK_POWER_MODS,
                                                    game::UF::UNIT_FIELD_ATTACK_POWER_MULTIPLIER));
    }
    const int32_t ap = who == "pet" ? (gh ? gh->getPetAttackPower() : 0)
                                    : (gh ? gh->getMeleeAttackPower() : 0);
    return pushAttackPower(L, {ap > 0 ? ap : 0, 0, 0});
}

/// UnitRangedAttackPower(unit) → base, positive buff, negative buff. The ranged
/// twin (0x00610ca0), reading 126, 127 and 128 the same way. It used to ignore
/// its argument altogether and answer the player's ranged attack power for
/// whoever asked, including the pet - the pet sheet registers
/// UNIT_RANGED_ATTACK_POWER and asks for "pet".
static int lua_UnitRangedAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    auto* unit = resolveUnit(L, who.c_str());
    if (unit) {
        return pushAttackPower(L, attackPowerAnswer(
                                      unit, game::UF::UNIT_FIELD_RANGED_ATTACK_POWER,
                                      game::UF::UNIT_FIELD_RANGED_ATTACK_POWER_MODS,
                                      game::UF::UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER));
    }
    const int32_t ap = gh ? gh->getRangedAttackPower() : 0;
    return pushAttackPower(L, {ap > 0 ? ap : 0, 0, 0});
}

/// UnitDefense(unit) → base, modifier. Base defense skill is five per level -
/// the cap a character sits at in practice, which is all the server lets us
/// infer since it does not send the trained skill. The modifier is the defense
/// skill granted by defense rating: AzerothCore takes int32(GetRatingBonusValue(
/// CR_DEFENSE_SKILL)) and each point is worth 0.04% avoidance. It was a flat
/// zero, so a geared tank's defense showed only the level cap with no sign of
/// the defence rating that pushes it to the crit-immunity threshold.
/// The skill line a weapon of this subclass trains (unarmed with none), and
/// a skill's value and modifier with a negative total clamped to zero
/// (0x0071b740's readers for UnitAttackBothHands, UnitRangedAttack and
/// UnitDefense).
static uint32_t weaponSkillLine(const game::ItemQueryResponseData* weapon) {
    if (!weapon || weapon->itemClass != 2) return 162;   // unarmed
    switch (weapon->subClass) {
        case 0: return 44;   case 1: return 172;  case 2: return 45;   case 3: return 46;
        case 4: return 54;   case 5: return 160;  case 6: return 229;  case 7: return 43;
        case 8: return 55;   case 10: return 136; case 13: return 473; case 15: return 173;
        case 16: return 176; case 18: return 226; case 19: return 228;
        default: return 162;
    }
}
static void pushSkillPair(lua_State* L, game::GameHandler* gh, uint32_t line) {
    const auto& skills = gh->getPlayerSkills();
    const auto it = skills.find(line);
    int base = it != skills.end() ? it->second.value : 0;
    int mod = it != skills.end() ? it->second.bonusTemp + it->second.bonusPerm : 0;
    if (base + mod < 0) mod = -base;
    lua_pushnumber(L, base);
    lua_pushnumber(L, mod);
}
/// The skill readers answer for the player only; anyone else is zeros.
static bool skillUnitIsPlayer(lua_State* L, game::GameHandler* gh) {
    std::string uid(lua_tostring(L, 1));
    toLowerInPlace(uid);
    return gh && resolveUnitGuid(gh, uid) == gh->getPlayerGuid() && gh->getPlayerGuid() != 0;
}
static const game::ItemQueryResponseData* wornWeapon(game::GameHandler* gh, game::EquipSlot slot) {
    const auto& s = gh->getInventory().getEquipSlot(slot);
    return s.empty() ? nullptr : gh->getItemInfo(s.item.itemId);
}

static int lua_UnitDefense(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitDefense(\"unit\")");   // 0x00610de0
    auto* gh = getGameHandler(L);
    if (!skillUnitIsPlayer(L, gh)) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    pushSkillPair(L, gh, 95);   // Defense
    return 2;
}

/// UnitAttackSpeed(unit) → mainHandSpeed, offHandSpeed, both in seconds.
///
/// The client answers this from the unit's own attack-time fields, not from a
/// weapon it may not be carrying: 0x00610a00 reads the blob's speed trio -
/// main hand +0xe0, off hand +0xe4, ranged +0xe8 - each a millisecond count it
/// divides by 1000, and the ranged one is UNIT_FIELD_RANGEDATTACKTIME, which
/// WoWee already reads at 64 for a ranged spell's category cooldown. Across
/// the unit fields the blob runs six dwords behind the wire index (UnitLevel
/// is 0x0060f9e0 reading +0xc0 for LEVEL's 54; MINDAMAGE's 70 and MAXDAMAGE's
/// 71 sit at +0x100 and +0x104), so the pair asked for here is fields 62 and
/// 63. Every unit carries them, which is what a cat form's 1.0 s and a pet's
/// own swing time look like; a weapon's delay is only the number the realm
/// wrote the field from, and answering from the weapon left every unit but the
/// player - the pet row of the pet sheet above all - on a bare-handed 2.0 s.
///
/// The off-hand speed (0x00610a00, ScriptEvents.cpp:0xb42) is answered only
/// for a player whose inventory has an item in the off-hand slot (its 17th,
/// +0x18f4 + 0x80) of item class 2, a weapon (0x00707220) - and for the active
/// player only once it knows the Dual Wield spell (0x006ceb00 tests
/// DAT_00c9eac8, which learning a spell with effect 40 sets, 0x006e7d24).
/// Another player's inventory is not this client's to read, so it answers nil
/// for them, and for every creature and pet. The field alone is no answer:
/// the realms write OFFHANDATTACKTIME for every unit (2000 for a player with
/// nothing in that hand), and answering from it gave every character a
/// "x.xx / 2.00" speed and an off-hand damage line. PaperDollFrame branches on
/// `if ( offhandSpeed )`. A main hand whose field never arrived falls back to
/// the weapon's delay rather than the client's 0.0: the three earlier layouts
/// do not place this pair in their tables, and a 0.0 there would divide every
/// damage-per-second figure on the sheet by nothing.
static int lua_UnitAttackSpeed(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitAttackSpeed(\"unit\")");   // 0x00610a00
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    const uint16_t mainIdx = game::fieldIndex(game::UF::UNIT_FIELD_BASEATTACKTIME);
    const uint16_t offIdx = game::fieldIndex(game::UF::UNIT_FIELD_OFFHANDATTACKTIME);
    double mainSpeed = 0.0;
    if (unit && mainIdx != 0xFFFF) mainSpeed = unit->getField(mainIdx) / 1000.0;

    // The off hand: the active player's, with a weapon there and Dual Wield.
    const bool activePlayer = gh && unit && unit->getGuid() == gh->getPlayerGuid();
    bool hasOffHand = false;
    double offSpeed = 0.0;
    if (activePlayer && gh->playerCanDualWield()) {
        const auto& oh = gh->getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
        constexpr uint32_t kItemClassWeapon = 2;
        const auto* info = oh.empty() ? nullptr : gh->getItemInfo(oh.item.itemId);
        if (info && info->itemClass == kItemClassWeapon) {
            hasOffHand = true;
            if (offIdx != 0xFFFF) offSpeed = unit->getField(offIdx) / 1000.0;
            if (offSpeed == 0.0) offSpeed = oh.item.delayMs > 0 ? oh.item.delayMs / 1000.0 : 2.0;
        }
    }
    if (mainSpeed == 0.0) {
        // Nothing wrote the field: the equipped item's delay, and bare hands
        // when there is no item. Always the player's own - no other unit has
        // an inventory here.
        mainSpeed = 2.0;
        if (activePlayer) {
            const auto& mh = gh->getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            if (!mh.empty() && mh.item.delayMs > 0) mainSpeed = mh.item.delayMs / 1000.0;
        }
    }
    lua_pushnumber(L, mainSpeed);
    if (hasOffHand) lua_pushnumber(L, offSpeed);
    else            lua_pushnil(L);
    return 2;
}

/// A damage field is float bits, and the client pushes them with no conversion
/// at all (0x00610860 takes +0x100 of the field blob and hands it straight to
/// Lua), so decode them here rather than giving the number a second home. A
/// field this expansion's table does not name reads as no damage, which is what
/// sends the callers below to their fallbacks.
static double fieldAsFloat(game::Unit* unit, game::UF field) {
    const uint16_t idx = game::fieldIndex(field);
    if (!unit || idx == 0xFFFF) return 0.0;
    return static_cast<double>(std::bit_cast<float>(unit->getField(idx)));
}

/// UnitDamage(unit) → min, max, off-hand min, off-hand max, positive bonus,
/// negative bonus, percent.
///
/// The client pushes the unit's own four damage fields and adds nothing to
/// them: 0x00610860 reads the blob's +0x100 through +0x10c - MINDAMAGE's 70
/// and MAXDAMAGE's 71, then the off-hand pair at 72 and 73 - and passes them
/// to Lua as float bits. The realm has already folded the weapon's damage and
/// the unit's attack power into that pair, which is why a hunter's pet, a cat
/// form and a geared warrior all answer from the same four reads. Working the
/// figure out of the equipment instead, as this did, could only ever be right
/// for the player: it modelled attack power as damage per second divided by
/// fourteen times the swing, took half of it for the off hand, and left every
/// other unit on the sheet - the pet tab above all, which asks for nothing but
/// the pet - showing bare hands.
///
/// The last three are the physical-school bonuses. The client answers them only
/// for a unit whose guid's high dword has bit 4 set - the one that lets it
/// reach into the unit's player extension, since the branch dereferences
/// `unit + 0x1008` and then checks the unit really is the local player before
/// using what it finds there - and from 0x00578210, 0x00578250 and
/// 0x00578290, accessors of a
/// figure the client keeps for its own character sheet rather than of an update
/// field, and one WoWee does not build. Everyone else gets 0, 0 and 1, which is
/// also what the client gives them; the cost of not answering them for the
/// player is a tooltip breakdown line, not the range itself, since the range
/// already carries whatever the realm folded in.
static int lua_UnitDamage(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitDamage(\"unit\")");   // 0x00610860
    auto* gh = getGameHandler(L);
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    auto* unit = resolveUnit(L, who.c_str());
    double lo = fieldAsFloat(unit, game::UF::UNIT_FIELD_MINDAMAGE);
    double hi = fieldAsFloat(unit, game::UF::UNIT_FIELD_MAXDAMAGE);
    double offLo = fieldAsFloat(unit, game::UF::UNIT_FIELD_MINOFFHANDDAMAGE);
    double offHi = fieldAsFloat(unit, game::UF::UNIT_FIELD_MAXOFFHANDDAMAGE);
    if (lo == 0.0 && hi == 0.0) {
        // Nothing wrote the pair. What this answered with before the fields
        // were named: the pet's own pair, which entity_controller keeps from
        // the realm's block even when the pet's entity is not one this can
        // reach, and otherwise the player's weapon plus the attack power the
        // realm would have folded in.
        if (who == "pet" && gh) {
            lo = gh->getPetMinDamage();
            hi = gh->getPetMaxDamage();
        } else {
            const int32_t ap = gh ? gh->getMeleeAttackPower() : -1;
            lo = 1.0;
            hi = 2.0;              // bare hands
            double speed = 2.0;
            if (gh && who == "player") {
                const auto& inv = gh->getInventory();
                const auto& mh = inv.getEquipSlot(game::EquipSlot::MAIN_HAND);
                if (!mh.empty() && mh.item.damageMax > 0.0f) {
                    lo = mh.item.damageMin;
                    hi = mh.item.damageMax;
                    if (mh.item.delayMs > 0) speed = mh.item.delayMs / 1000.0;
                }
                const auto& oh = inv.getEquipSlot(game::EquipSlot::OFF_HAND);
                if (!oh.empty() && oh.item.damageMax > 0.0f) {
                    offLo = oh.item.damageMin;
                    offHi = oh.item.damageMax;
                }
            }
            const double apSwing = (ap > 0) ? (ap / 14.0) * speed : 0.0;
            lo += apSwing;
            hi += apSwing;
            offLo = offLo > 0.0 ? offLo + apSwing / 2.0 : 0.0;
            offHi = offHi > 0.0 ? offHi + apSwing / 2.0 : 0.0;
        }
    }
    // The last three answers are fields as well, for the one unit the server
    // sends them for.  0x00610860 branches on the guid's player-extension bit and
    // then reads PLAYER_FIELD_MOD_DAMAGE_DONE_POS, _NEG and _PCT at index 0 - the
    // school pushed is a literal 0, `6a 00` in the code bytes, which is what
    // Ghidra's output drops from those three calls - through 0x00578210,
    // 0x00578250 and 0x00578290, each of which answers zero unless the unit it
    // runs on is the local player.  So the player gets its own three fields and
    // every other unit the no-bonus answers, which is what wowee answered for the
    // player too.  The one place the two differ is a pet: its damage-modifier
    // fields are the player's and never sent for it, so the client
    // hands back a percent of 0.0 and the sheet then divides the damage bounds by
    // it (paperdollframe.lua:544).  wowee keeps 1.0 there rather than reproducing
    // a divide by zero for a pet the realm has never sent a damage modifier for.
    double pos = 0.0, neg = 0.0, percent = 1.0;
    if (gh && unit && unit->getGuid() == gh->getPlayerGuid()) {
        const uint16_t posIndex = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS);
        const uint16_t negIndex = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_NEG);
        if (posIndex != 0xFFFF) pos = static_cast<int32_t>(unit->getField(posIndex));
        if (negIndex != 0xFFFF) neg = static_cast<int32_t>(unit->getField(negIndex));
        // A layout that does not name the multiplier keeps the sheet's 1.0, and
        // so does a field not yet sent: the client would hand back 0.0 there,
        // and PaperDollFrame_SetDamage divides the bounds by it
        // (paperdollframe.lua:544-545) - an inf on the sheet for nothing the
        // realm said. Every realm sends 1.0 for it.
        const uint16_t pctIndex = game::fieldIndex(game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT);
        if (pctIndex != 0xFFFF && unit->hasField(pctIndex))
            percent = fieldAsFloat(unit, game::UF::PLAYER_FIELD_MOD_DAMAGE_DONE_PCT);
        if (!(percent > 0.0)) percent = 1.0;
    }
    lua_pushnumber(L, lo);      // minDamage
    lua_pushnumber(L, hi);      // maxDamage
    lua_pushnumber(L, offLo);   // off-hand min
    lua_pushnumber(L, offHi);   // off-hand max
    lua_pushnumber(L, pos);     // physical bonus, positive
    lua_pushnumber(L, neg);     // ...and negative
    lua_pushnumber(L, percent); // ...and the multiplier the sheet divides by
    return 7;
}

/// UnitRangedDamage(unit) → speed, min, max, positive, negative, percent.
///
/// The same shape as UnitDamage one field group along: 0x00610550 pushes the
/// blob's ranged attack time at +0xe8 - UNIT_FIELD_RANGEDATTACKTIME, which
/// WoWee already reads at 64 for a ranged spell's category cooldown - divided
/// by 1000, then the ranged damage pair at +0x1ec and +0x1f0, fields 129 and
/// 130, as float bits. So the bow's own damage and the shot's speed both come
/// from the realm, and this answered neither: it named a bow's nominal 2.8 s
/// for every unit that asked and built the range out of attack power alone, so
/// a hunter's ranged row showed no weapon damage at all and a pet that asked
/// was handed a swing speed it never had.
///
/// The three bonuses are the per-school PLAYER_FIELD_MOD_DAMAGE_DONE_* run
/// again, indexed by the school of the shot's spell - which is the part of the
/// client's answer that needs the shot itself, and is not modelled here. They
/// stay the client's own no-bonus answers, 0, 0 and 1, as they are for every
/// unit but the active player (TYPEMASK_PLAYER and the active player's own
/// fields).
static int lua_UnitRangedDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string who(luaL_optstring(L, 1, "player"));
    toLowerInPlace(who);
    auto* unit = resolveUnit(L, who.c_str());
    const uint16_t speedIdx = game::fieldIndex(game::UF::UNIT_FIELD_RANGEDATTACKTIME);
    const double speed = (unit && speedIdx != 0xFFFF)
                             ? unit->getField(speedIdx) / 1000.0
                             : 0.0;
    double lo = fieldAsFloat(unit, game::UF::UNIT_FIELD_MINRANGEDDAMAGE);
    double hi = fieldAsFloat(unit, game::UF::UNIT_FIELD_MAXRANGEDDAMAGE);
    if (lo == 0.0 && hi == 0.0) {
        // Nothing wrote the pair, so fall back to what this did before the
        // fields were named - attack power over a bow's nominal speed. The
        // speed goes with the range rather than the realm's field because the
        // two are read together everywhere the sheet uses them, and a range
        // invented from a 2.8 s swing against a field speed would print a
        // damage-per-second figure matching neither.
        const double swingSpeed = speed > 0.0 ? speed : 2.8;
        const int32_t ap = gh ? gh->getRangedAttackPower() : -1;
        const double swing = (ap > 0) ? (ap / 14.0) * swingSpeed : 0.0;
        lo = hi = swing;
    }
    lua_pushnumber(L, speed);
    lua_pushnumber(L, lo);
    lua_pushnumber(L, hi);
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 1.0);
    return 6;
}

/// UnitRangedAttack(unit) → skill, modifier. As with defense, five per level.
static int lua_UnitRangedAttack(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitRangedAttack(\"unit\")");   // 0x006107d0
    auto* gh = getGameHandler(L);
    if (!skillUnitIsPlayer(L, gh)) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    const auto* ranged = wornWeapon(gh, game::EquipSlot::RANGED);
    if (!ranged || ranged->itemClass != 2) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    pushSkillPair(L, gh, weaponSkillLine(ranged));
    return 2;
}

/// UnitAttackBothHands(unit) → main skill, main modifier, off skill, off
/// modifier.
static int lua_UnitAttackBothHands(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitAttackBothHands(\"unit\")");   // 0x00610450
    auto* gh = getGameHandler(L);
    if (!skillUnitIsPlayer(L, gh)) { for (int i = 0; i < 4; ++i) lua_pushnumber(L, 0); return 4; }
    pushSkillPair(L, gh, weaponSkillLine(wornWeapon(gh, game::EquipSlot::MAIN_HAND)));
    pushSkillPair(L, gh, weaponSkillLine(wornWeapon(gh, game::EquipSlot::OFF_HAND)));
    return 4;
}

/// GetManaRegen() → while not casting, while casting. Both per second; the
/// paperdoll multiplies by five to show mana-per-5-seconds. The server sends
/// both figures already computed (UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER and its
/// interrupted twin), so this reads them rather than modelling the spirit and
/// intellect formula - it was a flat zero, so every caster's mana regen read 0.
static int lua_GetManaRegen(lua_State* L) {
    // 0x00612a90: 0, 0 unless the player's power is mana; each plus 0.001.
    auto* gh = getGameHandler(L);
    const auto me = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    const bool mana = me && ((me->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) >> 24) & 0xFFu) == 0;
    lua_pushnumber(L, mana ? gh->getManaRegen() + 0.001 : 0.0);
    lua_pushnumber(L, mana ? gh->getManaRegenCasting() + 0.001 : 0.0);
    return 2;
}

/// Whether this expansion's field table can answer the pet question at all.
///
/// Every table here names UNIT_FIELD_PETNUMBER but cataclysm's, and a layout
/// that does not cannot say whether a unit is anybody's pet. The two gates below
/// ask this first and keep the answer these bindings gave before, rather than a
/// "no" nothing in that layout supports - which matters most for the hunter leg,
/// since hunterPetOf reads that same field and answers false when it has nowhere
/// to read it from.
static bool petFieldsNamed() {
    return game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER) != 0xFFFF;
}

/// The gate the three modifier bindings share: the unit is the local player, or
/// it carries UNIT_FIELD_PETNUMBER. Anything else answers **zero**, not the
/// neutral one.
///
/// The client tests this identically in all three - 0x00612870, 0x006127f0 and
/// 0x00612900 each read `FUN_004cee50(unit) || blob+0x114 != 0` before they
/// consult anything. The first half is a guid compare: the eight bytes at the
/// head of the object record against the current-player slot in thread-local
/// storage. The second is petnumber, which is also why a warlock's voidwalker
/// and a death knight's ghoul - neither numbered as a pet - get zero, and the
/// pet paperdoll's stat tooltip for them says the stat changes nothing.
static bool modifierTarget(game::GameHandler* gh, const game::Unit* unit) {
    if (!unit) return false;
    if (gh && unit->getGuid() == gh->getPlayerGuid()) return true;
    if (!petFieldsNamed()) return true;
    return unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER)) != 0;
}

/// What the stats of the unit asked about give, where it is the player or a pet
/// with a number (the gate above), else 0: the character sheet's stat tooltips,
/// and the pet's (0x0060e130, 0x0060e1b0, 0x00612980, 0x00612a00).
template <typename Of>
static float ofUnitStats(lua_State* L, Of&& of) {
    auto* gh = getGameHandler(L);
    const auto* unit = gh ? resolveUnit(L, lua_tostring(L, 1)) : nullptr;
    if (!modifierTarget(gh, unit)) return 0.0f;
    return of(*gh, unit->getGuid() == gh->getPlayerGuid() ? gh->playerStats() : game::GameHandler::statsOf(*unit));
}

// GetCritChanceFromAgility(unit) → the melee crit percent its agility gives.
static int lua_GetCritChanceFromAgility(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetCritChanceFromAgility(\"unit\")");   // 0x0060e130
    lua_pushnumber(L, ofUnitStats(L, [](const auto& gh, const auto& s) { return gh.getMeleeCritFromAgility(s); }));
    return 1;
}

// GetSpellCritChanceFromIntellect(unit) → the spell crit percent its intellect
// gives.
static int lua_GetSpellCritChanceFromIntellect(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetSpellCritChanceFromIntellect(\"unit\")");   // 0x0060e1b0
    lua_pushnumber(L, ofUnitStats(L, [](const auto& gh, const auto& s) { return gh.getSpellCritFromIntellect(s); }));
    return 1;
}

// GetCombatRatingBonus(ratingIndex) → the percent that combat rating grants.
// The paperdoll reads it for dodge, parry, haste, defense, weapon skill and the
// other rating-based stats; it was a flat zero for all of them.
static int lua_GetCombatRatingBonus(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetCombatRatingBonus(ratingIndex)");   // 0x0060de70
    if (const long r = std::lround(lua_tonumber(L, 1)); r < 1 || r > 25)
        return luaL_error(L, "ratingIndex is in the range 1 .. %d", 25);
    auto* gh = getGameHandler(L);
    // Same 1-based CR_* → 0-based table convention as GetCombatRating above,
    // the index rounded (0x0060de70).
    const int cr = static_cast<int>(std::lround(lua_tonumber(L, 1))) - 1;
    lua_pushnumber(L, gh ? gh->getCombatRatingBonus(cr) : 0.0);
    return 1;
}

// GetArmorPenetration() → the percent of the target's armour the player ignores.
// The server stores armour penetration as a rating in the combat-rating field
// (Player::UpdateArmorPenetration writes PLAYER_FIELD_COMBAT_RATING_1 +
// CR_ARMOR_PENETRATION), and the paperdoll shows the rating's converted percent
// beside it - so this is the CR_ARMOR_PENETRATION rating bonus, index 24 in the
// 0-based internal scheme. Takes no unit; the flyout only asks about the player.
static int lua_GetArmorPenetration(lua_State* L) {
    auto* gh = getGameHandler(L);
    constexpr int kCrArmorPenetration = 24;  // 0-based CombatRating enum
    // No more than all of it (0x0060e4e0).
    lua_pushnumber(L, gh ? std::min<double>(gh->getCombatRatingBonus(kCrArmorPenetration), 100.0) : 0.0);
    return 1;
}

// Health / mana regenerated per second from Spirit - the pair the Spirit stat
// flyout shows, the mana with 0.001 to it where there is any unit to ask.
static int lua_GetUnitHealthRegenRateFromSpirit(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetUnitHealthRegenRateFromSpirit(\"unit\")");   // 0x00612980
    lua_pushnumber(L, ofUnitStats(L, [](const auto& gh, const auto& s) { return gh.getHealthRegenFromSpirit(s); }));
    return 1;
}
static int lua_GetUnitManaRegenRateFromSpirit(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetUnitManaRegenRateFromSpirit(\"unit\")");   // 0x00612a00
    lua_pushnumber(L, ofUnitStats(L, [](const auto& gh, const auto& s) { return gh.getManaRegenFromSpirit(s) + 0.001f; }));
    return 1;
}

/// GetUnitHealthModifier and GetUnitPowerModifier (0x006127f0, 0x00612900):
/// one for a hunter's pet, else what the template of its creature makes of it,
/// zero for every unit the gate does not admit. The local player has no
/// creature: zero.
static double petStatModifier(game::GameHandler* gh, const game::Unit* unit, bool power) {
    if (!petFieldsNamed()) return 1.0;
    if (!gh || !modifierTarget(gh, unit)) return 0.0;
    const auto& creatures = gh->getCreatureInfoCache();
    const auto it = creatures.find(unit->getEntry());
    const bool known = it != creatures.end();
    return game::templateModifier(hunterPetOf(gh, unit), known,
                                  !known ? 0.0f : power ? it->second.powerModifier : it->second.healthModifier);
}

/// GetUnitMaxHealthModifier(unit) → the multiplier on maximum health.
///
/// The unit's own UNIT_FIELD_MAXHEALTHMODIFIER as float bits, plus one:
/// 0x00612870 is `*(float *)(field blob + 0x22c) + 1.0`, and the client's field
/// metadata names the blob dword at 0x22c / 4 = 139 UNIT_FIELD_MAXHEALTHMODIFIER
/// (the descriptor record at .data+0xac6698, whose second dword is the blob
/// index; the enum comment works that blob dword into wire 145 and shows the
/// contiguous run it sits at the top of). Whether a 3.3.5 realm writes the field
/// at all is not settled here, and a field that never arrives reads zero, which
/// is the one this binding used to answer outright - so what changes is a realm
/// that does send one, and the character sheet's stamina line
/// (paperdollframe.lua:280) multiplies by whatever comes back.
///
/// Zero for a unit the gate excludes, which is what the client pushes there.
static int lua_GetUnitMaxHealthModifier(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, luaL_optstring(L, 1, "player"));
    if (!modifierTarget(gh, unit)) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    lua_pushnumber(L, fieldAsFloat(unit, game::UF::UNIT_FIELD_MAXHEALTHMODIFIER) + 1.0);
    return 1;
}

/// GetInventoryItemCooldown(unit, slot) → start, duration, enable
/// (0x005e7e60): the player's item there, its use spell or its used
/// enchantment's as cast from it (0x008090c0), 0, 0, 0 with neither or an
/// empty slot.
static int lua_GetInventoryItemCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (gh && slot >= 1 && slot <= 19) {
        const auto& sl = gh->getInventory().getEquipSlot(
            static_cast<game::EquipSlot>(slot - 1));
        if (!sl.empty()) return pushItemCopyCooldown(L, gh, sl.item.itemId, sl.item.guid, /*noneIsOff=*/true);
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}

/// A portrait texture with a file in it, "" to leave it empty, and no unit's
/// face.
static void portraitFile(wowee::ui::WidgetTree* tree, uint32_t id, const std::string& file, bool disc = false) {
    tree->setPortraitUnit(id, std::string());
    auto* w = tree->get(id);
    if (!w) return;
    w->solidColor = false;
    w->texturePath = file;
    w->portraitDisc = disc;
    w->texCoord[0] = w->texCoord[2] = 0.0f;
    w->texCoord[1] = w->texCoord[3] = 1.0f;
    w->texCoordRotated = false;
}

/// The unit's face on the texture. The image is remembered rather than filled
/// here: its handle is rebuilt whenever the portrait's render target is, so it
/// is assigned every frame, which the render loop does for every claim.
static void portraitFace(wowee::ui::WidgetTree* tree, game::GameHandler* gh, uint32_t id, uint64_t guid) {
    namespace claims = wowee::ui::portrait_claims;
    if (auto* w = tree->get(id)) {
        w->solidColor = false;
        w->texturePath.clear();
        w->portraitDisc = false;
    }
    tree->setPortraitUnit(id, guid == gh->getPlayerGuid() ? std::string(claims::kPlayer) : claims::unitKey(guid));
}

/// CreatureDisplayInfo's PortraitTextureName (0x00715950): the icon a display
/// is shown as instead of a face - a totem, a spore, a wisp. "" for most.
static std::string displayPortraitIcon(game::GameHandler* gh, uint32_t displayId) {
    static std::unordered_map<uint32_t, std::string> icons;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh->services().assetManager;
        if (!am || !am->isInitialized()) return {};
        loaded = true;
        const auto* layouts = pipeline::getActiveDBCLayout();
        const auto* l = layouts ? layouts->getLayout("CreatureDisplayInfo") : nullptr;
        const uint32_t col = l ? l->tryField("PortraitTextureName") : 0xFFFFFFFFu;
        auto dbc = am->loadDBC("CreatureDisplayInfo.dbc");
        if (dbc && dbc->isLoaded() && col < dbc->getFieldCount()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                std::string icon = dbc->getString(i, col);
                if (!icon.empty()) icons[dbc->getUInt32(i, 0)] = std::move(icon);
            }
        }
    }
    const auto it = icons.find(displayId);
    return it != icons.end() ? it->second : std::string();
}

/// CreatureDisplayInfo.CreatureModelAlpha (+0x14), of 255.
static uint32_t displayModelAlpha(game::GameHandler* gh, uint32_t displayId) {
    static std::unordered_map<uint32_t, uint32_t> alphas;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh->services().assetManager;
        if (!am || !am->isInitialized()) return 255;
        loaded = true;
        constexpr uint32_t kAlpha = 5;
        auto dbc = am->loadDBC("CreatureDisplayInfo.dbc");
        if (dbc && dbc->isLoaded() && kAlpha < dbc->getFieldCount()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                if (const uint32_t alpha = dbc->getUInt32(i, kAlpha); alpha < 255) alphas[dbc->getUInt32(i, 0)] = alpha;
        }
    }
    const auto it = alphas.find(displayId);
    return it != alphas.end() ? it->second : 255;
}

void takeUnitAlpha(game::GameHandler* gh, ui::Widget& frame) {
    if (!gh || !frame.visible) return;
    const auto entity = gh->getEntityManager().getEntity(frame.modelUnit);
    if (!entity || !entity->isUnit()) return;
    const uint32_t display = std::static_pointer_cast<game::Unit>(entity)->getDisplayId();
    frame.alpha = static_cast<float>(displayModelAlpha(gh, display)) / 255.0f;
}

/// SetLootPortrait(texture) (0x00588f00): the face of whoever the open loot is
/// from (0x00bfa8d8) on the texture, and 1; with no such unit the texture is
/// emptied (0x00481520) and nil.
static int lua_SetLootPortrait(lua_State* L) {
    auto* tree = getWidgetTree(L);
    auto* gh = getGameHandler(L);
    const uint32_t id = calledWidget(L);
    if (!tree || id == 0) { lua_pushnil(L); return 1; }
    const uint64_t guid = gh && gh->isLootWindowOpen() ? gh->getCurrentLoot().lootGuid : 0;
    if (guid != 0 && gh->getEntityManager().getEntity(guid)) {
        portraitFace(tree, gh, id, guid);
        lua_pushnumber(L, 1);
        return 1;
    }
    portraitFile(tree, id, std::string());
    lua_pushnil(L);
    return 1;
}

/// SetPortraitTexture(texture, "unit") (0x006113e0) -> whether there was one.
///
/// The face of the unit the token names now, and it stays that unit's until
/// the texture is given another. A display with an icon for a portrait shows
/// that. A player the client has no object for - a party or raid member out
/// of sight - gets the race's stand-in face (0x00618180):
/// Interface\\CharacterFrame\\TemporaryPortrait-<Male|Female>-<race>, the race
/// named by ChrRaces' ClientFileString and both read off the name cache's
/// record; without one the texture is emptied and the record asked for, whose
/// answer raises UNIT_PORTRAIT_UPDATE for the unit (0x00608540 ->
/// 0x00618110). A group member's pet out of sight gets TemporaryPortrait-Pet
/// (0x00513cb0 -> 0x00618230). Anything else leaves the texture empty.
static int lua_SetPortraitTexture(lua_State* L) {
    auto* tree = getWidgetTree(L);
    auto* gh = getGameHandler(L);
    const uint32_t id = calledWidget(L);
    if (!lua_isstring(L, 2)) return luaL_error(L, "Usage: SetPortraitTexture(texture, \"unit\")");
    if (!tree || id == 0) return 0;
    std::string unit(lua_tostring(L, 2));
    toLowerInPlace(unit);
    const uint64_t guid = gh ? resolveUnitGuid(gh, unit) : 0;
    const auto answer = [L](bool there) {
        lua_pushboolean(L, there ? 1 : 0);
        return 1;
    };
    const auto entity = guid != 0 ? gh->getEntityManager().getEntity(guid) : nullptr;
    if (entity || (guid != 0 && guid == gh->getPlayerGuid())) {
        if (guid != gh->getPlayerGuid() && !entity->isUnit()) {
            portraitFile(tree, id, std::string());
            return answer(false);
        }
        if (entity) {
            const auto shown = std::static_pointer_cast<game::Unit>(entity);
            if (const std::string icon = displayPortraitIcon(gh, shown->getDisplayId()); !icon.empty()) {
                portraitFile(tree, id, "Interface\\Icons\\" + icon, /*disc=*/true);
                return answer(true);
            }
        }
        portraitFace(tree, gh, id, guid);
        return answer(true);
    }
    const bool playerGuid = guid != 0 && ((guid >> 32) & 0xF0000000u) == 0;
    if (!playerGuid) {
        bool pet = false;
        if (guid != 0) {
            for (const auto& token : gh->unitTokensForGuid(guid)) pet = pet || token.find("pet") != std::string::npos;
        }
        portraitFile(tree, id, pet ? "Interface\\CharacterFrame\\TemporaryPortrait-Pet" : "");
        return answer(pet);
    }
    portraitFile(tree, id, std::string());
    const uint8_t race = gh->lookupPlayerRace(guid);
    const uint8_t gender = gh->lookupPlayerGender(guid);
    if (race == 0 || gender > 1) {
        gh->askNameThen(guid, "portrait", [gh, guid](bool found, const std::string&) {
            if (!found) return;
            for (const auto& token : gh->unitTokensForGuid(guid))
                gh->fireAddonEvent("UNIT_PORTRAIT_UPDATE", {token});
        });
        return answer(false);
    }
    auto* am = gh->services().assetManager;
    auto races = am && am->isInitialized() ? am->loadDBC("ChrRaces.dbc") : nullptr;
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* l = layouts ? layouts->getLayout("ChrRaces") : nullptr;
    const uint32_t fileCol = l ? l->tryField("ClientFileString") : 0xFFFFFFFFu;
    const int32_t row = races && races->isLoaded() ? races->findRecordById(race) : -1;
    if (row >= 0 && fileCol != 0xFFFFFFFFu) {
        portraitFile(tree, id,
                     std::string("Interface\\CharacterFrame\\TemporaryPortrait-") + (gender == 1 ? "Female" : "Male") +
                         "-" + races->getString(static_cast<uint32_t>(row), fileCol));
    }
    return answer(true);
}

// --- What the barber shop calls the two things it can change ---
//
// These name a category rather than describing it, and the barber builds a
// global's name out of the answer:
//
//     BarberShopFrameSelector1Category:SetText(_G["HAIR_"..GetHairCustomization().."_STYLE"])
//
// Concatenating nil raises, and this runs from BarberShop_OnLoad - which the
// client reaches the moment a player sits down, because BARBER_SHOP_OPEN is
// fired and the interface answers it by loading the barber addon. So sitting
// in the chair took the addon down as it loaded.
//
// The hair category is NORMAL for everyone but tauren, who have horns where
// other races have hair - the barber offers them "Horn Style" and "Horn Color",
// and HAIR_HORNS_STYLE and HAIR_HORNS_COLOR are both there in globalstrings
// beside the NORMAL pair. It is the only race that differs, which is why the
// facial-hair function below has a switch and this has an if.
/// A ChrRaces customization word for the player's race - the hair one, or
/// the facial one for the player's gender - or "NORMAL" (0x0052e6d0,
/// 0x0052e730).
static std::string raceCustomization(lua_State* L, bool facial) {
    auto* gh = getGameHandler(L);
    auto* am = gh ? gh->services().assetManager : nullptr;
    if (!gh || !am || !am->isInitialized()) return "NORMAL";
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* rl = layouts ? layouts->getLayout("ChrRaces") : nullptr;
    if (!rl) return "NORMAL";
    // The gender byte of UNIT_FIELD_BYTES_0, as the client reads it off the
    // player (+0x1008, +0x1c): 1 is female, anything else the male column.
    uint32_t bytes0 = 0;
    if (auto e = gh->getEntityManager().getEntity(gh->getPlayerGuid()))
        bytes0 = e->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
    const bool female = ((bytes0 >> 16) & 0xFFu) == 1;
    const uint32_t field = facial
        ? rl->tryField(female ? "FacialHairCustomization1" : "FacialHairCustomization0")
        : rl->tryField("HairCustomization");
    auto races = am->loadDBCOptional("ChrRaces.dbc");
    if (!races || !races->isLoaded() || field >= races->getFieldCount()) return "NORMAL";
    const int32_t row = races->findRecordById(gh->getPlayerRace());
    if (row < 0) return "NORMAL";
    return races->getString(static_cast<uint32_t>(row), field);
}

static int lua_GetHairCustomization(lua_State* L) {
    lua_pushstring(L, raceCustomization(L, false).c_str());
    return 1;
}

static int lua_GetFacialHairCustomization(lua_State* L) {
    lua_pushstring(L, raceCustomization(L, true).c_str());
    return 1;
}

/// CanAlterSkin() - whether the barber offers a fourth selector for skin.
///
/// False, and not merely for want of data: answering yes would put a selector
/// on screen that this client cannot fill, and the barber tests every selector
/// it has drawn before it will let the player buy anything.
// CanAlterSkin() (0x0052e4f0): whether the shop has a skin style for the
// player's race and gender.
static int lua_CanAlterSkin(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushboolean(L, svc && svc->barberCanAlterSkin && svc->barberCanAlterSkin() ? 1 : 0);
    return 1;
}

// --- The barber's selectors, its price and its two exits ---
//
// The style lists and the originals used to be built inside this client's own
// barber window, so with the panel handed over nothing had built them. They
// come from WindowManager through LuaServices now, from a version that runs
// whether or not this client is drawing the chair.

/// GetBarberShopStyleInfo(selector) → name, description, cost, isCurrent
///
/// blizzard_barbershopui.lua reads the first and the fourth, and reads the
/// fourth through select(4, ...) - so all four have to be there for the index
/// to land on the right one.
static int lua_GetBarberShopStyleInfo(lua_State* L) {
    // 0x0052e850: nothing out of the chair (DAT_00bd19b8), asked before the
    // argument; in it a number or the usage line (its own text names
    // GetBarberShopInfo, 0x00a08150), rounded, 1 to 4.
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isBarberShopOpen()) return 0;
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetBarberShopInfo(type)");
    const int selector = static_cast<int>(std::lround(lua_tonumber(L, 1)));
    if (selector < 1 || selector > 4) return 0;
    auto* svc = getLuaServices(L);
    // The skin's 4 only where the race has skin styles to go through
    // (0x0052e850: DAT_00bd19b4, as SetNextBarberShopStyle asks) - nothing
    // otherwise (T404: a 'Current' row was answered for a blood elf).
    if (selector == 4 && (!svc || !svc->barberCanAlterSkin || !svc->barberCanAlterSkin())) return 0;
    std::string name, description;
    uint32_t cost = 0;
    bool isCurrent = false;
    if (!svc || !svc->getBarberStyleInfo ||
        !svc->getBarberStyleInfo(selector, name, description, cost, isCurrent)) {
        return 0;
    }
    // name and description, nil and nil for a hair colour (0x0052e850); the
    // style's price; whether it is the one the player came in with.
    if (selector == 2) {
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushstring(L, name.c_str());
        lua_pushstring(L, description.c_str());
    }
    lua_pushinteger(L, static_cast<lua_Integer>(cost));
    luaPushOneOrNil(L, isCurrent);
    return 4;
}

/// SetNextBarberShopStyle(selector, direction) - the arrows beside a selector.
///
/// The forward arrow passes 1 and the back arrow passes nothing at all
/// (blizzard_barbershopui.xml:31 and :56), so an absent argument means back
/// rather than a missing parameter.
static int lua_SetNextBarberShopStyle(lua_State* L) {
    // 0x0052fdd0: a number or the usage line, rounded; 1 to 4, the skin's 4
    // only where there is a skin style to move through.
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetNextBarberShopStyle(type[, backward])");
    const int selector = static_cast<int>(std::lround(lua_tonumber(L, 1)));
    if (selector < 1 || selector > 4) return 0;
    if (selector == 4) {
        auto* svc = getLuaServices(L);
        if (!svc || !svc->barberCanAlterSkin || !svc->barberCanAlterSkin()) return 0;
    }
    const int direction = lua_isnoneornil(L, 2) ? -1 : 1;
    if (auto* svc = getLuaServices(L); svc && svc->setNextBarberStyle) {
        svc->setNextBarberStyle(selector, direction);
    }
    return 0;
}

/// GetBarberShopTotalCost() → copper for everything currently changed.
static int lua_GetBarberShopTotalCost(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushinteger(L, svc && svc->getBarberTotalCost
                           ? static_cast<lua_Integer>(svc->getBarberTotalCost()) : 0);
    return 1;
}

/// BarberShopReset() - put every selector back to what the character wears.
static int lua_BarberShopReset(lua_State* L) {
    if (auto* svc = getLuaServices(L); svc && svc->barberReset) svc->barberReset();
    return 0;
}

/// CancelBarberShop() - leave the chair without buying anything.
static int lua_CancelBarberShop(lua_State* L) {
    // The preview has been changing the character as the selectors moved, so
    // leaving without buying has to put them back. Before the close, because
    // closing the shop is what drops the state the reset is read from.
    if (auto* svc = getLuaServices(L); svc && svc->barberReset) svc->barberReset();
    if (auto* gh = getGameHandler(L)) gh->closeBarberShop();
    return 0;
}

/// GetMaxCombatRatingBonus(rating) → the cap on what a rating can give. Zero
/// would read as "capped at nothing", so this answers with a hundred percent,
/// which is no cap in any practical sense.
static int lua_GetMaxCombatRatingBonus(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetMaxCombatRatingBonus(ratingIndex)");   // 0x006082c0
    if (const long r = std::lround(lua_tonumber(L, 1)); r < 1 || r > 25)
        return luaL_error(L, "ratingIndex is in the range 1 .. %d", 25);
    // -1 for every rating but the three crit-taken ones (15-17), whose cap is
    // the double 0x404080000B000000 (0x006082c0).
    const long r = std::lround(lua_tonumber(L, 1));
    if (r >= 15 && r <= 17) {
        const uint64_t bits = 0x404080000B000000ull;
        double d;
        std::memcpy(&d, &bits, sizeof d);
        lua_pushnumber(L, d);
    } else {
        lua_pushnumber(L, -1);
    }
    return 1;
}

/// GetRestState() (0x0051b8b0) → the rest state's id, name and experience
/// factor: PLAYER_BYTES_2's byte 3 (1 rested, 2 normal, 6 Refer-a-Friend -
/// whether there is rested experience to spend, not whether the player is
/// in an inn) looked up in Exhaustion.dbc; three nils with no row for it.
/// It answered 1 or 2 by the rested pool alone, with English names and a
/// factor of 1.5 where the table says 2.
static int lua_GetRestState(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int state = gh ? gh->getPlayerRestState() : -1;
    const ExhaustionRow row = exhaustionRow(gh, state);
    if (!row.found) { lua_pushnil(L); lua_pushnil(L); lua_pushnil(L); return 3; }
    lua_pushnumber(L, state);
    lua_pushstring(L, row.name.c_str());
    lua_pushnumber(L, row.factor);
    return 3;
}

// --- Quest Log API ---

static int lua_UnitAffectingCombat(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitAffectingCombat(\"unit\")");   // 0x0060f860
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const auto entity = gh && guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    luaPushOneOrNil(L, entity && flags != 0xFFFF && (entity->getField(flags) & 0x80000u) != 0);
    return 1;
}



// HasFullControl() - whether the player is in command of their own character
//
// Read as `not HasFullControl()` to grey out the right-click menu, so an absent
// answer disabled duelling, trading and inviting permanently. Nothing here
// charms, fears or confuses anyone, so control is never lost and saying so is
// the truthful answer rather than a convenient one.
// GetPetSpellBonusDamage() → the spell power a pet inherits
//
// The pet tab formats this with %d before testing it, and %d against nil raises
// on the spot - on a tab of the character frame, which is drawn by default.
// It is not worked out here any more than it is in the client: the accessor at
// 0x0060e410 returns the player's PLAYER_FIELD_PET_SPELL_POWER dword as a signed
// number, so a pet that loses spell damage reads negative and
// petpaperdollframe.lua:619 puts it in red. Zero stays the answer for a realm
// that has no such field to write.
static int lua_GetPetSpellBonusDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getPetSpellBonusDamage() : 0);
    return 1;
}

static int lua_HasFullControl(lua_State* L) {
    // 0x00611600: a player that is alive and whose control the server has
    // not taken away.
    auto* gh = getGameHandler(L);
    const auto me = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    const auto* unit = me && me->isUnit() ? static_cast<const game::Unit*>(me.get()) : nullptr;
    // Nor while it looks through another's eyes (+0x1858 0x1, set as it
    // sends CMSG_FAR_SIGHT 1, 0x006e2880).
    const auto* moves = gh ? gh->getMovementHandler() : nullptr;
    const bool farSight = moves && moves->farSightAnnounced();
    luaPushOneOrNil(L, unit && !farSight && unit->getHealth() > 0 && gh->isServerMovementAllowed());
    return 1;
}

// --- Dying, and getting back up ---
//
// The death popup already appears; its buttons had nothing behind them, so a
// player could be told they had died and be unable to do anything about it.

// RepopMe() - release the spirit and run back

// RetrieveCorpse() - resurrect where the body is
static int lua_RetrieveCorpse(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->reclaimCorpse();
    return 0;
}

// Dismount() - what /dismount does
static int lua_Dismount(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->dismount();
    return 0;
}

// StartAttack([unit]) / StopAttack() - swing at what is targeted
//
// The interface passes a unit only to attack something other than the current
// target, which this client cannot do without changing target first, so the
// argument is read and the current target used.
// StartAttack([unit or "name" [, exactMatch]]) (0x00523090): the unit named -
// a token first, then the name resolver with mask 8 as TargetUnit uses it -
// or with no argument the current target.
static int lua_StartAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint64_t target = gh->getTargetGuid();
    if (const char* given = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr; given && *given) {
        std::string who(given);
        toLowerInPlace(who);
        target = resolveUnitGuid(gh, who);
        if (target == 0) target = resolveNamedUnit(gh, who, luaClientSwitchArg(L, 2, false), false);
    }
    if (target != 0) gh->startAutoAttack(target);
    return 0;
}

static int lua_StopAttack(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->stopAutoAttack();
    return 0;
}

/// GetNumRaidMembers() → everyone in the raid, this player included, or nought
/// when the group is not a raid.
///
/// The player is counted because the interface counts them: GetDisplayedAllyFrames
/// compares this against MEMBERS_PER_RAID_GROUP to decide between the party
/// frames and the raid frames, and a raid of five that answered four would be
/// drawn as a party for ever.
static int lua_GetNumRaidMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->raidRosterSize() : 0);
    return 1;
}

/// GetNumPartyMembers() → the other members of this player's own party.
///
/// The roster the server sends is already written without the player in it -
/// SMSG_GROUP_LIST carries members-minus-one and then that many entries - and
/// this subtracted one from it again. A pair answered nought, which is a party
/// the interface draws no frames for at all, and a five-man answered three and
/// lost its last member.
///
/// In a raid it is the player's own subgroup that is asked about, which is what
/// the party frames along the left of the screen show for a raid of one group.
static int lua_GetNumPartyMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    const auto& pd = gh->getPartyData();
    if (!game::groupIsRaid(pd.groupType)) {
        lua_pushnumber(L, static_cast<int>(pd.members.size()));
        return 1;
    }
    int inSubgroup = 0;
    for (const auto& member : pd.members) {
        if (member.subGroup == pd.subGroup) ++inSubgroup;
    }
    lua_pushnumber(L, inSubgroup);
    return 1;
}

// UnitInParty(unit) (0x0060c920 -> 0x0052c680): the player, group or none, or
// one of the four in the party slots - in a raid its own subgroup's.
static int lua_UnitInParty(lua_State* L) {
    auto* gh = getGameHandler(L);
    luaPushOneOrNil(L, gh && inPlayersParty(gh, namedUnitGuid(L, gh)));
    return 1;
}

static int lua_UnitInRaid(lua_State* L) {
    // The unit's place in the raid roster, from 0, or nil (0x0060ca20 ->
    // 0x00608130) - the roster raidN counts from 1.
    auto* gh = getGameHandler(L);
    const char* uid = lua_tostring(L, 1);
    if (!gh || !uid) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (const int n = gh->raidRosterIndex(resolveUnitGuid(gh, uidStr))) {
        lua_pushnumber(L, n - 1);
        return 1;
    }
    return luaReturnNil(L);
}

// GetRaidRosterInfo(index) → name, rank, subgroup, level, class, fileName,
//   zone, online, isDead, role, isML, and the three LFG roles (0x00573690).
static int lua_GetRaidRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto& pd = gh->getPartyData();
    const auto member = gh->raidRosterMember(index);
    if (!member) { return luaReturnNil(L); }
    const game::GroupMember& m = *member;
    lua_pushstring(L, m.name.c_str());       // name
    lua_pushnumber(L, m.guid == pd.leaderGuid ? 2 : (m.flags & 0x01 ? 1 : 0)); // rank (0=member, 1=assist, 2=leader)
    lua_pushnumber(L, m.subGroup + 1);       // subgroup (1-indexed)
    // 0x00573690: a member whose unit is known answers from it - its
    // UNIT_FIELD_LEVEL, the player's own zone, online, dead by its health -
    // and only one out of sight from the roster's stats (T386).
    auto entity = gh->getEntityManager().getEntity(m.guid);
    const bool seen = entity && (entity->getType() == game::ObjectType::UNIT ||
                                 entity->getType() == game::ObjectType::PLAYER);
    if (seen) lua_pushnumber(L, entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_LEVEL)));
    else lua_pushnumber(L, m.isOnline ? m.level : 0);  // level: 0 for one offline and unseen
    // Class, localized and as its token; nil and nil while not known.
    uint8_t classId = 0;
    uint32_t bytes0 = 0;
    if (entity) {
        bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
    }
    if (const char* token = luaClassToken(classId)) {
        lua_pushstring(L, localizedClassName(gh, classId, ((bytes0 >> 16) & 0xFFu) == 1 ? 1 : 0).c_str());
        lua_pushstring(L, token);
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }
    // zone and online: the area the member is in, or PLAYER_OFFLINE and nil.
    if (seen || m.isOnline) {
        uint32_t zoneId = m.zoneId;
        if (seen) {
            auto* svc = getLuaServices(L);
            zoneId = svc && svc->getLiveZoneId ? svc->getLiveZoneId() : 0;
        }
        const std::string zone = zoneId ? gh->getWhoAreaName(zoneId) : std::string();
        if (zone.empty()) lua_pushnil(L);
        else lua_pushstring(L, zone.c_str());
        luaPushOneOrNil(L, true);
    } else {
        lua_pushstring(L, game::interfaceString("PLAYER_OFFLINE", "Offline").c_str());
        lua_pushnil(L);
    }
    // isDead: the roster's dead bit (0x04) on one online, or no health left.
    constexpr uint16_t kMemberDead = 0x04;
    const bool dead = seen ? entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_HEALTH)) == 0
                           : m.isOnline && (m.onlineStatus & kMemberDead) != 0;
    luaPushOneOrNil(L, dead);
    // role: the member flags' main tank (0x02) or main assist (0x04).
    constexpr uint8_t kMainTank = 0x02, kMainAssist = 0x04;
    if (m.flags & kMainTank) lua_pushstring(L, "MAINTANK");
    else if (m.flags & kMainAssist) lua_pushstring(L, "MAINASSIST");
    else lua_pushnil(L);
    // isML: the master looter, under master loot only.
    constexpr uint8_t kMasterLoot = 2;
    luaPushOneOrNil(L, pd.lootMethod == kMasterLoot && pd.looterGuid == m.guid);
    // The LFG roles: tank 0x02, healer 0x04, damage 0x08.
    luaPushOneOrNil(L, (m.roles & 0x02) != 0);
    luaPushOneOrNil(L, (m.roles & 0x04) != 0);
    luaPushOneOrNil(L, (m.roles & 0x08) != 0);
    return 14;
}

// GetThreatStatusColor(status) (0x00511fe0).
static int lua_GetThreatStatusColor(lua_State* L) {
    const auto status = static_cast<int64_t>(std::nearbyint(lua_tonumber(L, 1) + 1.0));
    const uint32_t c = game::threatStatusColor(static_cast<uint32_t>(status));
    for (const int shift : {16, 8, 0}) lua_pushnumber(L, static_cast<float>(c >> shift & 0xff) * 0.00392156f);
    return 3;
}

// GetReadyCheckStatus(unit) (0x00574180): "ready", "notready" or "waiting"
// while a check runs, to a party's leader or a raid's leader and assistants;
// nil otherwise.
static int lua_GetReadyCheckStatus(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* status = nullptr;
    if (gh) {
        std::string uid = luaL_optstring(L, 1, "");
        toLowerInPlace(uid);
        status = gh->getReadyCheckStatus(resolveUnitGuid(gh, uid));
    }
    if (status) lua_pushstring(L, status);
    else lua_pushnil(L);
    return 1;
}

// GetReadyCheckTimeLeft() (0x00572c80): whole seconds to the ready check's
// deadline, 0 out of a group or with none running.
static int lua_GetReadyCheckTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getReadyCheckSecondsLeft() : 0);
    return 1;
}

// RegisterUnitWatch / UnregisterUnitWatch - secure unit frame stubs
static int lua_RegisterUnitWatch(lua_State* L) { (void)L; return 0; }
static int lua_UnregisterUnitWatch(lua_State* L) { (void)L; return 0; }

static int lua_UnitIsUnit(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))   // 0x0060c3d0
        return luaL_error(L, "Usage: UnitIsUnit(\"unit\", \"otherUnit\")");
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    // The same token is the same unit, whoever is behind it (0x0060c3d0).
    if (u1 == u2) { lua_pushnumber(L, 1); return 1; }
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    luaPushOneOrNil(L, g1 != 0 && g1 == g2);
    return 1;
}

/// How the first unit regards the second, numbered as UnitReaction numbers
/// it - 0x007251c0 asked by the first, which is what UnitReaction (0x0060d280),
/// UnitIsEnemy (0x0060d330) and UnitIsFriend (0x0060d3d0) all ask. Not
/// symmetric: Dalaran's Kirin Tor regard a player at a neutral standing as
/// neutral, and the player, not at war with them, regards them as friendly.
/// The interface passes both orders - UnitIsFriend("player", self.unit)
/// beside UnitIsEnemy(self.unit, "player") - so which side is asking matters.
/// 0 when either unit is unknown.
static int reactionBetween(lua_State* L, const char* first, const char* second) {
    auto* gh = getGameHandler(L);
    auto* a = resolveUnit(L, first);
    auto* b = resolveUnit(L, second);
    if (!gh || !a || !b) return 0;
    return gh->unitReaction(*a, *b) + 1;
}

/// UnitIsFriend(unit1, unit2): unit1 regards unit2 as friendly (0x00514050).
static int lua_UnitIsFriend(lua_State* L) {
    const char* a = luaL_optstring(L, 1, "player");
    const char* b = luaL_optstring(L, 2, nullptr);
    if (!b) { b = a; a = "player"; }
    luaPushOneOrNil(L, reactionBetween(L, a, b) >= 5);
    return 1;
}

/// UnitIsEnemy(unit1, unit2) - is unit2 hostile to unit1. The same shape as
/// UnitIsFriend above, and it was wrong the same way: asked about the player it
/// answered false for every enemy.
static int lua_UnitIsEnemy(lua_State* L) {
    // 0x0060d330: unit1's reaction to unit2 below 2, which is hostile or worse;
    // both units are required.
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitIsEnemy(\"unit\", \"otherUnit\")");   // 0x0060d330
    const char* a = lua_tostring(L, 1);
    const char* b = lua_tostring(L, 2);
    const int reaction = reactionBetween(L, a, b);
    luaPushOneOrNil(L, reaction != 0 && reaction <= 2);
    return 1;
}

// UnitCreatureType(unit) (0x00611780) → CreatureType.dbc's localized name for
// the unit's type, nil without one. The type (0x0071f300): the shapeshift
// form's CreatureType when it sets one, else a creature's template type, else
// the race's (ChrRaces CreatureType) for a player.
static int lua_UnitCreatureType(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    auto entity = guid != 0 ? gh->getEntityManager().getEntity(guid) : nullptr;
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) return luaReturnNil(L);
    uint32_t type = 0;
    const uint32_t bytes2 = unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2));
    if (const auto* row = gh->shapeshiftFormRow(static_cast<uint8_t>(bytes2 >> 24)); row && row->creatureType > 0)
        type = row->creatureType;
    auto* am = gh->services().assetManager;
    const auto* layouts = pipeline::getActiveDBCLayout();
    if (type == 0 && entity->getType() != game::ObjectType::PLAYER) {
        type = gh->getCreatureType(unit->getEntry());
    } else if (type == 0) {
        const uint32_t race = unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) & 0xFFu;
        const auto* layout = layouts ? layouts->getLayout("ChrRaces") : nullptr;
        const uint32_t typeF = layout ? layout->tryField("CreatureType") : 0xFFFFFFFFu;
        auto races = am && am->isInitialized() ? am->loadDBCOptional("ChrRaces.dbc") : nullptr;
        if (races && races->isLoaded() && typeF < races->getFieldCount())
            if (const int32_t r = races->findRecordById(race); r >= 0)
                type = races->getUInt32(static_cast<uint32_t>(r), typeF);
    }
    if (type == 0) return luaReturnNil(L);
    auto types = am && am->isInitialized() ? am->loadDBCOptional("CreatureType.dbc") : nullptr;
    if (types && types->isLoaded() && types->getFieldCount() > 1) {
        if (const int32_t r = types->findRecordById(type); r >= 0) {
            const std::string name = types->getString(static_cast<uint32_t>(r), 1);
            lua_pushstring(L, name.c_str());
            return 1;
        }
        return luaReturnNil(L);
    }
    // Without the file, the English names.
    static constexpr const char* kTypes[] = {
        "", "Beast", "Dragonkin", "Demon", "Elemental", "Giant", "Undead", "Humanoid",
        "Critter", "Mechanical", "Not specified", "Totem", "Non-combat Pet", "Gas Cloud"
    };
    if (type >= std::size(kTypes)) return luaReturnNil(L);
    lua_pushstring(L, kTypes[type]);
    return 1;
}

// GetPlayerInfoByGUID(guid) → localizedClass, englishClass, localizedRace,
// englishRace, sex, name, realm (0x00613f90). Only a player's guid, and only
// once the name cache has it (0x0067d770 asks the server meanwhile): nothing
// at all before. The class and race names are the cached player's, in its
// gender (0x0072aab0, 0x0072aa70); the English ones are the class token and
// the race's file string; sex is 2 for male, 3 for female.
static int lua_GetPlayerInfoByGUID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* guidStr = luaL_checkstring(L, 1);
    if (!gh || !guidStr) return 0;
    uint64_t guid = 0;
    if (guidStr[0] == '0' && (guidStr[1] == 'x' || guidStr[1] == 'X'))
        guid = strtoull(guidStr + 2, nullptr, 16);
    else
        guid = strtoull(guidStr, nullptr, 16);
    if (guid == 0) return luaL_error(L, "Usage: GetPlayerInfoByGUID(\"playerGUID\")");
    if ((guid >> 60) != 0) return 0;   // not a player

    std::string name = gh->lookupName(guid);
    uint8_t classId = gh->lookupPlayerClass(guid);
    uint8_t raceId = gh->lookupPlayerRace(guid);
    uint8_t gender = gh->lookupPlayerGender(guid);
    if (guid == gh->getPlayerGuid()) {
        for (const auto& c : gh->getCharacters())
            if (c.guid == guid && name.empty()) name = c.name;
        classId = gh->getPlayerClass();
        raceId = gh->getPlayerRace();
    }
    if (name.empty()) return 0;
    const int g = gender == 1 ? 1 : 0;
    lua_pushstring(L, localizedClassName(gh, classId, g).c_str());              // 1
    if (const char* token = luaClassToken(classId)) lua_pushstring(L, token);  // 2
    else lua_pushnil(L);
    lua_pushstring(L, localizedRaceName(gh, raceId, g).c_str());                // 3
    if (raceId > 0 && raceId < 12) lua_pushstring(L, kLuaRaceFileNames[raceId]); // 4
    else lua_pushnil(L);
    lua_pushnumber(L, gender == 1 ? 3 : 2);                                    // 5
    lua_pushstring(L, name.c_str());                                           // 6
    lua_pushstring(L, "");                                                     // 7: same realm
    return 7;
}


static int lua_UnitClassification(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "normal"); return 1; }
    const char* uid = luaL_optstring(L, 1, "");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "normal"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "normal");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "normal"); return 1; }
    // A pet's is none (0x00718a00 asks UNIT_FIELD_PETNUMBER).
    const int rank = unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER)) != 0
                         ? 0 : gh->getCreatureRank(unit->getEntry());
    switch (rank) {
        case 1:  lua_pushstring(L, "elite"); break;
        case 2:  lua_pushstring(L, "rareelite"); break;
        case 3:  lua_pushstring(L, "worldboss"); break;
        case 4:  lua_pushstring(L, "rare"); break;
        // 0x0060d970's table 0x00ad21bc: normal, elite, rareelite,
        // worldboss, rare, trivial.
        case 5:  lua_pushstring(L, "trivial"); break;
        default: lua_pushstring(L, "normal"); break;
    }
    return 1;
}

// GetComboPoints(unit [, target]) → number
/// GetComboPoints(unit [, target]) - the combo points that unit has on that
/// target, and on that target only.
///
/// 0x00611670 resolves both names to guids and answers for the two units that
/// can hold combo points: the player - gated on the class byte being 4 (rogue)
/// or 11 (druid), the same byte class masks are shifted by, which is where the
/// client says no for everyone else - and the player's pet (0x005d3520). The
/// count comes back only when the unit named is the one the points are on:
/// 0x00513920 compares against the stored combo target, having first put the
/// player's current target in place of a zero pair, so the argument really is
/// a question about a unit rather than about whoever is selected now. Every
/// other unit, and any other target, answers 0.
///
/// WoWee answered the player's count for every call: an addon asking
/// GetComboPoints("player", "focus") got the current target's number, and the
/// pet's own points - which the server sends separately, and which
/// UNIT_COMBO_POINTS("pet") already carries - were never readable here at all.
/// A call with no unit at all answers nothing, as the client's
/// "Usage: GetComboPoints(\"unit\"[, \"target\"]" does, rather than 0.
static int lua_GetComboPoints(lua_State* L) {
    const char* unit = lua_tostring(L, 1);
    if (!unit) return 0;
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }

    std::string uid(unit);
    toLowerInPlace(uid);
    const uint64_t who = resolveUnitGuid(gh, uid);

    // A target the call does not name, or names without being able to resolve,
    // is the player's current target - 0x00513920 and 0x005d3520 both put the
    // current target in place of a zero pair before comparing.
    const char* named = lua_tostring(L, 2);
    uint64_t on = 0;
    if (named && *named) {
        std::string targetName(named);
        toLowerInPlace(targetName);
        on = resolveUnitGuid(gh, targetName);
    }
    if (on == 0) on = gh->getTargetGuid();

    uint8_t points = 0;
    if (who == gh->getPlayerGuid()) {
        const uint8_t cls = gh->getPlayerClass();
        if ((cls == 4 || cls == 11) && gh->getComboTarget() == on)
            points = gh->getComboPoints();
    } else if (who != 0 && who == gh->getPetGuid()) {
        points = gh->petComboPointsOn(on);
    }
    lua_pushnumber(L, points);
    return 1;
}

// UnitReaction(unit, otherUnit) → 1-8 (hostile to exalted)
// Simplified: hostile=2, neutral=4, friendly=5
static int lua_UnitReaction(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitReaction(\"unit\", \"otherUnit\")");   // 0x0060d280
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { return luaReturnNil(L); }
    // If unit2 is the player, always friendly to self
    std::string u1(uid1);
    toLowerInPlace(u1);
    std::string u2(uid2);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == g2) { lua_pushnumber(L, 5); return 1; } // same unit = friendly
    // 0x0060d280: how unit1 regards unit2, plus one. How a creature regards
    // the player, which is the question the tooltip and the name colours ask
    // - UnitReaction(unit, "player") - is its standing for a faction with
    // one: the Kurenai read as friendly to an Alliance player at Unfriendly
    // when answered hostile-or-friendly, who could not then find out why
    // Telaar would not speak to them. The other way round the standing does
    // not count, only whether the player is at war (reactionBetween).
    const int reaction = reactionBetween(L, uid1, uid2);
    if (reaction == 0) { return luaReturnNil(L); }
    lua_pushnumber(L, reaction);
    return 1;
}

/// Which side the battlefield scoreboard is showing: 1 Alliance, 0 Horde,
/// -1 both. Set by the frame's tabs through SetBattlefieldScoreFaction.
static int& battlefieldScoreFaction(game::GameHandler* gh) {
    static int none = -1;
    return gh ? gh->battlefieldScoreFactionRef() : none;
}

/// The scoreboard's sort keys (0x00bea2c0): seventeen pairs of a key and a
/// direction, every key 0..16 once, all direction 0 at start (the area init
/// at 0x0054e2xx) and never reset. Keys: 0 killing blows, 1 deaths, 2 honor,
/// 3 name, 4 class, 5 honorable kills, 6 damage, 7 healing, 8 team, 9..16 the
/// battleground's stat1..stat8.
struct BgSortKey { int key; int turned; };
static std::array<BgSortKey, 17>& battlefieldSortKeys() {
    static std::array<BgSortKey, 17> keys = [] {
        std::array<BgSortKey, 17> k{};
        for (int i = 0; i < 17; ++i) k[static_cast<size_t>(i)] = {i, 0};
        return k;
    }();
    return keys;
}

/// 0x0054cee0: the key to the front of the list - at the front already, its
/// direction turned; else with the direction it last had. 12340 writes that
/// direction into the front slot (0x00bea2c4) BEFORE it shifts the entries
/// down, so the old front key moves to the second place carrying the moved
/// key's direction, not its own - its own turn is lost (V536 b).
static void battlefieldSortBy(int key) {
    auto& keys = battlefieldSortKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].key != key) continue;
        keys[0].turned = keys[i].turned;
        if (i == 0) {
            keys[0].turned = keys[0].turned == 0 ? 1 : 0;
        } else {
            for (size_t j = i; j > 0; --j) keys[j] = keys[j - 1];
        }
        keys[0].key = key;
        return;
    }
}

/// One step of 0x0054a740 for two rows with names: -1 when `a` goes first,
/// 1 when `b` does, 0 to ask the next key. Class and stat keys decide even
/// when equal in 12340 (they answer 1 for equal rows, `decided`); here equal
/// rows keep the packet's order instead of the sort's accident.
static int bgScoreKeyCompare(game::GameHandler* gh, int key, bool& decided,
                             const game::BgPlayerScore& a, const game::BgPlayerScore& b) {
    decided = false;
    const auto desc = [](uint32_t x, uint32_t y) { return x == y ? 0 : (y < x ? -1 : 1); };
    switch (key) {
        case 0: return desc(a.killingBlows, b.killingBlows);
        case 1: return desc(a.deaths, b.deaths);
        case 2: return desc(a.bonusHonor, b.bonusHonor);
        case 3: {
            const int c = std::strcmp(a.name.c_str(), b.name.c_str());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        case 4: {
            decided = true;
            const uint8_t ca = gh ? gh->lookupPlayerClass(a.guid) : 0;
            const uint8_t cb = gh ? gh->lookupPlayerClass(b.guid) : 0;
            return ca == cb ? 0 : (cb < ca ? -1 : 1);
        }
        case 5: return desc(a.honorableKills, b.honorableKills);
        case 6: return desc(a.damageDone, b.damageDone);
        case 7: return desc(a.healingDone, b.healingDone);
        case 8: return a.team == b.team ? 0 : (b.team <= a.team ? 1 : -1);
        default: {
            if (key < 9) return 0;
            const size_t stat = static_cast<size_t>(key - 9);
            if (stat >= a.bgStats.size() || stat >= b.bgStats.size()) return 0;
            decided = true;
            const auto x = static_cast<int32_t>(a.bgStats[stat].second);
            const auto y = static_cast<int32_t>(b.bgStats[stat].second);
            return x == y ? 0 : (x <= y ? 1 : -1);
        }
    }
}

/// The scoreboard rows that pass the current filter, in the current order.
///
/// Pointers into the scoreboard rather than copies: this is rebuilt on every
/// GetBattlefieldScore call, once per row per redraw, and a battleground holds
/// forty players.
/// A scoreboard row's side as 12340 keeps it (+0x20): an arena's from the
/// packet; a battleground's, which the packet does not carry, from the race
/// the name cache holds (0x0054aa30) - 0 Horde, 1 Alliance, -1 neither, and
/// 0 for a player the cache does not know.
static int bgScoreRowSide(game::GameHandler* gh, const game::BgPlayerScore& p) {
    if (p.hasTeam) return p.team;
    const uint8_t race = gh ? gh->lookupPlayerRace(p.guid) : 0;
    uint32_t group = 0;
    if (race != 0) {
        auto* am = gh->services().assetManager;
        if (am && am->isInitialized()) {
            auto races = am->loadDBCOptional("ChrRaces.dbc");
            auto templates = am->loadDBCOptional("FactionTemplate.dbc");
            if (races && races->isLoaded() && templates && templates->isLoaded() &&
                races->getFieldCount() > 2 && templates->getFieldCount() > 3) {
                const int32_t r = races->findRecordById(race);
                const int32_t t = r >= 0 ? templates->findRecordById(races->getUInt32(static_cast<uint32_t>(r), 2)) : -1;
                if (t >= 0) group = templates->getUInt32(static_cast<uint32_t>(t), 3);
            }
        }
    }
    return game::battleground_rules::scoreRowSide(race != 0, group);
}

static std::vector<const game::BgPlayerScore*>
filteredBgScores(game::GameHandler* gh, const game::BgScoreboardData& sb) {
    std::vector<const game::BgPlayerScore*> rows;
    rows.reserve(sb.players.size());
    const int want = battlefieldScoreFaction(gh);
    for (const auto& p : sb.players) {
        // 0x0054aa30 counts the rows of the chosen side (0x00acd170, -1 both).
        if (want < 0 || bgScoreRowSide(gh, p) == want)
            rows.push_back(&p);
    }

    // 0x0054aa30 sorts the board with 0x0054a740 over the key list, from the
    // first board on; rows without a name record compare equal.
    const auto& keys = battlefieldSortKeys();
    std::stable_sort(rows.begin(), rows.end(),
                     [gh, &keys](const game::BgPlayerScore* a, const game::BgPlayerScore* b) {
                         if (a->name.empty() || b->name.empty()) return false;
                         for (const auto& k : keys) {
                             bool decided = false;
                             int c = bgScoreKeyCompare(gh, k.key, decided, *a, *b);
                             if (k.turned != 0) c = -c;
                             if (c != 0) return c < 0;
                             if (decided) return false;
                         }
                         return false;
                     });
    return rows;
}

/// 0x0054aa30's end: UPDATE_BATTLEFIELD_SCORE, unless names are still owed.
static void announceBattlefieldScore(game::GameHandler* gh) {
    if (!gh) return;
    const auto* sb = gh->getBgScoreboard();
    if (sb && sb->namesPending != 0) return;
    gh->fireAddonEvent("UPDATE_BATTLEFIELD_SCORE", {});
}

// UnitIsConnected(unit) → boolean
static int lua_UnitIsConnected(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsConnected(\"unit\")");   // 0x0060f790
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    // 0x0060f790: a unit this client has is connected (the player too);
    // without one, a group member's status record says (+8 bit 1).
    if (guid == gh->getPlayerGuid()) { luaPushOneOrNil(L, 1); return 1; }
    if (auto entity = gh->getEntityManager().getEntity(guid); entity && entity->isUnit()) {
        luaPushOneOrNil(L, 1);
        return 1;
    }
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            luaPushOneOrNil(L, m.isOnline ? 1 : 0);
            return 1;
        }
    }
    // Not a member: the pet a member's stats named, while that member is
    // online (0x00513cb0 -> 0x0052bf80: slot taken, +8 bit 1, pet guid at
    // +0x160 of the record) - a member's pet out of sight is connected
    // (found by V in code).
    for (const auto& m : pd.members) {
        if (m.guid != 0 && m.isOnline && m.petGuid != 0 && m.petGuid == guid) {
            luaPushOneOrNil(L, 1);
            return 1;
        }
    }
    luaPushOneOrNil(L, 0);
    return 1;
}

// HasAction(slot) → boolean (1-indexed slot)

/// 0x0052d310 / 0x00573200: the player, a party or raid member, or the pet
/// or charm (UNIT_FIELD_CHARM, else UNIT_FIELD_SUMMON) of one of them. For a
/// member out of view the client asks the pet guid its party stats carried
/// (+0x160 of the record, while the member is online: +8 bit 1) - found by V
/// in code: this was left out, so a member's pet out of view did not count.
static bool isGroupMemberOrPet(game::GameHandler* gh, uint64_t guid) {
    if (!gh || guid == 0) return false;
    const auto petOf = [gh](uint64_t owner) {
        const uint64_t charm = guidField(gh, owner, game::UF::UNIT_FIELD_CHARM);
        return charm ? charm : guidField(gh, owner, game::UF::UNIT_FIELD_SUMMON);
    };
    const uint64_t player = gh->getPlayerGuid();
    if (guid == player || petOf(player) == guid) return true;
    for (const auto& m : gh->getPartyData().members) {
        if (m.guid == 0) continue;
        if (m.guid == guid) return true;
        if (gh->getEntityManager().getEntity(m.guid)) {
            if (petOf(m.guid) == guid) return true;
        } else if (m.isOnline && m.petGuid != 0 && m.petGuid == guid) {
            return true;
        }
    }
    return false;
}

/// UnitIsFeignDeath(unit) (0x0060f3d0): 1 for a unit of the player's own
/// group (0x00512a30) whose UNIT_DYNAMIC_FLAGS carry 0x20, which is how a
/// feigning hunter is shown to them; nil otherwise.
static int lua_UnitIsFeignDeath(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsFeignDeath(\"unit\")");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    if (gh && unit && isGroupMemberOrPet(gh, unit->getGuid()) &&
        (unit->getField(game::fieldIndex(game::UF::UNIT_DYNAMIC_FLAGS)) & 0x20) != 0) {
        lua_pushnumber(L, 1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// UnitIsTrivial(unit) (0x006112b0 -> 0x006de130): a unit that is not a
/// player, far enough below the player's level to be grey - more than the
/// table at 0x00a31bb8 allows for the player's level / 5, or more than 8 past
/// level 99. 1 or nil.
static int lua_UnitIsTrivial(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsTrivial(\"unit\")");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    bool trivial = false;
    if (gh && unit && unit->getType() != game::ObjectType::PLAYER) {
        auto me = std::dynamic_pointer_cast<game::Unit>(gh->getEntityManager().getEntity(gh->getPlayerGuid()));
        if (me) {
            const int playerLevel = static_cast<int>(me->getLevel());
            const int gap = playerLevel - static_cast<int>(unit->getLevel());
            if (gap > 0)
                trivial = gap > game::triviality::greyGap(static_cast<uint32_t>(playerLevel));
        }
    }
    if (trivial) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// UnitIsPVPSanctuary(unit) (0x0060cf20): UNIT_FIELD_BYTES_2's second byte
/// carries 0x08 in a sanctuary such as Dalaran. 1 or nil.
static int lua_UnitIsPVPSanctuary(lua_State* L) {
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    const uint32_t bytes2 = unit ? unit->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2)) : 0;
    if ((bytes2 >> 8) & 0x08) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// IsRealPartyLeader() (0x0052cd30) / IsRealRaidLeader() (0x00573ab0): the
/// group's leader (DAT_00bd1990) is the player. 1 or nil.
static int lua_IsRealGroupLeader(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t leader = gh ? gh->realGroupLeader() : 0;
    if (leader != 0 && leader == gh->getPlayerGuid()) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// GetUnitPitch(unit) (0x00613330): the unit's pitch (vtable +0x14c), 0
/// for none.
static int lua_GetUnitPitch(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetUnitPitch(\"unit\")");
    auto* gh = getGameHandler(L);
    const uint64_t guid = resolveUnitArgGuid(L, gh);
    const auto unit = gh && guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    lua_pushnumber(L, unit && unit->isUnit() ? gh->unitPitch(guid) : 0.0f);
    return 1;
}

/// GetPowerRegen() (0x00612b40): the player's power type's regeneration a
/// second, as it regenerates and interrupted (0x004f5390: the table's,
/// 0x004f5300, and UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER or the
/// INTERRUPTED one), each a thousandth more.
static int lua_GetPowerRegen(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto player = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    if (!player || !player->isUnit()) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
    const int type = bytes0 != 0xFFFF ? static_cast<int>((player->getField(bytes0) >> 24) & 0xFFu) : 0;
    for (int interrupted = 0; interrupted < 2; ++interrupted) {
        float modifier = 0.0f;
        const uint16_t f = game::fieldIndex(interrupted ? game::UF::UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER
                                                        : game::UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER);
        if (f != 0xFFFF && type < 7) {
            const uint32_t raw = player->getField(static_cast<uint16_t>(f + type));
            std::memcpy(&modifier, &raw, sizeof modifier);
        }
        const float base = type < 7 ? game::power_prediction::kBaseRegen[type][interrupted] : 0.0f;
        lua_pushnumber(L, static_cast<double>(base + modifier) + 0.001);
    }
    return 2;
}

/// The realm's known-titles run, when the character's block has carried one.
///
/// Six dwords, read one after the other from the field the layout names: the
/// client names the run's first dword and reads the other five beside it
/// (IsTitleKnown, 0x0051b410, at blob +0x778 through +0x78f, which is the same
/// six its own watchers are installed on - three pairs of four bytes, each
/// watched eight wide).
///
/// False only when there is no run at all - the layouts before WotLK name no
/// such field - or no player object yet; then the titles earned this session
/// are the answer. Otherwise the run is the answer, and a dword the block did
/// not carry is zero: the realms send a create block's fields only where they
/// are not zero, so a character with an empty title group never carries that
/// group's dwords, and asking for all six left most characters on the session
/// set - every title they arrived with missing.
static bool knownTitleWords(game::GameHandler* gh, game::KnownTitleWords& words) {
    words.fill(0);
    if (!gh) return false;
    const uint16_t idx = game::fieldIndex(game::UF::PLAYER_FIELD_KNOWN_TITLES);
    if (idx == 0xFFFF) return false;
    const auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!player) return false;
    for (uint32_t i = 0; i < game::kKnownTitleWords; ++i)
        words[i] = player->getField(static_cast<uint16_t>(idx + i));
    return true;
}

void registerUnitLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetUnitPitch",  lua_GetUnitPitch},
                {"GetPowerRegen", lua_GetPowerRegen},
                {"UnitName",      lua_UnitName},
                {"GetUnitName",   lua_UnitName},
                {"UnitHealth",    lua_UnitHealth},
                {"UnitHealthMax", lua_UnitHealthMax},
                {"UnitPower",     lua_UnitPower},
                {"UnitPowerMax",  lua_UnitPowerMax},
                {"UnitMana",      lua_UnitPower},
                {"UnitManaMax",   lua_UnitPowerMax},
                {"UnitLevel",     lua_UnitLevel},
                {"UnitExists",    lua_UnitExists},
                {"UnitIsDead",    lua_UnitIsDead},
                // UnitUsingVehicle(unit) - in a seat, or on its way into or
                // out of one.
                {"UnitUsingVehicle", [](lua_State* L) -> int {
            // 1 while in any passenger state, else nil (0x006134a0).
            auto* gh = getGameHandler(L);
            const uint64_t guid = resolveUnitArgGuid(L, gh);
            if (gh && guid && (gh->vehicleRoster().seatState(guid) != 0 || outOfViewInVehicle(gh, guid)))
                lua_pushnumber(L, 1.0);
            else lua_pushnil(L);
            return 1;
        }},
                {"UnitVehicleSeatInfo", lua_UnitVehicleSeatInfo},
                {"UnitVehicleSeatCount", lua_UnitVehicleSeatCount},
                {"UnitSwitchToVehicleSeat", lua_UnitSwitchToVehicleSeat},
                {"CanSwitchVehicleSeat", lua_CanSwitchVehicleSeat},
                {"GetVehicleUIIndicator", lua_GetVehicleUIIndicator},
                {"GetVehicleUIIndicatorSeat", lua_GetVehicleUIIndicatorSeat},
                {"CanEjectPassengerFromSeat", lua_CanEjectPassengerFromSeat},
                {"EjectPassengerFromSeat", lua_EjectPassengerFromSeat},
                {"UnitIsGhost",   lua_UnitIsGhost},
                {"UnitIsDeadOrGhost", lua_UnitIsDeadOrGhost},
                {"UnitIsAFK",     lua_UnitIsAFK},
                {"UnitIsDND",     lua_UnitIsDND},
                {"UnitPlayerControlled", lua_UnitPlayerControlled},
                {"UnitIsTapped",        lua_UnitIsTapped},
                {"UnitIsTappedByPlayer", lua_UnitIsTappedByPlayer},
                {"UnitIsTappedByAllThreatList", lua_UnitIsTappedByAllThreatList},
                // Not a Unit* name, but it is a unit-field read, so it sits by
                // the other readers of that same dword. It came over from
                // lua_system_api.cpp, where it was a flat false.
                {"IsReferAFriendLinked",  lua_IsReferAFriendLinked},
                {"UnitIsVisible",       lua_UnitIsVisible},
                {"UnitGroupRolesAssigned", lua_UnitGroupRolesAssigned},
                {"UnitCanAttack",       lua_UnitCanAttack},
                {"UnitCanCooperate",    lua_UnitCanCooperate},
                {"UnitCreatureFamily",  lua_UnitCreatureFamily},
                {"UnitOnTaxi",          lua_UnitOnTaxi},
                {"UnitThreatSituation", lua_UnitThreatSituation},
                {"UnitDetailedThreatSituation", lua_UnitDetailedThreatSituation},
                {"UnitSex",       lua_UnitSex},
                {"UnitClass",     lua_UnitClass},
                {"UnitArmor",     lua_UnitArmor},
                {"UnitResistance", lua_UnitResistance},
                {"UnitStat",      lua_UnitStat},
                {"GetDodgeChance",    lua_GetDodgeChance},
                {"GetParryChance",    lua_GetParryChance},
                {"GetBlockChance",    lua_GetBlockChance},
                {"GetCritChance",     lua_GetCritChance},
                {"GetRangedCritChance", lua_GetRangedCritChance},
                {"GetSpellCritChance",  lua_GetSpellCritChance},
                {"GetCombatRating",     lua_GetCombatRating},
                {"GetSpellBonusDamage", lua_GetSpellBonusDamage},
                {"GetSpellBonusHealing", lua_GetSpellBonusHealing},
                {"GetAttackPowerForStat", lua_GetAttackPowerForStat},
                {"GetShapeshiftFormInfo", [](lua_State* L) -> int {
            // GetShapeshiftFormInfo(index) → icon, name, isActive, isCastable
            auto* gh = getGameHandler(L);
            const double asked = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : 0.0;
            if (asked == 0.0) return luaL_error(L, "Usage: GetShapeshiftFormInfo(index)");
            const int index = static_cast<int>(asked);
            if (!gh || index < 1) return 0;

            // The one table CastShapeshiftForm and GetNumShapeshiftForms
            // also read, filtered the same way. The bar walks
            // 1..GetNumShapeshiftForms() and asks about each index, so all
            // three have to be looking at the same list in the same order.
            const auto spells = gh->stanceBarSpells();
            if (static_cast<size_t>(index) > spells.size()) return 0;
            const uint32_t spell = spells[static_cast<size_t>(index) - 1];
            if (game::isPreWotlk()) {
                const auto forms = game::knownShapeshiftForms(gh->getPlayerClass(), gh->getKnownSpells());
                const auto& fi = forms[static_cast<size_t>(index) - 1];
                lua_pushstring(L, fi.icon);
                lua_pushstring(L, fi.name);
                luaPushOneOrNil(L, gh->getShapeshiftFormId() == fi.formId ? 1 : 0);
                luaPushOneOrNil(L, 1);
                return 4;
            }
            // 0x0053de90: active is 0x00802cb0's answer - not a form id, which
            // a presence or an aura does not set. Active, the spell shows its
            // ActiveIconID's icon, else its own; its name is Spell.dbc's.
            // Castable is active, or usable (0x0080b5d0) and paid for
            // (0x008017e0).
            const bool active = gh->formSpellInEffect(spell);
            const auto& cache = gh->spellNameCacheRef();
            const auto it = cache.find(spell);
            const uint32_t activeIcon = active && it != cache.end() ? it->second.activeIconId : 0;
            const std::string icon = activeIcon != 0 ? gh->getIconPath(activeIcon) : gh->getSpellIconPath(spell);
            bool castable = active;
            if (!castable) {
                auto* sh = gh->getSpellHandler();
                const auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
                castable = sh && player && player->isUnit() && sh->spellUsable(spell, nullptr) &&
                           sh->enoughPower(spell, static_cast<const game::Unit&>(*player));
            }
            if (icon.empty()) lua_pushnil(L);
            else lua_pushstring(L, icon.c_str());
            lua_pushstring(L, gh->getSpellName(spell).c_str());
            luaPushOneOrNil(L, active ? 1 : 0);
            luaPushOneOrNil(L, castable ? 1 : 0);
            return 4;
        }},
                // UnitIsPVP 0x0060ce20 / UnitIsPVPFreeForAll 0x0060cfb0: a unit
                // answers from UNIT_FIELD_BYTES_2's PvP byte (descriptor +0x1d1:
                // 0x01 flagged, 0x04 free-for-all), a corpse (PvP only) from
                // CORPSE_FIELD_FLAGS 0x04, and with no such object a group
                // member's status byte (0x00513c30 +8: 0x02, 0x10). These read
                // UNIT_FIELD_FLAGS 0x1000 and PLAYER_FLAGS 0x80, nothing for a
                // member out of sight.
                {"UnitIsPVP", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsPVP(\"unit\")");   // 0x0060ce20
            return pushPvpPredicate(L, false);
        }},
                {"UnitIsPVPFreeForAll", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitIsPVPFreeForAll(\"unit\")");   // 0x0060cfb0
            return pushPvpPredicate(L, true);
        }},
                // The honour panel prints "(Rank N)" from the second return
                // and concatenates it unguarded, so a missing one takes the
                // panel's whole update with it. Rank zero is what an
                // unranked character has.
                // Whether the off hand holds a weapon rather than a shield or
                // a held item. DurabilityFrame_SetAlerts branches on it to
                // decide which of its two off-hand icons to show.
                {"OffhandHasWeapon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { luaPushOneOrNil(L, 0); return 1; }
            const auto& slot = gh->getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (slot.empty()) { luaPushOneOrNil(L, 0); return 1; }
            const auto* info = gh->getItemInfo(slot.item.itemId);
            // Item class 2, a weapon (0x005eac10).
            luaPushOneOrNil(L, info && info->itemClass == 2);
            return 1;
        }},
                // 0x0060da00: 0x00521bf0 with its last argument 0. A creature
                // by how it regards the player; a unit a player controls by
                // who may attack whom, then by its reaction while flagged for
                // PvP, else blue - and blue for no unit.
                {"UnitSelectionColor", [](lua_State* L) -> int {
            const char* uid = luaL_optstring(L, 1, "player");
            auto* unit = resolveUnit(L, uid);
            auto* gh = getGameHandler(L);
            rendering::selection_circle::ColorInput in;
            in.circle = false;
            in.playerControlled = true;
            if (unit && gh) {
                const uint64_t me = gh->getPlayerGuid();
                in.playerControlled = (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED) != 0;
                in.reaction = gh->unitReactionToPlayer(*unit);
                in.targetMayAttackPlayer = gh->unitMayAttack(unit->getGuid(), me);
                in.playerMayAttackTarget = gh->unitMayAttack(me, unit->getGuid());
                const auto pvpOf = [](const game::Entity& e) {
                    const uint16_t b2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
                    const uint16_t fl = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
                    const bool holds = b2 != 0xFFFF && !game::isPreWotlk();
                    return game::unitPvpFlags(holds, holds ? e.getField(b2) : 0u,
                                              fl != 0xFFFF ? e.getField(fl) : 0u);
                };
                const auto meEntity = gh->getEntityManager().getEntity(me);
                const uint32_t theirs = pvpOf(*unit);
                const uint32_t mine = meEntity ? pvpOf(*meEntity) : 0u;
                in.pvp = (theirs & game::pvp::kPvp) != 0 && (theirs & game::pvp::kSanctuary) == 0 &&
                         (mine & game::pvp::kSanctuary) == 0;
            }
            const glm::vec4 c = rendering::selection_circle::color(in);
            lua_pushnumber(L, c.r);
            lua_pushnumber(L, c.g);
            lua_pushnumber(L, c.b);
            lua_pushnumber(L, c.a);
            return 4;
        }},
                {"UnitIsPartyLeader", [](lua_State* L) -> int {
            // 0x0060c770: a player whose PLAYER_FLAGS carry GROUP_LEADER
            // (0x1, descriptor +0x1008 +8), or the guid the group list names
            // its leader (0x00bd1968) - the flag answers for a stranger who
            // leads a group of their own, which the list cannot. The guid
            // compare is plain: a token naming nobody (guid 0) while there
            // is no leader (0) answers 1 there as well.
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "");
            std::string uidStr(uid);
            toLowerInPlace(uidStr);
            const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
            bool leader = gh && gh->getPartyData().leaderGuid == guid;
            if (!leader && gh && guid != 0) {
                const auto entity = gh->getEntityManager().getEntity(guid);
                leader = entity && entity->getType() == game::ObjectType::PLAYER &&
                         (entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS)) & 0x1u) != 0;
            }
            luaPushOneOrNil(L, leader ? 1 : 0);
            return 1;
        }},
                // Whether the token names a corpse object (0x0060c6f0: the
                // object of type mask 0x80), not whether a unit is dead
                // (game::isCorpseObject).
                {"UnitIsCorpse", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            std::string uid(luaL_optstring(L, 1, "player"));
            toLowerInPlace(uid);
            const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
            const auto entity = guid != 0 ? gh->getEntityManager().getEntity(guid) : nullptr;
            luaPushOneOrNil(L, game::isCorpseObject(entity.get()) ? 1 : 0);
            return 1;
        }},
                // Whether the first unit may help the second: 0x0060d690
                // asks 0x007293d0 of unit1, its last argument 0.
                {"UnitCanAssist", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
                return luaL_error(L, "Usage: UnitCanAssist(\"unit\", \"otherUnit\")");   // 0x0060d690
            auto* gh = getGameHandler(L);
            std::string u1(luaL_optstring(L, 1, "player")), u2(luaL_optstring(L, 2, "target"));
            toLowerInPlace(u1);
            toLowerInPlace(u2);
            const uint64_t g1 = gh ? resolveUnitGuid(gh, u1) : 0;
            const uint64_t g2 = gh ? resolveUnitGuid(gh, u2) : 0;
            luaPushOneOrNil(L, (g1 != 0 && g2 != 0 && gh->unitMayAssist(g1, g2)) ? 1 : 0);
            return 1;
        }},
                // GetPVPRankInfo(rank [, unit]) - 0x00611cb0. The name is the
                // interface string "PVP_RANK_<rank>_<side>" (0x00819d40 is the
                // GlobalStrings lookup; PVP_RANK_1_0 .. PVP_RANK_19_1 are
                // there), for a rank of 1..18. The side is a number given as
                // the second argument, else the unit's (the player's when none
                // is named; a named unit that is not a player gives 0): its
                // race's faction template group, 0 Horde, 1 Alliance, -1
                // neither (0x006d6e90, bg_flag::raceTeam). With a name it
                // pushes the name and rank-5 below rank 5 or rank-4 above;
                // without one nil and 0. Both callers test
                // `if (not rankName) rankName = NONE` (honorframe.lua:44-46).
                {"GetPVPRankInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetPVPRankInfo(rank [, unit])");
            const int rank = static_cast<int>(lua_tonumber(L, 1));
            int side = 0;
            if (lua_isnumber(L, 2)) {
                side = static_cast<int>(lua_tonumber(L, 2));
            } else {
                auto* gh = getGameHandler(L);
                const game::Unit* unit = nullptr;
                if (lua_isstring(L, 2)) {
                    unit = resolveUnit(L, lua_tostring(L, 2));
                    if (unit && unit->getType() != game::ObjectType::PLAYER) unit = nullptr;
                } else if (gh) {
                    unit = resolveUnit(L, "player");
                }
                const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
                if (unit && bytes0 != 0xFFFF)
                    side = game::bg_flag::raceTeam(static_cast<uint8_t>(unit->getField(bytes0) & 0xFF));
            }
            if (static_cast<unsigned>(rank - 1) < 18u) {
                char key[32];
                std::snprintf(key, sizeof(key), "PVP_RANK_%d_%d", rank, side);
                lua_getglobal(L, key);
                if (lua_isstring(L, -1) && lua_objlen(L, -1) > 0) {
                    lua_pushnumber(L, rank < 5 ? rank - 5 : rank - 4);
                    return 2;
                }
                lua_pop(L, 1);
            }
            lua_pushnil(L);
            lua_pushnumber(L, 0);
            return 2;
        }},
                // Honour and arena totals, in the shapes their panels expect.
                // These three are fields the server already sends; the arena
                // and honour *currencies* below are too, and the ones that stay
                // zero are the ones no field carries.
                {"GetPVPLifetimeStats",   lua_GetPVPLifetimeStats},
                {"GetPVPSessionStats",    lua_GetPVPSessionStats},
                {"GetPVPYesterdayStats",  lua_GetPVPYesterdayStats},
                // Seven returns, as the battleground panel reads them:
                // status, map, instance, level range, team size, registered.
                // Stopping at three left teamSize nil and the panel formatted
                // a number from nothing.
                // GetBattlefieldStatus(queue) → status, map, instance, the level
                // range, the arena team size and whether it is a rated match.
                //
                // "none" for every queue, while SMSG_BATTLEFIELD_STATUS has been
                // parsed into three slots carrying exactly this. So the PVP
                // frame showed nothing queued however long the player had been
                // waiting, and battlefieldframe.lua's own test -
                // `queueStatus ~= "none"` - never passed.
                //
                // The status tokens are the four the interface compares against
                // and the ids are AzerothCore's BattlegroundStatus: 1 waiting in
                // the queue, 2 invited and waiting to accept, 3 in progress, 4
                // ending. Four reads as active because the battleground is
                // still the one the player is standing in.
                {"GetBattlefieldStatus", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetBattlefieldStatus(index)");   // 0x0054bc30
            auto* gh = getGameHandler(L);
            const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
            const char* status = "none";
            std::string mapName;
            uint32_t minLevel = 0, maxLevel = 0, instanceId = 0;
            uint8_t teamSize = 0;
            bool isRated = false;
            if (gh && idx >= 1 && idx <= 3) {
                const auto& q = gh->getBgQueues()[static_cast<size_t>(idx) - 1];
                switch (q.statusId) {
                    case 1: status = "queued";  break;
                    case 2: status = "confirm"; break;
                    case 3: case 4: status = "active"; break;
                    default: break;
                }
                if (q.statusId != 0) {
                    mapName = q.bgName;
                    teamSize = q.arenaType;
                    instanceId = q.instanceId;
                    isRated = q.isRated;
                    // The range arrives with the status itself. It used to be
                    // looked up in the available-battleground list, which only
                    // turns up at a battlemaster - so a queue joined anywhere
                    // else answered a range of zero to zero for as long as it
                    // lasted. The list is still the fallback, for a queue
                    // carried over from a login where the status came first.
                    minLevel = q.minLevel;
                    maxLevel = q.maxLevel;
                    if (minLevel == 0 && maxLevel == 0) {
                        for (const auto& bg : gh->getAvailableBgs()) {
                            if (bg.bgTypeId != q.bgTypeId) continue;
                            minLevel = bg.minLevel;
                            maxLevel = bg.maxLevel;
                            break;
                        }
                    }
                }
            }
            // An empty queue answers a nil map name, which is what the real
            // client does and what battlefieldframe.lua's `if ( mapName )` is
            // written for. Its own path rather than a conditional push, so the
            // seven values read in order on both - the return-order sweep reads
            // the sequence of pushes, and a branch in the middle of one is a
            // sequence it cannot follow.
            if (mapName.empty()) {
                lua_pushstring(L, status);
                lua_pushnil(L);
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                luaPushOneOrNil(L, 0);
                return 7;
            }
            lua_pushstring(L, status);
            lua_pushstring(L, mapName.c_str());
            // Both were answered as a flat zero with a comment saying the wire
            // does not carry them. It carries both - they sit between the map
            // type and the status, in the stretch this used to read two bytes
            // short of. The interface uses them: battlefieldframe.lua appends a
            // non-zero instance to the name, which is what makes a queue read
            // "Warsong Gulch 2".
            lua_pushnumber(L, instanceId);
            lua_pushnumber(L, minLevel);
            lua_pushnumber(L, maxLevel);
            lua_pushnumber(L, teamSize);
            // 1 or nil, not a number. 0x0054bc30 answers 1 or nil and the
            // interface asks `if ( registeredMatch )` - and zero is true in
            // Lua, so answering 0 for "not rated" labelled every arena queue
            // a rated match.
            luaPushOneOrNil(L, isRated ? 1 : 0);
            return 7;
        }},
                // The money a quest asks for, which the tracker compares
                // against the player's before calling an objective complete.
                // SetBattlefieldScoreFaction(faction) - show one side only.
                //
                // The scoreboard's tabs are a filter, not a sort: 1 is
                // Alliance, 0 is Horde, and nil is both. It has to reach the
                // two functions below, because the rows the frame draws are
                // the rows they hand back - the frame does no filtering of its
                // own and would otherwise show everyone under either tab.
                // SetBattlefieldScoreFaction([faction]) (0x0054c120): 0 or 1, or
                // anything not a number for both; any other number is ignored.
                // The rows are filtered as they are read, so the list follows
                // at once, as 0x0054aa30's refilter does.
                // T415: and then 0x0054aa30 - the board sorted again and
                // UPDATE_BATTLEFIELD_SCORE, which is what redraws the rows.
                {"SetBattlefieldScoreFaction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!lua_isnumber(L, 1)) {
                battlefieldScoreFaction(gh) = -1;
                announceBattlefieldScore(gh);
                return 0;
            }
            const int faction = static_cast<int>(lua_tonumber(L, 1));
            if (faction != -1 && (faction < 0 || faction > 1)) return 0;
            battlefieldScoreFaction(gh) = faction;
            announceBattlefieldScore(gh);
            return 0;
        }},
                // SortBattlefieldScoreData(type) (0x0054de00): the column by
                // name, without case - kills, deaths, cp, name, class, hk,
                // damage, healing, team, statN (N + 8, nothing at all past
                // stat8), anything else kills - to the front of the key list
                // (0x0054cee0: the front one turned round), then sorted and
                // UPDATE_BATTLEFIELD_SCORE (0x0054aa30); nothing while names
                // are owed (0x00bea590).
                {"SortBattlefieldScoreData", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usgae: SortBattlefieldScoreData(\"type\")");   // 0x0054de00
            const std::string column(lua_tostring(L, 1));
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            const auto is = [&](const char* name) {
                const size_t n = std::strlen(name);
                if (column.size() != n) return false;
                for (size_t i = 0; i < n; ++i)
                    if (lower(column[i]) != name[i]) return false;
                return true;
            };
            int key = 0;
            if (is("kills")) key = 0;
            else if (is("deaths")) key = 1;
            else if (is("cp")) key = 2;
            else if (is("name")) key = 3;
            else if (is("class")) key = 4;
            else if (is("hk")) key = 5;
            else if (is("damage")) key = 6;
            else if (is("healing")) key = 7;
            else if (is("team")) key = 8;
            else if (column.size() >= 4 && lower(column[0]) == 's' && lower(column[1]) == 't' &&
                     lower(column[2]) == 'a' && lower(column[3]) == 't') {
                key = std::atoi(column.c_str() + 4) + 8;
                if (key < 0 || key > 16) return 0;
            }
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (sb && sb->namesPending != 0) return 0;
            battlefieldSortBy(key);
            announceBattlefieldScore(gh);
            return 0;
        }},
                {"GetNumBattlefieldScores", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            lua_pushnumber(L, sb ? static_cast<double>(filteredBgScores(gh, *sb).size()) : 0.0);
            return 1;
        }},
                {"GetBattlefieldScore", [](lua_State* L) -> int {
            // GetBattlefieldScore(index) → name, killingBlows, honorableKills, deaths, honorGained, faction, rank, race, class, classToken, damageDone, healingDone
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (!sb) return luaReturnNil(L);
            const auto rows = filteredBgScores(gh, *sb);
            if (index < 1 || index > static_cast<int>(rows.size())) {
                return luaReturnNil(L);
            }
            const auto& p = *rows[static_cast<size_t>(index - 1)];
            lua_pushstring(L, p.name.c_str());     // name
            lua_pushnumber(L, p.killingBlows);      // killingBlows
            lua_pushnumber(L, p.honorableKills);    // honorableKills
            lua_pushnumber(L, p.deaths);            // deaths
            lua_pushnumber(L, p.bonusHonor);        // honorGained
            lua_pushnumber(L, bgScoreRowSide(gh, p));   // faction (0 Horde, 1 Alliance, -1)
            lua_pushnumber(L, 0);                   // rank
            // race, class and classToken from the name cache (0x0054be90):
            // the race's name, then class and token, nil and nil while the
            // class is not known - the scoreboard hides the class icon for a
            // nil token, and a guessed one drew a warrior beside everybody.
            {
                const int gender = gh->lookupPlayerGender(p.guid) == 1 ? 1 : 0;
                const uint8_t raceId = gh->lookupPlayerRace(p.guid);
                lua_pushstring(L, raceId ? localizedRaceName(gh, raceId, gender).c_str() : "");
                const uint8_t classId = gh->lookupPlayerClass(p.guid);
                if (const char* token = luaClassToken(classId)) {
                    lua_pushstring(L, localizedClassName(gh, classId, gender).c_str());
                    lua_pushstring(L, token);
                } else {
                    lua_pushnil(L);
                    lua_pushnil(L);
                }
            }
            lua_pushnumber(L, p.damageDone);        // damageDone
            lua_pushnumber(L, p.healingDone);       // healingDone
            return 12;
        }},
                // ---- The scoreboard's per-battleground columns ----
                //
                // Every battleground adds its own: flags captured in Warsong,
                // bases assaulted in Arathi, towers defended in Alterac. The
                // server names each one and sends a value per player, and this
                // client has parsed both into BgPlayerScore::bgStats all along
                // - nothing read them, and GetNumBattlefieldStats answered zero
                // from the counting stub, so the scoreboard drew the common
                // columns and stopped.
                // GetNumBattlefieldStats 0x00549f20 / GetBattlefieldStatInfo
                // 0x0054c170: the battleground's own columns, which the client
                // takes from WorldStateUI.dbc when the battle starts running -
                // text (+0x14), icon (+0x10), tooltip (+0x18) in the client's
                // language, three nils past the end. This counted the widest
                // server row and answered an English label table with no icon
                // (T391).
                {"GetNumBattlefieldStats", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(gh->battlefieldStatColumns().size()) : 0.0);
            return 1;
        }},
                {"GetBattlefieldStatInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetBattlefieldStatInfo(index)");   // 0x0054c170
            auto* gh = getGameHandler(L);
            const int64_t index = static_cast<int64_t>(std::llround(lua_tonumber(L, 1)));
            const auto columns = gh ? gh->battlefieldStatColumns() : std::vector<const game::world_state_ui::Row*>{};
            if (index < 1 || static_cast<size_t>(index) > columns.size()) {
                lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
                return 3;
            }
            const auto* row = columns[static_cast<size_t>(index - 1)];
            lua_pushstring(L, row->text.c_str());
            lua_pushstring(L, row->icon.c_str());
            lua_pushstring(L, row->tooltip.c_str());
            return 3;
        }},
                // GetBattlefieldStatData(playerIndex, statIndex) → the value.
                //
                // Always a number. The frame compares it against zero the line
                // after it asks, so nil is not "no value" there, it is an error.
                {"GetBattlefieldStatData", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return luaL_error(L, "Usage: GetBattlefieldStatData(playerIndex, statIndex)");   // 0x00549f60
            auto* gh = getGameHandler(L);
            const int playerIndex = static_cast<int>(luaL_checknumber(L, 1));
            const int statIndex   = static_cast<int>(luaL_checknumber(L, 2));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            double value = 0.0;
            if (sb && playerIndex >= 1 && playerIndex <= static_cast<int>(sb->players.size())) {
                const auto& stats = sb->players[static_cast<size_t>(playerIndex - 1)].bgStats;
                if (statIndex >= 1 && statIndex <= static_cast<int>(stats.size()))
                    value = static_cast<double>(stats[static_cast<size_t>(statIndex - 1)].second);
            }
            lua_pushnumber(L, value);
            return 1;
        }},
                // GetBattlefieldTeamInfo(faction) → name, rating, newRating, skill
                //
                // The three words MSG_PVP_LOG_DATA carries for the team, as
                // they came (0x0054a180).
                {"GetBattlefieldTeamInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetBattlefieldTeamInfo(index)");   // 0x0054a180
            auto* gh = getGameHandler(L);
            const int faction = static_cast<int>(luaL_checknumber(L, 1));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            const bool ok = sb && faction >= 0 && faction <= 1;
            const game::ArenaTeamScore none{};
            const auto& team = ok ? sb->arenaTeams[faction] : none;
            if (ok) lua_pushstring(L, team.teamName.c_str()); else lua_pushnil(L);
            lua_pushnumber(L, static_cast<double>(team.rating));
            lua_pushnumber(L, static_cast<double>(team.newRating));
            lua_pushnumber(L, static_cast<double>(team.matchmakerRating));
            return 4;
        }},
                // ReportPlayerIsPVPAFK(name) - flag someone as not taking part.
                //
                // Named rather than targeted: the scoreboard row knows who it
                // is showing but not their guid, so the name is matched against
                // the scoreboard this client already holds. A name that is not
                // on it cannot be reported, which is also true in the real
                // client - the report is only offered from a row.
                {"ReportPlayerIsPVPAFK", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: ReportPlayerIsPVPAFK(\"unit\")");   // 0x00612d50
            auto* gh = getGameHandler(L);
            const char* name = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (!gh || !sb || !name || !*name) return 0;
            for (const auto& p : sb->players) {
                if (p.name == name) { gh->reportPvpAfk(p.guid); return 0; }
            }
            return 0;
        }},
                {"GetBattlefieldWinner", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (sb && sb->hasWinner) lua_pushnumber(L, sb->winner);
            else lua_pushnil(L);
            return 1;
        }},
                {"RequestBattlefieldScoreData", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->requestPvpLog();
            return 0;
        }},
                {"AcceptBattlefieldPort", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: AcceptBattlefieldPort(index, accept)");   // 0x0054da10
            // AcceptBattlefieldPort(queue, accept): the queue counts from one,
            // and a refusal is also how a queue still waiting is left. The
            // queue used to be dropped, which sent every answer to whichever
            // slot held an invitation - so Leave Queue on a queue that had not
            // popped found none and did nothing.
            auto* gh = getGameHandler(L);
            const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
            // 0x0054da10 asks the taint check (kind 0x14: insecure code only
            // inside a hardware event) once it has its arguments.
            if (!taint::allowed(L, 0x14)) return 0;
            // Only the two slots there are: any other index answers nil and
            // sends nothing.
            if (idx < 1 || idx > 2) { lua_pushnil(L); return 1; }
            const uint32_t slot = static_cast<uint32_t>(idx - 1);
            // The answer read as the client reads a switch (0x00815500(L,2,0)):
            // StaticPopup's 'Abbrechen' passes 0, which lua_toboolean took for
            // a yes and so ported the player in (T393).
            const bool accept = luaClientSwitchArg(L, 2, false);
            // An acceptance outside a battleground (Map.dbc InstanceType 3,
            // 0x0054a700) is refused, said on the error line, nothing sent
            // and nil answered: UNIT_FLAG 0x800 (0x1c2 ERR_NOT_IN_COMBAT),
            // dead or a ghost (vtable +0x128, 0x87 ERR_PLAYER_DEAD), falling
            // (movement 0x1000, 0x296 ERR_NOT_WHILE_FALLING), on a transport
            // (vtable +0x40 via 0x0054a430, 0x88 ERR_CLIENT_LOCKED_OUT), the
            // fatigue timer running down (0x00513f20(0) scale < 0, 0x297
            // ERR_NOT_WHILE_FATIGUED).
            if (gh && accept && gh->currentMapInstanceType() != 3) {
                if (const auto me = gh->getEntityManager().getEntity(gh->getPlayerGuid())) {
                    const auto refuse = [&](const char* key, const char* enUS) {
                        gh->playGameErrorSound(key);
                        gh->addUIError(game::interfaceString(key, enUS));
                        lua_pushnil(L);
                        return 1;
                    };
                    const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
                    if (flags != 0xFFFF && (me->getField(flags) & 0x800u) != 0)
                        return refuse("ERR_NOT_IN_COMBAT", "You can't do that while in combat");
                    if (gh->isPlayerDead() || gh->isPlayerGhost())
                        return refuse("ERR_PLAYER_DEAD", "You can't do that when you're dead.");
                    if ((gh->getMovementInfo().flags & 0x1000u) != 0)
                        return refuse("ERR_NOT_WHILE_FALLING", "You can't do that while jumping or falling");
                    if (gh->getPlayerTransportGuid() != 0 || gh->getMovementInfo().transportGuid != 0)
                        return refuse("ERR_CLIENT_LOCKED_OUT", "You can't do that right now.");
                    if (gh->getMirrorTimer(0).scale < 0)
                        return refuse("ERR_NOT_WHILE_FATIGUED", "You can't do that while fatigued");
                }
            }
            if (gh) {
                if (accept) gh->acceptBattlefield(slot);
                else gh->declineBattlefield(slot);
            }
            lua_pushnumber(L, 1.0);  // 0x0054da10 answers 1 once it has sent
            return 1;
        }},
                {"UnitRace",          lua_UnitRace},
                {"UnitPowerType",     lua_UnitPowerType},
                {"UnitGUID",          lua_UnitGUID},
                {"UnitIsPlayer",      lua_UnitIsPlayer},
                {"UnitPVPName",       lua_UnitPVPName},
                {"InCombatLockdown",  lua_InCombatLockdown},
                {"CheckInteractDistance", lua_CheckInteractDistance},
                {"UnitInRange",           lua_UnitInRange},
                {"IsRaidLeader",          lua_IsRaidLeader},
                {"IsRealPartyLeader",     lua_IsRealGroupLeader},
                {"IsRealRaidLeader",      lua_IsRealGroupLeader},
                {"UnitIsFeignDeath",      lua_UnitIsFeignDeath},
                {"UnitIsTrivial",         lua_UnitIsTrivial},
                {"UnitIsPVPSanctuary",    lua_UnitIsPVPSanctuary},
                // The class token without the localised name beside it, which
                // is all UnitClassBase is: the same second return UnitClass
                // already computes.
                {"UnitClassBase",         lua_UnitClassBase},
                {"IsMounted",         lua_IsMounted},
                {"IsFlying",          lua_IsFlying},
                {"IsSwimming",        lua_IsSwimming},
                {"IsResting",         lua_IsResting},
                {"IsFalling",         lua_IsFalling},
                {"IsStealthed",       lua_IsStealthed},
                {"GetUnitSpeed",      lua_GetUnitSpeed},
                {"UnitAffectingCombat", lua_UnitAffectingCombat},
                {"GetNumRaidMembers",   lua_GetNumRaidMembers},
                {"GetNumPartyMembers",  lua_GetNumPartyMembers},
                // The counts before the dungeon finder inflates them. There is
                // no dungeon finder here, so they are the same number - and
                // UIParent does arithmetic on them, where absent is an error
                // rather than a zero.
                // The real group's counts (0x00573xxx / 0x0052c190): the
                // player's own group's, beside a battleground's roster.
                {"GetRealNumRaidMembers",  [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->realGroupRaidCount() : 0);
            return 1;
        }},
                {"GetRealNumPartyMembers", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->realGroupPartyCount() : 0);
            return 1;
        }},
                {"HasFullControl",      lua_HasFullControl},
                {"GetPetSpellBonusDamage", lua_GetPetSpellBonusDamage},
                {"RetrieveCorpse",      lua_RetrieveCorpse},
                {"Dismount",            lua_Dismount},
                {"StartAttack",         lua_StartAttack},
                {"StopAttack",          lua_StopAttack},
                {"UnitInParty",         lua_UnitInParty},
                {"UnitInRaid",          lua_UnitInRaid},
                {"UnitHasVehicleUI",    lua_UnitHasVehicleUI},
                {"UnitVehicleSkin",     lua_UnitVehicleSkin},
                {"UnitInVehicleControlSeat", lua_UnitInVehicleControlSeat},
                {"UnitInVehicle",       lua_UnitInVehicle},
                {"UnitControllingVehicle", lua_UnitControllingVehicle},
                {"UnitIsPossessed",     lua_UnitIsPossessed},
                {"UnitIsCharmed",       lua_UnitIsCharmed},
                {"UnitIsControlling",   lua_UnitIsControlling},
                {"UnitIsTalking",       lua_UnitIsTalking},
                {"UnitInBattleground",  lua_UnitInBattleground},
                {"UnitPlayerOrPetInParty", lua_UnitPlayerOrPetInParty},
                {"UnitPlayerOrPetInRaid",  lua_UnitPlayerOrPetInRaid},
                {"UnitCharacterPoints", lua_UnitCharacterPoints},
                {"PetHasActionBar",     lua_PetHasActionBar},
                {"PetCanBeDismissed",   lua_PetCanBeDismissed},
                {"PetCanBeAbandoned",   lua_PetCanBeAbandoned},
                {"GetRaidRosterInfo",   lua_GetRaidRosterInfo},
                {"GetThreatStatusColor", lua_GetThreatStatusColor},
                {"GetReadyCheckStatus", lua_GetReadyCheckStatus},
                {"GetReadyCheckTimeLeft", lua_GetReadyCheckTimeLeft},
                {"RegisterUnitWatch",   lua_RegisterUnitWatch},
                {"UnregisterUnitWatch", lua_UnregisterUnitWatch},
                {"UnitIsUnit",          lua_UnitIsUnit},
                {"UnitIsFriend",        lua_UnitIsFriend},
                {"UnitIsEnemy",         lua_UnitIsEnemy},
                {"UnitCreatureType",    lua_UnitCreatureType},
                {"UnitClassification",  lua_UnitClassification},
                {"UnitReaction",        lua_UnitReaction},
                {"UnitIsConnected",     lua_UnitIsConnected},
                {"GetComboPoints",      lua_GetComboPoints},
                {"GetPlayerInfoByGUID",  lua_GetPlayerInfoByGUID},
                {"UnitXP",                  lua_UnitXP},
                {"UnitXPMax",               lua_UnitXPMax},
                {"GetXPExhaustion",         lua_GetXPExhaustion},
                {"GetTimeToWellRested",     lua_GetTimeToWellRested},
                {"UnitAttackPower",         lua_UnitAttackPower},
                {"UnitRangedAttackPower",   lua_UnitRangedAttackPower},
                {"UnitDefense",             lua_UnitDefense},
                {"UnitAttackSpeed",         lua_UnitAttackSpeed},
                {"UnitDamage",              lua_UnitDamage},
                {"UnitRangedDamage",        lua_UnitRangedDamage},
                {"UnitRangedAttack",        lua_UnitRangedAttack},
                {"UnitAttackBothHands",     lua_UnitAttackBothHands},
                {"GetManaRegen",            lua_GetManaRegen},
                {"GetMaxCombatRatingBonus", lua_GetMaxCombatRatingBonus},
                {"GetUnitMaxHealthModifier", lua_GetUnitMaxHealthModifier},
                {"GetInventoryItemCooldown", lua_GetInventoryItemCooldown},
                {"SetPortraitTexture",         lua_SetPortraitTexture},
                {"SetLootPortrait",            lua_SetLootPortrait},
                {"GetHairCustomization",       lua_GetHairCustomization},
                {"GetFacialHairCustomization", lua_GetFacialHairCustomization},
                {"CanAlterSkin",               lua_CanAlterSkin},
                {"GetBarberShopStyleInfo",     lua_GetBarberShopStyleInfo},
                {"SetNextBarberShopStyle",     lua_SetNextBarberShopStyle},
                {"GetBarberShopTotalCost",     lua_GetBarberShopTotalCost},
                {"BarberShopReset",            lua_BarberShopReset},
                {"CancelBarberShop",           lua_CancelBarberShop},
                // The Okay button, named as an OnClick attribute rather than
                // called from a script body - which is exactly why the
                // readiness report called this element finished while its one
                // committing action raised.
                {"ApplyBarberShopStyle", [](lua_State* L) -> int {
            if (auto* svc = getLuaServices(L); svc && svc->barberApply) svc->barberApply();
            return 0;
        }},
                // HasWandEquipped() - a wand in the ranged slot, which the
                // character sheet needs because a wand's damage is read
                // differently from a bow's: PaperDollFrame_SetRangedDamage
                // takes the plain average and skips the attack-power bonus a
                // physical ranged weapon gets. Answering no meant a wand user's
                // ranged damage was computed as though the wand were a bow.
                //
                // Weapon class 2, subclass 19, off the same table the client's
                // own subclass names come from.
                {"HasWandEquipped",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { luaPushOneOrNil(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(game::EquipSlot::RANGED);
            bool wand = false;
            if (!sl.empty()) {
                if (const auto* info = gh->getItemInfo(sl.item.itemId); info && info->valid) {
                    wand = (info->itemClass == 2 && info->subClass == 19);
                }
            }
            luaPushOneOrNil(L, wand);
            return 1;
        }},
                // UnitHasRelicSlot(unit) - the four classes whose ranged slot
                // holds a relic instead of a weapon: paladin librams, death
                // knight sigils, shaman totems and druid idols. Answering no
                // for all of them made the paperdoll label that slot "Ranged"
                // and read its stats as a ranged weapon's.
                {"UnitHasRelicSlot",        [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitHasRelicSlot(\"unit\")");   // 0x00611330
            // A player unit whose class's ChrClasses flags hold 0x8
            // (0x00611330).
            auto* gh = getGameHandler(L);
            std::string uid(lua_tostring(L, 1));
            toLowerInPlace(uid);
            auto* unit = gh ? resolveUnit(L, uid.c_str()) : nullptr;
            if (!unit || unit->getType() != game::ObjectType::PLAYER) { luaPushOneOrNil(L, 0); return 1; }
            const uint8_t c = unitClassId(L, gh, uid);
            luaPushOneOrNil(L, classWearsRelics(gh, c));
            return 1;
        }},
                // Whether the repair cursor is up. A flat false meant the
                // merchant's repair button re-armed on every click instead of
                // toggling, and neither the bag nor the paperdoll tooltip ever
                // showed an item's repair cost.
                {"InRepairMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool active = repairCursorUp() && gh && gh->isVendorWindowOpen();
            luaPushOneOrNil(L, active ? 1 : 0);
            return 1;
        }},
                // IsInventoryItemLocked(slot) - that slot's item is on the
                // cursor, so the paperdoll greys it while it is in the air.
                // Answering no meant a picked-up item stayed drawn in the slot
                // it had already left.
                //
                // It shows as SetItemButtonDesaturated draws it: the icon
                // desaturated (Texture:SetDesaturated, Desaturate.bls).
                {"IsInventoryItemLocked",   [](lua_State* L) -> int {
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            auto* gh = getGameHandler(L);
            // The lock of the item in any of the player's inventory slots,
            // the bank's 28 (40-67) and the backpack's (24-39) too, as
            // GetContainerItemInfo answers for them (T371).
            uint64_t guid = 0;
            if (gh) {
                if (slot >= 1 && slot <= 23) guid = gh->getEquipSlotGuid(slot - 1);
                else if (slot >= 24 && slot <= 39) guid = gh->getBackpackItemGuid(slot - 24);
                else if (slot >= 40 && slot <= 67) guid = gh->getBankSlotGuid(slot - 40);
            }
            // On the cursor from the bank or the backpack, as the container
            // answer sees it (bag -1 or 0, its own slot).
            const auto& held = cursorItemSlot();
            const bool heldHere = !held.equipped &&
                                  ((slot >= 40 && slot <= 67 && held.bag == -1 && held.slot == slot - 39) ||
                                   (slot >= 24 && slot <= 39 && held.bag == 0 && held.slot == slot - 23));
            luaPushOneOrNil(L, slot > 0 && (cursorEquipSlot() == slot || heldHere ||
                                            (guid != 0 && itemLocked(gh, guid))));
            return 1;
        }},
                // GetInventoryItemBroken(unit, slot) - worn out, so the
                // paperdoll draws that slot's icon red. The durability is
                // already tracked and already answered by
                // GetInventoryItemDurability; only this was left saying no.
                {"GetInventoryItemBroken",  [](lua_State* L) -> int {
            // 0x005e9d80 -> 0x005e7cf0: the unit as a string, or the usage
            // line with the function's own name in it.
            if (!lua_isstring(L, 1))
                return luaL_error(L, "Usage: %s(unit, slot)", "GetInventoryItemBroken");
            auto* gh = getGameHandler(L);
            const int slotId = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || slotId < 1 || slotId > 19) { luaPushOneOrNil(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(
                static_cast<game::EquipSlot>(slotId - 1));
            luaPushOneOrNil(L, !sl.empty() && sl.item.maxDurability > 0 &&
                               sl.item.curDurability == 0);
            return 1;
        }},
                // CursorCanGoInSlot(slot) (0x005ea880 -> 0x006d4a00): whether
                // what the cursor holds can be put there - the paperdoll's, the
                // bags' and the bank's buttons light up by it. No value for a
                // slot there is none of.
                {"CursorCanGoInSlot",       [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int place = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || !game::item_slots::known(place)) return 0;
            const uint32_t held = cursorItemId();
            if (held == 0) {
                luaPushOneOrNil(L, place == game::item_slots::kAmmo && cursorHoldsAmmo());
                return 1;
            }
            // A bag with something in it - one that is worn - stays among them.
            if (const int bag = cursorEquipSlot() - 20; bag >= 0 && bag < game::Inventory::NUM_BAG_SLOTS &&
                                                        !game::item_slots::bagPlace(place)) {
                const auto& inventory = gh->getInventory();
                for (int i = 0; i < inventory.getBagSize(bag); ++i) {
                    if (!inventory.getBagSlot(bag, i).empty()) return luaReturnNil(L);
                }
            }
            luaPushOneOrNil(L, gh->itemFitsPlace(held, place, classWearsRelics(gh, unitClassId(L, gh, "player"))));
            return 1;
        }},
                // The pet tab of the character sheet, which is handed over -
                // PetPaperDollFrame_SetStats and PetExpBar_Update run whenever
                // it is shown, so with a pet out these raised on opening it.
                //
                // 0x006127f0 (health) and 0x00612900 (power): petStatModifier.
                {"GetUnitHealthModifier",   [](lua_State* L) -> int {
            lua_pushnumber(L, petStatModifier(getGameHandler(L),
                            resolveUnit(L, luaL_optstring(L, 1, "player")), false));
            return 1; }},
                {"GetUnitPowerModifier",    [](lua_State* L) -> int {
            lua_pushnumber(L, petStatModifier(getGameHandler(L),
                            resolveUnit(L, luaL_optstring(L, 1, "player")), true));
            return 1; }},
                {"GetPetExperience", [](lua_State* L) -> int {
            // currXP, nextXP - off the pet unit's own fields, not the player's.
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getPetExperience() : 0);
            lua_pushnumber(L, gh ? gh->getPetNextLevelExp() : 0);
            return 2;
        }},
                // GetCompanionCooldown(mode, index) → start, duration, enabled.
                //
                // A companion's cooldown is its summon spell's, the same
                // relationship GetTradeSkillCooldown reads for a recipe. The
                // comment that used to sit here said this client does not
                // enumerate companions, which stopped being true when
                // rebuildCompanions was written - GetCompanionInfo has listed
                // them, and CallCompanion below has summoned them, ever since.
                //
                // (start, duration), not (now, remaining): the cooldown frame
                // draws a sweep of `duration` beginning at `start`, so the
                // start is wound back by however much has already run.
                {"GetCompanionCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
            if (index > static_cast<int>(list.size())) return 0;
            // 0x0053e490: the companion's spell, from the history (0x00809000).
            return pushHistoryCooldown(L, gh->playerSpellCooldown(list[static_cast<size_t>(index) - 1].spellId));
        }},
                // Summoning a mount or a critter is casting its spell - there
                // is no separate companion message on the wire in 3.3.5.
                // CallCompanion(type, index) (0x0053e5e0): the usage line for a
                // bad type or a zero index; the index rounded.
                {"CallCompanion", [](lua_State* L) -> int {
            const int type = companionTypeArg(L, 1);
            const long idx = companionIndexArg(L, 2);
            if (type < 0 || idx == 0) return luaL_error(L, "Usage: CallCompanion(type, index)");
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(idx);
            if (!gh || index < 1) return 0;
            const auto& list = gh->getCompanions(type == 1);
            if (index <= static_cast<int>(list.size())) {
                gh->castSpell(list[static_cast<size_t>(index) - 1].spellId, 0);
            }
            return 0;
        }},
                // SummonRandomCritter() (0x0053e7e0 -> 0x006d7b30): one of
                // the player's critters at random (rand() % count), cast.
                {"SummonRandomCritter", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& list = gh->getCompanions(false);
            if (!list.empty())
                gh->castSpell(list[static_cast<size_t>(std::rand()) % list.size()].spellId, 0);
            return 0;
        }},
                // DismissCompanion(type) (0x0053e6b0): the usage line for a type
                // that is neither CRITTER nor MOUNT. Every companion of the
                // type in effect (0x00802cb0) is taken off (0x00802f80): a
                // critter by CMSG_DISMISS_CRITTER with the player's critter -
                // a summon has no aura to cancel - a mount by its aura.
                {"DismissCompanion", [](lua_State* L) -> int {
            const int type = companionTypeArg(L, 1);
            if (type < 0) return luaL_error(L, "Usage: DismissCompanion(type)");
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            std::vector<uint32_t> out;
            for (const auto& c : gh->getCompanions(type == 1))
                if (gh->companionInEffect(c.spellId)) out.push_back(c.spellId);
            for (uint32_t spell : out) {
                if (type == 1) gh->cancelAura(spell);
                else gh->dismissCritter();
            }
            return 0;
        }},
                // IsTitleKnown(bit), answered from the mask the realm sends
                // wherever it sends one: that is the client's own source
                // (0x0051b410 reads the six dwords of
                // PLAYER_FIELD_KNOWN_TITLES off the player object and nothing
                // else), and it is why titles earned before this session -
                // every title a character arrives with - were missing here.
                // knownTitleWords above says when there is a mask to read, and
                // known_titles.hpp which bit of which dword a title id is.
                //
                // Where the layout names no run, the titles earned in this
                // session stay the answer: SMSG_TITLE_EARNED is the only title
                // news those realms send, and an earned title that is not
                // listed is a title the character cannot put on.
                //
                // A number rather than a boolean, because paperdollframe.lua
                // asks `IsTitleKnown(i) ~= 0` and false compares unequal to
                // zero - which is the trap that made this a zero rather than a
                // false in the first place. The loop asking it runs
                // 1..GetNumTitles, which is the CharTitles table's row count
                // plus one and not the whole space the six dwords cover, so an
                // id past the table is one the sheet never asks about; every id
                // the table has a row for comes through here.
                {"IsTitleKnown",            [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int bit = static_cast<int>(luaL_optnumber(L, 1, -1));
            game::KnownTitleWords words{};
            if (knownTitleWords(gh, words)) {
                // An id of nothing at all - a negative number, or 0, which the
                // client rejects outright - asks for no bit and is not known.
                lua_pushnumber(L, game::titleIsKnown(words, static_cast<uint32_t>(bit)) ? 1 : 0);
                return 1;
            }
            const bool known = gh && bit >= 0 &&
                               gh->getKnownTitleBits().count(static_cast<uint32_t>(bit)) > 0;
            lua_pushnumber(L, known ? 1 : 0);
            return 1; }},
                {"GetCombatRatingBonus",    lua_GetCombatRatingBonus},
                {"GetCritChanceFromAgility", lua_GetCritChanceFromAgility},
                {"GetSpellCritChanceFromIntellect", lua_GetSpellCritChanceFromIntellect},
                // Three values - main hand, off hand, ranged - because the
                // character sheet reads the second and concatenates it. One
                // value left it nil, and the line that prints "expertise /
                // off-hand expertise" raised rather than printing.
                // GetExpertise() → expertise points, main hand and off hand.
                //
                // Two values, not three: the character sheet destructures
                // `local expertise, offhandExpertise = GetExpertise()` and
                // prints them either side of a slash when an off hand is being
                // swung. The third was never read, and both of the first two
                // were zero for everyone - the server sends them, in points,
                // and nothing here was reading the fields.
                {"GetExpertise", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getExpertise() : 0);
            lua_pushnumber(L, gh ? gh->getOffhandExpertise() : 0);
            return 2;
        }},
                // Two, for the same reason and under the same guard, four
                // lines below the call above: the character sheet formats the
                // off-hand percentage whenever an off-hand is equipped, and
                // format("%.2f", nil) raises. Fixing GetExpertise alone left
                // every dual-wielder's stat panel broken on the next line.
                // A point of expertise is a quarter of a percent off the
                // chance to be dodged or parried, which is the arithmetic the
                // sheet's tooltip line does not do for itself - it formats
                // whatever it is given with two decimal places and a % sign.
                {"GetExpertisePercent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, (gh ? gh->getExpertise() : 0) * 0.25);
            lua_pushnumber(L, (gh ? gh->getOffhandExpertise() : 0) * 0.25);
            return 2;
        }},
                {"GetArmorPenetration",     lua_GetArmorPenetration},
                // Both of these are player fields the server has already summed
                // from the item mods, not figures the client works out from the
                // bags - the note here used to say the latter, and the client
                // reads a field in both cases (see the two bindings).
                {"GetSpellPenetration",     lua_GetSpellPenetration},
                {"GetShieldBlock",          lua_GetShieldBlock},
                {"GetUnitHealthRegenRateFromSpirit", lua_GetUnitHealthRegenRateFromSpirit},
                {"GetUnitManaRegenRateFromSpirit",   lua_GetUnitManaRegenRateFromSpirit},
                {"GetWatchedFactionInfo",   lua_GetWatchedFactionInfo},
                {"GetNumBagSlots",          lua_GetNumBagSlots},
                {"GetMirrorTimerProgress",  lua_GetMirrorTimerProgress},
                {"GetRestState",            lua_GetRestState},
                {"HasFocus", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->hasFocus() ? 1 : 0);
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons

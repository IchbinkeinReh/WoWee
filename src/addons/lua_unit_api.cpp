// lua_unit_api.cpp - Unit query, stats, party/raid, and player state Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/shapeshift_forms.hpp"
#include "game/battleground_flag.hpp"
#include "game/game_utils.hpp"
#include "game/bg_score_defs.hpp"
#include "game/pvp_flags.hpp"
#include "rendering/selection_circle.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_taint.hpp"
#include "game/power_prediction.hpp"
#include "game/unit_level_display.hpp"
#include "game/known_titles.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <bit>
#include "game/interface_text.hpp"

namespace wowee::addons {

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
    auto* ghName = getGameHandler(L);
    const std::string shown = unit && ghName ? ghName->shownUnitName(*unit) : unit ? unit->getName() : std::string{};
    if (unit && !shown.empty()) {
        lua_pushstring(L, shown.c_str());
    } else {
        // Fallback: party member name for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findRosterMember(gh, guid);
        if (pm && !pm->name.empty()) {
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
    lua_pushnil(L);  // the realm: none for one's own
    return 2;
}


static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getHealth());
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curHealth : 0);
    }
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
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
        lua_pushnumber(L, pm ? pm->maxHealth : 0);
    }
    return 1;
}

/// 0x007fde00: what a power is shown over - rage and runic power in tenths,
/// happiness in thousandths (table 0x00af5220).
static uint32_t powerDivisor(int type) {
    static constexpr uint32_t kDivisor[7] = {1, 10, 1, 1, 1000, 1, 10};
    return type >= 0 && type < 7 ? kDivisor[type] : 1;
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
        const bool answers = gh && type < 7 && (type == unit->getPowerType() || unit->getGuid() == gh->getPlayerGuid());
        lua_pushnumber(L, answers ? static_cast<uint32_t>(gh->unitPower(*unit, type)) / powerDivisor(type) : 0);
    } else {
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        // Party stats carry one power and say which it is, so a request for a
        // different one is answered with nothing rather than with that.
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
    const char* uid = luaL_optstring(L, 1, "player");
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
        lua_pushboolean(L, 1);
    } else {
        // Party members in other zones don't have entities but still "exist"
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        lua_pushboolean(L, guid != 0 && findRosterMember(gh, guid) != nullptr);
    }
    return 1;
}

static int lua_UnitIsDead(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, unit->getHealth() == 0);
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushboolean(L, pm ? (pm->curHealth == 0 && pm->maxHealth > 0) : 0);
    }
    return 1;
}

static int lua_UnitClass(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, uid);
    if (unit && gh) {

        uint8_t classId = 0;
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        if (uidStr == "player") {
            classId = gh->getPlayerClass();
        } else {
            // Read class from UNIT_FIELD_BYTES_0 (class is byte 1)
            uint64_t guid = resolveUnitGuid(gh, uidStr);
            if (guid != 0) {
                auto entity = gh->getEntityManager().getEntity(guid);
                if (entity) {
                    uint32_t bytes0 = entity->getField(
                        game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                    classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
                }
            }
            // Fallback: check name query class/race cache
            if (classId == 0 && guid != 0) {
                classId = gh->lookupPlayerClass(guid);
            }
        }
        // Nothing at all when the class is not known, because that is what WoW
        // answers for a unit it cannot see and what every caller is written
        // against: `local _, class = UnitClass(self.unit); if ( class ) then`.
        //
        // "UNKNOWN" passes that guard and then indexes nothing -
        // CLASS_ICON_TCOORDS has no such key - so the line after it unpacked a
        // nil. ArenaEnemyFrame_OnLoad does exactly this, which took the whole
        // of Blizzard_ArenaUI down at load. A binding that answers a constant
        // where the real one answers nothing defeats every guard written for
        // it, and the guard is the caller saying it already knows how to cope.
        //
        // The token decides: it is the one thing that knows which class ids
        // WoW actually uses, and the two callers in the social API ask it the
        // same question for the same reason.
        const char* token = luaClassToken(classId);
        if (!token) return 0;
        // Second is the uppercase token, not the display name again. Every
        // class-indexed table in FrameXML is keyed by it, so answering "Mage"
        // where "MAGE" was meant looked up nothing: SetPortraitTexture found no
        // CLASS_ICON_TCOORDS entry and fell back to the placeholder portrait,
        // which is the blue question mark on the character sheet.
        lua_pushstring(L, kLuaClasses[classId]);
        lua_pushstring(L, token);
        lua_pushnumber(L, classId);
        return 3;
    }
    // The unit does not exist, which is the case WoW answers nothing for. See
    // above: a constant here reads as a class the caller then looks up.
    return 0;
}

// UnitIsGhost(unit) - true if unit is in ghost form
static int lua_UnitIsGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isPlayerGhost());
    } else {
        // Check UNIT_FIELD_FLAGS for UNIT_FLAG_GHOST (0x00000100) - best approximation
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool ghost = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                // Ghost is PLAYER_FLAGS bit 0x10, NOT UNIT_FIELD_FLAGS bit 0x100
                // (which is UNIT_FLAG_IMMUNE_TO_PC - would flag immune NPCs as ghosts).
                uint32_t pf = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
                ghost = (pf & 0x00000010) != 0;
            }
        }
        lua_pushboolean(L, ghost);
    }
    return 1;
}

// UnitIsDeadOrGhost(unit)
static int lua_UnitIsDeadOrGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    auto* gh = getGameHandler(L);
    bool dead = (unit && unit->getHealth() == 0);
    if (!dead && gh) {
        std::string uidStr(uid);
        toLowerInPlace(uidStr);
        if (uidStr == "player") dead = gh->isPlayerGhost() || gh->isPlayerDead();
    }
    lua_pushboolean(L, dead);
    return 1;
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
static int pushPlayerFlagPredicate(lua_State* L, uint32_t bit) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            if (playerFlags & bit) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int lua_UnitIsAFK(lua_State* L) {
    return pushPlayerFlagPredicate(L, 0x01);
}

static int lua_UnitIsDND(lua_State* L) {
    return pushPlayerFlagPredicate(L, 0x02);
}

// UnitPlayerControlled(unit) - true for players and player-controlled pets
static int lua_UnitPlayerControlled(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { return luaReturnFalse(L); }
    // Players are always player-controlled; pets check UNIT_FLAG_PLAYER_CONTROLLED (0x01000000)
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushboolean(L, 1);
    } else {
        uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
        lua_pushboolean(L, (flags & 0x01000000) != 0);
    }
    return 1;
}

// UnitIsTapped(unit) - true if mob is tapped (tagged by any player)
static int lua_UnitIsTapped(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0004) != 0); // UNIT_DYNFLAG_TAPPED_BY_PLAYER
    return 1;
}

// UnitIsTappedByPlayer(unit) - true if tapped by the local player (can loot)
static int lua_UnitIsTappedByPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    uint32_t df = unit->getDynamicFlags();
    // Tapped by player: has TAPPED flag but also LOOTABLE or TAPPED_BY_ALL
    bool tapped = (df & 0x0004) != 0;
    bool lootable = (df & 0x0001) != 0;
    bool sharedTag = (df & 0x0008) != 0;
    lua_pushboolean(L, tapped && (lootable || sharedTag));
    return 1;
}

// UnitIsTappedByAllThreatList(unit) - true if shared-tag mob
static int lua_UnitIsTappedByAllThreatList(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { return luaReturnFalse(L); }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0008) != 0);
    return 1;
}

namespace {

/// A unit's standing on a mob's threat list: the status WoW reports, its raw
/// threat, and the leader's.
///
/// One place, because UnitThreatSituation and UnitDetailedThreatSituation must
/// agree - the indicator's colour comes from the first and its number from the
/// second, and two copies of the rule would eventually disagree about which
/// unit is tanking while the number beside it said otherwise.
///
/// Answers false when no list has arrived for this mob, which is not the same
/// as being absent from one: the caller falls back to guessing from whom the
/// mob is attacking, and only a list that exists can say "not on it".
struct ThreatStanding {
    int status = 0;          ///< 0 none, 1 threat, 2 insecurely, 3 securely tanking
    uint32_t threat = 0;
    uint32_t topThreat = 0;
    bool onList = false;
};

bool threatStandingFor(game::GameHandler* gh, uint64_t unitGuid, uint64_t mobGuid,
                       ThreatStanding& out) {
    if (!gh || unitGuid == 0 || mobGuid == 0) return false;
    const auto* list = gh->getThreatList(mobGuid);
    if (!list || list->empty()) return false;
    out.topThreat = (*list)[0].threat;
    for (size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i].victimGuid != unitGuid) continue;
        out.onList = true;
        out.threat = (*list)[i].threat;
        if (i != 0) { out.status = 1; return true; }
        // Securely tanking only when the lead is clear. WoW's own rule is a
        // percentage margin over the runner-up; at the head with someone close
        // behind it is "insecurely", which is what turns the indicator amber
        // before it turns red.
        const uint32_t next = (list->size() > 1) ? (*list)[1].threat : 0;
        out.status = (list->size() > 1 && next * 11 >= out.topThreat * 10) ? 2 : 3;
        return true;
    }
    return true;  // the list exists and this unit is not on it
}

}  // namespace

// UnitThreatSituation(unit, mobUnit) → 0=not tanking, 1=not tanking but threat, 2=insecurely tanking, 3=securely tanking
static int lua_UnitThreatSituation(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t playerUnitGuid = resolveUnitGuid(gh, uidStr);
    if (playerUnitGuid == 0) { return luaReturnZero(L); }
    // If no mob specified, check general combat threat against current target
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        mobGuid = resolveUnitGuid(gh, mStr);
    }
    // The mob's own threat list first, which is what the server actually sent.
    // This used to go straight to the guess below because the list was built
    // from a misread packet and was never worth consulting - SMSG_THREAT_UPDATE
    // was read as though it carried the second guid that only
    // SMSG_HIGHEST_THREAT_UPDATE has.
    ThreatStanding standing;
    if (threatStandingFor(gh, playerUnitGuid, mobGuid, standing)) {
        lua_pushnumber(L, standing.status);
        return 1;
    }
    // Approximate threat: check if the mob is targeting this unit
    if (mobGuid != 0) {
        auto mobEntity = gh->getEntityManager().getEntity(mobGuid);
        if (mobEntity) {
            if (game::unitTargetGuid(*mobEntity) == playerUnitGuid) {
                lua_pushnumber(L, 3); // securely tanking
                return 1;
            }
        }
    }
    // Check if player is in combat (basic threat indicator)
    if (playerUnitGuid == gh->getPlayerGuid() && gh->isInCombat()) {
        lua_pushnumber(L, 1); // in combat but not tanking
        return 1;
    }
    lua_pushnumber(L, 0);
    return 1;
}

// UnitDetailedThreatSituation(unit, mobUnit) → isTanking, status, threatPct, rawThreatPct, threatValue
static int lua_UnitDetailedThreatSituation(lua_State* L) {
    // Use UnitThreatSituation logic for the basics
    auto* gh = getGameHandler(L);
    if (!gh) {
        lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 5;
    }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t unitGuid = resolveUnitGuid(gh, uidStr);
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        toLowerInPlace(mStr);
        mobGuid = resolveUnitGuid(gh, mStr);
    }

    ThreatStanding standing;
    if (threatStandingFor(gh, unitGuid, mobGuid, standing)) {
        const bool isTanking = (standing.status >= 2);
        double rawPct = 0.0;
        if (standing.topThreat > 0)
            rawPct = 100.0 * standing.threat / standing.topThreat;
        // Against the point where aggro changes hands rather than against the
        // leader: WoW pulls at a tenth clear of the current tank, so a hundred
        // here means "about to take it" and is what the numeric indicator
        // colours on. Whoever holds it is already at a hundred by definition.
        const double pullPct = isTanking ? 100.0 : std::min(100.0, rawPct / 1.1);
        lua_pushboolean(L, isTanking);
        lua_pushnumber(L, standing.status);
        lua_pushnumber(L, pullPct);
        lua_pushnumber(L, rawPct);
        lua_pushnumber(L, standing.threat);
        return 5;
    }

    // No list for this mob yet. The same guess UnitThreatSituation falls back
    // to, and the same lack of a number behind it.
    bool isTanking = false;
    int status = 0;
    if (unitGuid != 0 && mobGuid != 0) {
        auto mobEnt = gh->getEntityManager().getEntity(mobGuid);
        if (mobEnt && game::unitTargetGuid(*mobEnt) == unitGuid) {
            isTanking = true;
            status = 3;
        }
    }
    lua_pushboolean(L, isTanking);
    lua_pushnumber(L, status);
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // threatPct
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // rawThreatPct
    lua_pushnumber(L, 0); // threatValue - unknown without the list
    return 5;
}

// CheckInteractDistance(unit, distIndex) → boolean
// distIndex: 1=inspect(28yd), 2=trade(11yd), 3=duel(10yd), 4=follow(28yd)
static int lua_CheckInteractDistance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_checkstring(L, 1);
    int distIdx = static_cast<int>(luaL_optnumber(L, 2, 4));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { return luaReturnFalse(L); }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    float maxDist = 28.0f; // default: follow/inspect range
    switch (distIdx) {
        case 1: maxDist = 28.0f; break; // inspect
        case 2: maxDist = 11.11f; break; // trade
        case 3: maxDist = 9.9f; break; // duel
        case 4: maxDist = 28.0f; break; // follow
    }
    lua_pushboolean(L, dist <= maxDist);
    return 1;
}

// UnitInRange(unit) → inRange, checkedOk
//
// The raid frames dim a member who has gone too far to help. Forty yards is
// what WoW means by it - the range most party-useful spells share - and the
// second return says whether the question could be answered at all: a member
// on another part of the map has no entity here, and dimming them for being
// out of range would be a guess dressed as a measurement.
static int lua_UnitInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    const uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto other  = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!other || !player) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);   // could not be checked
        return 2;
    }
    const float dx = player->getX() - other->getX();
    const float dy = player->getY() - other->getY();
    const float dz = player->getZ() - other->getZ();
    lua_pushboolean(L, (dx*dx + dy*dy + dz*dz) <= (40.0f * 40.0f));
    lua_pushboolean(L, 1);
    return 2;
}

// IsRaidLeader() / IsPartyLeader-style check against the group's leader guid.
static int lua_IsRaidLeader(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const auto& group = gh->getPartyData();
    lua_pushboolean(L, group.leaderGuid != 0 &&
                       group.leaderGuid == gh->getPlayerGuid() ? 1 : 0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)
static int lua_UnitIsVisible(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit != nullptr);
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
    const char* uid = luaL_optstring(L, 1, "player");
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
    // Not in the party, or in no party at all: no role, and the icon is hidden
    // rather than guessed at.
    return pushRoles(false, false, false);
}

// UnitCanAttack(unit, otherUnit) → boolean
static int lua_UnitCanAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == 0 || g2 == 0 || g1 == g2) { return luaReturnFalse(L); }
    // 0x0060d730 asks 0x00729740 with unit1 attacking.
    lua_pushboolean(L, gh->unitMayAttack(g1, g2));
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
static int lua_UnitCreatureFamily(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) { return luaReturnNil(L); }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { return luaReturnNil(L); }
    uint32_t family = gh->getCreatureFamily(unit->getEntry());
    if (family == 0) { return luaReturnNil(L); }
    static constexpr const char* kFamilies[] = {
        "", "Wolf", "Cat", "Spider", "Bear", "Boar", "Crocolisk", "Carrion Bird",
        "Crab", "Gorilla", "Raptor", "", "Tallstrider", "", "", "Felhunter",
        "Voidwalker", "Succubus", "", "Doomguard", "Scorpid", "Turtle", "",
        "Imp", "Bat", "Hyena", "Bird of Prey", "Wind Serpent", "", "Dragonhawk",
        "Ravager", "Warp Stalker", "Sporebat", "Nether Ray", "Serpent", "Moth",
        "Chimaera", "Devilsaur", "Ghoul", "Silithid", "Worm", "Rhino", "Wasp",
        "Core Hound", "Spirit Beast"
    };
    lua_pushstring(L, (family < sizeof(kFamilies)/sizeof(kFamilies[0]) && kFamilies[family][0])
        ? kFamilies[family] : "Beast");
    return 1;
}

// UnitOnTaxi(unit) → boolean (true if on a flight path)
static int lua_UnitOnTaxi(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isOnTaxiFlight());
    } else {
        lua_pushboolean(L, 0); // Can't determine for other units
    }
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
    lua_pushnumber(L, player ? -static_cast<double>(static_cast<int32_t>(player->getField(index))) : 0.0);
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

// GetSpellCritChance(school) → percent  (1=Holy,2=Fire,3=Nature,4=Frost,5=Shadow,6=Arcane)
static int lua_GetSpellCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    int school = static_cast<int>(luaL_checknumber(L, 1));
    float v = gh ? gh->getSpellCritPct(school) : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCombatRating(ratingIndex) → value.
// The FrameXML CR_* constants are 1-based (CR_WEAPON_SKILL=1 … CR_ARMOR_PENETRATION=25),
// but the player-field array and the combat game tables are 0-based (AzerothCore's
// CombatRating enum, weapon skill at 0). Convert at the boundary or every rating reads
// its neighbour's slot.
static int lua_GetCombatRating(lua_State* L) {
    auto* gh = getGameHandler(L);
    int cr = static_cast<int>(luaL_checknumber(L, 1)) - 1;
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
    const int school = static_cast<int>(luaL_checkinteger(L, 1));
    if (school < 1 || school > 7)
        return luaL_error(L, "bad argument #1 to 'GetSpellBonusDamage' (number expected, got %s)",
                          lua_typename(L, lua_type(L, 1)));
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

// GetMeleeHaste / GetAttackPowerForStat stubs for addon compat
static int lua_GetAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getMeleeAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);  // base
    lua_pushnumber(L, 0);   // posBuff
    lua_pushnumber(L, 0);   // negBuff
    return 3;
}


static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); lua_pushstring(L, "Unknown"); lua_pushnumber(L, 0); return 3; }
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);

    const uint8_t raceId = unitRaceOf(gh, uid);
    const bool known = (raceId > 0 && raceId < 12);
    const char* name = known ? kLuaRaces[raceId] : "Unknown";
    const char* file = known ? kLuaRaceFileNames[raceId] : "Unknown";
    lua_pushstring(L, name);      // 1: localized race
    lua_pushstring(L, file);      // 2: race file name, spliced into asset paths
    lua_pushnumber(L, raceId);    // 3: raceId (WoW returns 3 values)
    return 3;
}

static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);

    if (unit) {
        uint8_t pt = unit->getPowerType();
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
    if (pm) {
        uint8_t pt = pm->powerType;
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kLuaPowerNames[pt] : "MANA");
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushstring(L, "MANA");
    return 2;
}

static int lua_UnitGUID(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
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

static int lua_UnitIsPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto entity = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    lua_pushboolean(L, entity && entity->getType() == game::ObjectType::PLAYER);
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
    if (gh && guid && gh->vehicleRoster().seatState(guid) == 3) lua_pushnumber(L, 1.0);
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
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    lua_pushboolean(L, guidField(gh, guid, game::UF::UNIT_FIELD_CHARM) != 0);
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
    const char* uid = luaL_optstring(L, 1, "player");
    const uint64_t guid = gh ? resolveUnitGuid(gh, uid) : 0;
    lua_pushboolean(L, guidField(gh, guid, game::UF::UNIT_FIELD_CHARMEDBY) != 0);
    return 1;
}
static int lua_UnitIsTalking(lua_State* L) { return luaReturnFalse(L); }
static int lua_UnitInBattleground(lua_State* L) { return luaReturnNil(L); }

/// True for a unit in the player's own group, and for its pet. The party
/// frames use this to decide whether a unit is one of ours at all.
static int lua_UnitPlayerOrPetInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    // "partypet2" asks about party member 2's pet, and the answer is whatever
    // holds for the member.
    const size_t petPos = uidStr.find("pet");
    if (petPos != std::string::npos) uidStr.erase(petPos, 3);
    if (uidStr.empty() || uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
        return 1;
    }
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    const auto& pd = gh->getPartyData();
    bool found = false;
    for (const auto& m : pd.members) {
        if (m.guid == guid) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

static int lua_UnitPlayerOrPetInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !game::groupIsRaid(gh->getPartyData().groupType)) { return luaReturnFalse(L); }
    return lua_UnitPlayerOrPetInParty(L);
}

/// Talent points, and the two the interface actually reads: unspent points and
/// the total spent. The micro button flashes on the first.
// UnitCharacterPoints(unit) → talent points, primary professions still learnable
//
// Two values. The trainer's confirmation reads the second bare -
// `if ( cp2 < MAX_LEARNABLE_PROFESSIONS )` - to decide whether it is offering a
// first profession or a second, so returning one value made confirming a
// profession an error rather than a question.
//
// Two is the limit, and a primary profession is category 11 in SkillLine.dbc,
// which is the same test isProfessionSpell uses rather than a second opinion
// about what counts.
static int lua_UnitCharacterPoints(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (!gh || uidStr != "player") { lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 2; }

    constexpr int kMaxPrimaryProfessions = 2;
    int known = 0;
    for (const auto& [skillId, skill] : gh->getPlayerSkills()) {
        (void)skill;
        if (gh->getSkillCategory(skillId) == 11) ++known;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(gh->getUnspentTalentPoints()));
    lua_pushinteger(L, std::max(0, kMaxPrimaryProfessions - known));
    return 2;
}

static int lua_PetHasActionBar(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasPet());
    return 1;
}
static int lua_PetCanBeDismissed(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasPet());
    return 1;
}
// Abandoning is a hunter's pet, and nothing here knows which kind it has yet.
static int lua_PetCanBeAbandoned(lua_State* L) { return luaReturnFalse(L); }

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
    lua_pushboolean(L, gh && gh->isMounted());
    return 1;
}

static int lua_IsFlying(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerFlying());
    return 1;
}

static int lua_IsSwimming(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isSwimming());
    return 1;
}

static int lua_IsResting(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerResting());
    return 1;
}

static int lua_IsFalling(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Check FALLING movement flag
    if (!gh) { return luaReturnFalse(L); }
    const auto& mi = gh->getMovementInfo();
    lua_pushboolean(L, (mi.flags & 0x2000) != 0); // MOVEFLAG_FALLING = 0x2000
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
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (!gh || std::string(uid) != "player") {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getServerRunSpeed());
    return 1;
}

// --- Container/Bag API ---
// WoW bags: container 0 = backpack (16 slots), containers 1-4 = equipped bags


static int lua_UnitXP(lua_State* L) {
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
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string u(uid);
    toLowerInPlace(u);
    if (u == "player") {
        uint32_t nlxp = gh->getPlayerNextLevelXp();
        lua_pushnumber(L, nlxp > 0 ? nlxp : 1);
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

// GetXPExhaustion() → rested XP pool remaining (nil if none)
static int lua_GetXPExhaustion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t rested = gh->getPlayerRestedXp();
    if (rested > 0) lua_pushnumber(L, rested);
    else lua_pushnil(L);
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

/// GetMirrorTimerProgress(timer) → milliseconds left on breath, fatigue or
/// feign death. None of the three is tracked yet, and the honest answer is
/// zero rather than a stub that reads as a timer running.
/// GetMirrorTimerProgress(timer) → milliseconds left on that timer.
///
/// It answered a constant zero, and MirrorTimerFrame_OnUpdate divides it by a
/// thousand and calls SetValue with the result on every frame - so the breath
/// bar was reset to empty as fast as it could be drawn. An empty frame with a
/// label was the whole of what it could ever show.
///
/// Named, not numbered: WoW passes the timer's name through the event and
/// FrameXML hands the same string back here.
static int lua_GetMirrorTimerProgress(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string name(luaL_optstring(L, 1, ""));
    toLowerInPlace(name);
    int type = -1;
    if (name == "exhaustion") type = 0;
    else if (name == "breath") type = 1;
    else if (name == "death") type = 2;
    if (!gh || type < 0) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getMirrorTimer(type).value);
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
static int lua_UnitDefense(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    constexpr int kCrDefenseSkill = 1;  // 0-based CombatRating enum
    const int modifier = gh ? static_cast<int>(gh->getCombatRatingBonus(kCrDefenseSkill)) : 0;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, modifier);
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
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    return 2;
}

/// UnitAttackBothHands(unit) → main skill, main modifier, off skill, off
/// modifier.
static int lua_UnitAttackBothHands(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int level = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, level * 5);
    lua_pushnumber(L, 0);
    return 4;
}

/// GetManaRegen() → while not casting, while casting. Both per second; the
/// paperdoll multiplies by five to show mana-per-5-seconds. The server sends
/// both figures already computed (UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER and its
/// interrupted twin), so this reads them rather than modelling the spirit and
/// intellect formula - it was a flat zero, so every caster's mana regen read 0.
static int lua_GetManaRegen(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getManaRegen() : 0.0);
    lua_pushnumber(L, gh ? gh->getManaRegenCasting() : 0.0);
    return 2;
}

// GetCritChanceFromAgility([unit]) → the melee crit percent the player's Agility
// gives, from the combat game tables. Was a flat zero, so the crit stat's
// flyout read "0.00% from Agility" for every character.
static int lua_GetCritChanceFromAgility(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getMeleeCritFromAgility() : 0.0);
    return 1;
}

// GetSpellCritChanceFromIntellect([unit]) → the spell crit percent Intellect
// gives, from the spell-crit game tables. Was a flat zero.
static int lua_GetSpellCritChanceFromIntellect(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getSpellCritFromIntellect() : 0.0);
    return 1;
}

// GetCombatRatingBonus(ratingIndex) → the percent that combat rating grants.
// The paperdoll reads it for dodge, parry, haste, defense, weapon skill and the
// other rating-based stats; it was a flat zero for all of them.
static int lua_GetCombatRatingBonus(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same 1-based CR_* → 0-based table convention as GetCombatRating above.
    const int cr = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
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
    lua_pushnumber(L, gh ? gh->getCombatRatingBonus(kCrArmorPenetration) : 0.0);
    return 1;
}

// Health / mana regenerated per second from Spirit - the pair the Spirit stat
// flyout shows. Were flat zeros. Only the player's Spirit and class are tracked,
// so any other unit answers zero rather than the player's value under its name.
static int lua_GetUnitHealthRegenRateFromSpirit(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string unit(luaL_optstring(L, 1, "player"));
    toLowerInPlace(unit);
    lua_pushnumber(L, (gh && unit == "player") ? gh->getHealthRegenFromSpirit() : 0.0);
    return 1;
}
static int lua_GetUnitManaRegenRateFromSpirit(lua_State* L) {
    auto* gh = getGameHandler(L);
    std::string unit(luaL_optstring(L, 1, "player"));
    toLowerInPlace(unit);
    lua_pushnumber(L, (gh && unit == "player") ? gh->getManaRegenFromSpirit() : 0.0);
    return 1;
}

/// GetUnitMaxHealthModifier(unit) → the multiplier on maximum health. One,
/// because nothing here modifies it - and one rather than zero because the
/// sheet multiplies by it.
static int lua_GetUnitMaxHealthModifier(lua_State* L) {
    lua_pushnumber(L, 1.0);
    return 1;
}

/// GetInventoryItemCooldown(unit, slot) → start, duration, enable
/// (0x005e7e60): the player's item there, with a use spell, as that spell
/// cast from it (0x008090c0); else 0, 0, 0.
static int lua_GetInventoryItemCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (gh && slot >= 1 && slot <= 19) {
        const auto& sl = gh->getInventory().getEquipSlot(
            static_cast<game::EquipSlot>(slot - 1));
        if (!sl.empty()) return pushItemCooldown(L, gh, sl.item.itemId);
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}

// SetPortraitTexture(texture, unit) - put a unit's face in a texture.
//
// The player's and the target's, each rendered to an offscreen image of its
// own. Any other unit leaves the texture as it was and clears every claim on
// it, so a portrait frame reused for a party member does not keep showing
// whichever of the two it last held.
//
// The texture is remembered rather than filled here: the handle is rebuilt
// whenever the portrait's render target is, so it has to be assigned every
// frame, which the render loop does for everything on this list.
static int lua_SetPortraitTexture(lua_State* L) {
    auto* tree = getWidgetTree(L);
    if (!tree || !lua_istable(L, 1)) return 0;
    // The widget id the frame table carries. widgetIdOf lives in lua_engine.cpp
    // and is not declared anywhere this file can see it.
    lua_getfield(L, 1, "__wid");
    const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (id == 0) return 0;

    std::string unit(luaL_optstring(L, 2, ""));
    toLowerInPlace(unit);
    // Only the units this client can build a face for. Anything else releases
    // the texture rather than claiming it, so a portrait frame reused for a
    // unit with no picture does not keep the last one it had.
    const bool answerable =
        (unit == "player" || unit == "target" || unit == "pet" ||
         unit == "focus" || unit == "npc" || unit == "questnpc" ||
         (unit.rfind("party", 0) == 0 && unit.size() == 6 &&
          unit[5] >= '1' && unit[5] <= '4'));
    tree->setPortraitUnit(id, answerable ? unit : std::string());
    return 0;
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
static int lua_GetHairCustomization(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool tauren = gh &&
        static_cast<game::Race>(gh->getPlayerRace()) == game::Race::TAUREN;
    lua_pushstring(L, tauren ? "HORNS" : "NORMAL");
    return 1;
}

/// The facial category, which genuinely differs by race - a troll's is tusks
/// and a night elf's is markings, and calling either "hair" reads as a mistake
/// rather than as a shortcut. These are the game's own categories; anything
/// unlisted falls back to NORMAL, which is what a beard is.
static int lua_GetFacialHairCustomization(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* category = "NORMAL";
    if (gh) {
        switch (static_cast<game::Race>(gh->getPlayerRace())) {
            case game::Race::NIGHT_ELF: category = "MARKINGS"; break;
            case game::Race::UNDEAD:    category = "FEATURES"; break;
            case game::Race::TAUREN:    category = "HORNS";    break;
            case game::Race::TROLL:     category = "TUSKS";    break;
            case game::Race::BLOOD_ELF: category = "EARRINGS"; break;
            default: break;
        }
    }
    lua_pushstring(L, category);
    return 1;
}

/// CanAlterSkin() - whether the barber offers a fourth selector for skin.
///
/// False, and not merely for want of data: answering yes would put a selector
/// on screen that this client cannot fill, and the barber tests every selector
/// it has drawn before it will let the player buy anything.
static int lua_CanAlterSkin(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

// --- The barber's selectors, its price and its two exits ---
//
// The style lists and the originals used to be built inside this client's own
// barber window, so with the panel handed over nothing had built them. They
// come from WindowManager through LuaServices now, from a version that runs
// whether or not this client is drawing the chair.

/// GetBarberShopStyleInfo(selector) → name, category, cost, isCurrent
///
/// blizzard_barbershopui.lua reads the first and the fourth, and reads the
/// fourth through select(4, ...) - so all four have to be there for the index
/// to land on the right one.
static int lua_GetBarberShopStyleInfo(lua_State* L) {
    const int selector = static_cast<int>(luaL_optinteger(L, 1, 0));
    auto* svc = getLuaServices(L);
    std::string name;
    bool isCurrent = false;
    if (!svc || !svc->getBarberStyleInfo ||
        !svc->getBarberStyleInfo(selector, name, isCurrent)) {
        return 0;
    }
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, "");     // category, which this client does not name
    lua_pushinteger(L, 0);     // per-selector cost; the total is the one asked for
    lua_pushboolean(L, isCurrent ? 1 : 0);
    return 4;
}

/// SetNextBarberShopStyle(selector, direction) - the arrows beside a selector.
///
/// The forward arrow passes 1 and the back arrow passes nothing at all
/// (blizzard_barbershopui.xml:31 and :56), so an absent argument means back
/// rather than a missing parameter.
static int lua_SetNextBarberShopStyle(lua_State* L) {
    const int selector = static_cast<int>(luaL_optinteger(L, 1, 0));
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
    lua_pushnumber(L, 100);
    return 1;
}

/// GetRestState() → stateID, stateName, xpMultiplier.
///
/// One is rested and two is normal, which is the way round WoW numbers them -
/// this answered the opposite. MainMenuBar multiplies by the third return the
/// line after reading it, so returning only the id was arithmetic on nil every
/// time the cursor crossed the experience bar.
///
/// Rested means there is rested experience left to spend, not that the player
/// is standing in an inn. Those are different questions and this asked the
/// second: IsResting reads the rest-state byte out of PLAYER_BYTES_2, which the
/// server clears the moment you walk outside, while the pool you earned in
/// there is spent over the next several levels. So the bar went back to the
/// normal purple with the exhaustion tick still several bubbles ahead of the
/// fill - the two disagreeing on screen about the same thing.
///
/// IsResting keeps that byte, which is the right source for the "Resting"
/// indicator it feeds.
static int lua_GetRestState(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool rested = gh && gh->getPlayerRestedXp() > 0;
    lua_pushnumber(L, rested ? 1 : 2);
    lua_pushstring(L, rested ? "Rested" : "Normal");
    lua_pushnumber(L, rested ? 1.5 : 1.0);
    return 3;
}

// --- Quest Log API ---

static int lua_UnitAffectingCombat(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInCombat());
    } else {
        // Check UNIT_FLAG_IN_COMBAT (0x00080000) in UNIT_FIELD_FLAGS
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool inCombat = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t flags = entity->getField(
                    game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
                inCombat = (flags & 0x00080000) != 0; // UNIT_FLAG_IN_COMBAT
            }
        }
        lua_pushboolean(L, inCombat);
    }
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
    lua_pushboolean(L, 1);
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
static int lua_StartAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t target = gh->getTargetGuid();
    if (target != 0) gh->startAutoAttack(target);
    return 0;
}

static int lua_StopAttack(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->stopAutoAttack();
    return 0;
}

// GetReleaseTimeRemaining() → milliseconds before the corpse releases itself,
// or -1 when nothing will force it
//
// UIParent asks this the moment the player dies, as `> 0 or == -1`, so an
// absent answer is an error on every death rather than a popup that does not
// appear. Nothing here counts down to a forced release, and -1 is the game's
// own way of saying so.

/// GetNumRaidMembers() → everyone in the raid, this player included, or nought
/// when the group is not a raid.
///
/// The player is counted because the interface counts them: GetDisplayedAllyFrames
/// compares this against MEMBERS_PER_RAID_GROUP to decide between the party
/// frames and the raid frames, and a raid of five that answered four would be
/// drawn as a party for ever.
static int lua_GetNumRaidMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { return luaReturnZero(L); }
    const auto& pd = gh->getPartyData();
    if (!game::groupIsRaid(pd.groupType)) { return luaReturnZero(L); }
    lua_pushnumber(L, static_cast<int>(pd.members.size()) + 1);
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

static int lua_UnitInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnFalse(L); }
        const auto& pd = gh->getPartyData();
        bool found = false;
        for (const auto& m : pd.members) {
            if (m.guid == guid) { found = true; break; }
        }
        lua_pushboolean(L, found);
    }
    return 1;
}

static int lua_UnitInRaid(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const auto& pd = gh->getPartyData();
    if (!game::groupIsRaid(pd.groupType)) { return luaReturnFalse(L); }
    if (uidStr == "player") {
        lua_pushboolean(L, 1);
        return 1;
    }
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    bool found = false;
    for (const auto& m : pd.members) {
        if (m.guid == guid) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

// GetRaidRosterInfo(index) → name, rank, subgroup, level, class, fileName, zone, online, isDead, role, isML
static int lua_GetRaidRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { return luaReturnNil(L); }
    const auto& pd = gh->getPartyData();
    if (index > static_cast<int>(pd.members.size())) { return luaReturnNil(L); }
    const auto& m = pd.members[index - 1];
    lua_pushstring(L, m.name.c_str());       // name
    lua_pushnumber(L, m.guid == pd.leaderGuid ? 2 : (m.flags & 0x01 ? 1 : 0)); // rank (0=member, 1=assist, 2=leader)
    lua_pushnumber(L, m.subGroup + 1);       // subgroup (1-indexed)
    lua_pushnumber(L, m.level);              // level
    // Class: resolve from entity if available
    std::string className = "Unknown";
    auto entity = gh->getEntityManager().getEntity(m.guid);
    if (entity) {
        uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        uint8_t classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
        if (classId > 0 && classId < 12) className = kLuaClasses[classId];
    }
    lua_pushstring(L, className.c_str());    // class (localized)
    lua_pushstring(L, className.c_str());    // fileName
    lua_pushstring(L, "");                   // zone
    lua_pushboolean(L, m.isOnline);          // online
    lua_pushboolean(L, m.curHealth == 0);    // isDead
    lua_pushstring(L, "NONE");               // role
    lua_pushboolean(L, pd.looterGuid == m.guid ? 1 : 0); // isML
    return 11;
}

// GetThreatStatusColor(statusIndex) → r, g, b
static int lua_GetThreatStatusColor(lua_State* L) {
    int status = static_cast<int>(luaL_optnumber(L, 1, 0));
    switch (status) {
        case 0: lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); break; // gray (no threat)
        case 1: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.47f); break; // yellow (threat)
        case 2: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.6f);  lua_pushnumber(L, 0.0f);  break; // orange (high threat)
        case 3: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.0f);  lua_pushnumber(L, 0.0f);  break; // red (tanking)
        default: lua_pushnumber(L, 1.0f); lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  break;
    }
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
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    toLowerInPlace(u1);
    toLowerInPlace(u2);
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    lua_pushboolean(L, g1 != 0 && g1 == g2);
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
    lua_pushboolean(L, reactionBetween(L, a, b) >= 5);
    return 1;
}

/// UnitIsEnemy(unit1, unit2) - is unit2 hostile to unit1. The same shape as
/// UnitIsFriend above, and it was wrong the same way: asked about the player it
/// answered false for every enemy.
static int lua_UnitIsEnemy(lua_State* L) {
    // 0x0060d330: unit1's reaction to unit2 below 2, which is hostile or worse.
    const char* a = luaL_optstring(L, 1, "player");
    const char* b = luaL_optstring(L, 2, nullptr);
    if (!b) { b = a; a = "player"; }
    const int reaction = reactionBetween(L, a, b);
    lua_pushboolean(L, reaction != 0 && reaction <= 2);
    return 1;
}

static int lua_UnitCreatureType(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "Unknown"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { lua_pushstring(L, "Unknown"); return 1; }
    // Player units are always "Humanoid"
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "Humanoid");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "Unknown"); return 1; }
    uint32_t ct = gh->getCreatureType(unit->getEntry());
    static constexpr const char* kTypes[] = {
        "Unknown", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical",
        "Not specified", "Totem", "Non-combat Pet", "Gas Cloud"
    };
    lua_pushstring(L, (ct < 14) ? kTypes[ct] : "Unknown");
    return 1;
}

// GetPlayerInfoByGUID(guid) → localizedClass, englishClass, localizedRace, englishRace, sex, name, realm
static int lua_GetPlayerInfoByGUID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* guidStr = luaL_checkstring(L, 1);
    if (!gh || !guidStr) {
        for (int i = 0; i < 7; i++) lua_pushnil(L);
        return 7;
    }
    // Parse hex GUID string "0x0000000000000001"
    uint64_t guid = 0;
    if (guidStr[0] == '0' && (guidStr[1] == 'x' || guidStr[1] == 'X'))
        guid = strtoull(guidStr + 2, nullptr, 16);
    else
        guid = strtoull(guidStr, nullptr, 16);

    if (guid == 0) { for (int i = 0; i < 7; i++) lua_pushnil(L); return 7; }

    // Look up entity name
    std::string name = gh->lookupName(guid);
    if (name.empty() && guid == gh->getPlayerGuid()) {
        const auto& chars = gh->getCharacters();
        for (const auto& c : chars)
            if (c.guid == guid) { name = c.name; break; }
    }

    // For player GUID, return class/race if it's the local player
    const char* className = "Unknown";
    const char* raceName = "Unknown";
    if (guid == gh->getPlayerGuid()) {
        uint8_t cid = gh->getPlayerClass();
        uint8_t rid = gh->getPlayerRace();
        if (cid < 12) className = kLuaClasses[cid];
        if (rid < 12) raceName = kLuaRaces[rid];
    }

    lua_pushstring(L, className);  // 1: localizedClass
    lua_pushstring(L, className);  // 2: englishClass
    lua_pushstring(L, raceName);   // 3: localizedRace
    lua_pushstring(L, raceName);   // 4: englishRace
    lua_pushnumber(L, 0);          // 5: sex (0=unknown)
    lua_pushstring(L, name.c_str()); // 6: name
    lua_pushstring(L, "");         // 7: realm
    return 7;
}


static int lua_UnitClassification(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "normal"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
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
    int rank = gh->getCreatureRank(unit->getEntry());
    switch (rank) {
        case 1:  lua_pushstring(L, "elite"); break;
        case 2:  lua_pushstring(L, "rareelite"); break;
        case 3:  lua_pushstring(L, "worldboss"); break;
        case 4:  lua_pushstring(L, "rare"); break;
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
static int& battlefieldScoreFaction() {
    static int faction = -1;
    return faction;
}

/// Which column the scoreboard is sorted by, and which way.
///
/// The direction lives here because the interface does not keep it: clicking a
/// column hands the same name over again and expects the order to reverse,
/// which only works if something remembers what it was.
static std::string& battlefieldSortColumn() {
    static std::string column;
    return column;
}
static bool& battlefieldSortDescending() {
    static bool descending = true;
    return descending;
}

/// The scoreboard rows that pass the current filter, in the current order.
///
/// Pointers into the scoreboard rather than copies: this is rebuilt on every
/// GetBattlefieldScore call, once per row per redraw, and a battleground holds
/// forty players.
static std::vector<const game::BgPlayerScore*>
filteredBgScores(const game::BgScoreboardData& sb) {
    std::vector<const game::BgPlayerScore*> rows;
    rows.reserve(sb.players.size());
    const int want = battlefieldScoreFaction();
    for (const auto& p : sb.players) {
        // A row with no team on the wire cannot be filtered out of a faction
        // tab - a battleground sends none, so every row shows on both tabs
        // rather than all of them landing on Horde because zero is Horde.
        if (want < 0 || !p.hasTeam || static_cast<int>(p.team) == want)
            rows.push_back(&p);
    }

    const std::string& column = battlefieldSortColumn();
    if (column.empty()) return rows;

    // Only the columns this client has the numbers for. "class" is not one of
    // them - the scoreboard packet carries no class.
    //
    // Damage and healing were excluded here for the same reason, back when the
    // parser skipped their eight bytes and every row read zero; sorting by
    // them would have rearranged nothing while looking like it worked. The
    // parser reads them now and GetBattlefieldScore hands them to the frame,
    // so the two columns show real numbers - and were the only two whose
    // header did nothing when clicked.
    auto key = [&column](const game::BgPlayerScore* p) -> double {
        if (column == "kills")   return p->killingBlows;
        if (column == "deaths")  return p->deaths;
        if (column == "hk")      return p->honorableKills;
        if (column == "cp")      return p->bonusHonor;
        if (column == "team")    return p->team;
        if (column == "damage")  return p->damageDone;
        if (column == "healing") return p->healingDone;
        return 0.0;
    };
    const bool byName = (column == "name");
    const bool descending = battlefieldSortDescending();
    std::stable_sort(rows.begin(), rows.end(),
                     [&](const game::BgPlayerScore* a, const game::BgPlayerScore* b) {
                         if (byName) {
                             return descending ? a->name > b->name : a->name < b->name;
                         }
                         return descending ? key(a) > key(b) : key(a) < key(b);
                     });
    return rows;
}

// UnitIsConnected(unit) → boolean
static int lua_UnitIsConnected(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnFalse(L); }
    // Player is always connected
    if (guid == gh->getPlayerGuid()) { lua_pushboolean(L, 1); return 1; }
    // Check party/raid member online status
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            lua_pushboolean(L, m.isOnline ? 1 : 0);
            return 1;
        }
    }
    // Non-party entities that exist are considered connected
    auto entity = gh->getEntityManager().getEntity(guid);
    lua_pushboolean(L, entity ? 1 : 0);
    return 1;
}

// HasAction(slot) → boolean (1-indexed slot)

/// 0x0052d310 / 0x00573200: the player, a party or raid member, or the pet
/// or charm (UNIT_FIELD_CHARM, else UNIT_FIELD_SUMMON) of one of them. For a
/// member out of view the client asks the pet guid its party stats carried;
/// this client keeps none, so such a pet is not counted.
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
        if (gh->getEntityManager().getEntity(m.guid) && petOf(m.guid) == guid) return true;
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
    static constexpr int kGreyGap[20] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, lua_tostring(L, 1));
    bool trivial = false;
    if (gh && unit && unit->getType() != game::ObjectType::PLAYER) {
        auto me = std::dynamic_pointer_cast<game::Unit>(gh->getEntityManager().getEntity(gh->getPlayerGuid()));
        if (me) {
            const int playerLevel = static_cast<int>(me->getLevel());
            const int gap = playerLevel - static_cast<int>(unit->getLevel());
            if (gap > 0) {
                const unsigned band = static_cast<unsigned>(playerLevel / 5);
                trivial = band < 20 ? gap > kGreyGap[band] : gap > 8;
            }
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
            if (gh && guid && gh->vehicleRoster().seatState(guid) != 0) lua_pushnumber(L, 1.0);
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
                {"GetAttackPowerForStat", lua_GetAttackPower},
                {"GetShapeshiftFormInfo", [](lua_State* L) -> int {
            // GetShapeshiftFormInfo(index) → icon, name, isActive, isCastable
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            uint8_t classId = gh->getPlayerClass();
            uint8_t currentForm = gh->getShapeshiftFormId();

            // The one table CastShapeshiftForm and GetNumShapeshiftForms
            // also read, filtered the same way. The bar walks
            // 1..GetNumShapeshiftForms() and asks about each index, so all
            // three have to be looking at the same list in the same order.
            const auto forms = game::knownShapeshiftForms(classId, gh->getKnownSpells());
            if (static_cast<size_t>(index) > forms.size()) { return luaReturnNil(L); }
            const auto& fi = forms[static_cast<size_t>(index) - 1];
            // 0x0053de90: active is 0x00802cb0's answer, the form's spell's
            // aura on the player - not a form id, which a stance or presence
            // may not even set.
            const bool active = game::isPreWotlk() ? currentForm == fi.formId
                                                   : gh->formSpellInEffect(fi.spellId);
            lua_pushstring(L, fi.icon);                          // icon
            lua_pushstring(L, fi.name);                          // name
            lua_pushboolean(L, active ? 1 : 0);                  // isActive
            lua_pushboolean(L, 1);                               // isCastable
            return 4;
        }},
                {"UnitIsPVP", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            if (!gh) { return luaReturnFalse(L); }
            uint64_t guid = resolveUnitGuid(gh, std::string(uid));
            if (guid == 0) { return luaReturnFalse(L); }
            auto entity = gh->getEntityManager().getEntity(guid);
            if (!entity) { return luaReturnFalse(L); }
            // UNIT_FLAG_PVP = 0x00001000
            uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
            lua_pushboolean(L, (flags & 0x00001000) ? 1 : 0);
            return 1;
        }},
                {"UnitIsPVPFreeForAll", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            if (!gh) { return luaReturnFalse(L); }
            uint64_t guid = resolveUnitGuid(gh, std::string(uid));
            if (guid == 0) { return luaReturnFalse(L); }
            auto entity = gh->getEntityManager().getEntity(guid);
            if (!entity) { return luaReturnFalse(L); }
            // FFA PvP is PLAYER_FLAGS bit 0x80, NOT UNIT_FIELD_FLAGS bit 0x00080000
            // (which is UNIT_FLAG_PACIFIED - would flag pacified mobs as FFA-PVP).
            uint32_t pf = entity->getField(game::fieldIndex(game::UF::PLAYER_FLAGS));
            lua_pushboolean(L, (pf & 0x00000080) ? 1 : 0);
            return 1;
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
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& slot = gh->getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (slot.empty()) { lua_pushboolean(L, 0); return 1; }
            const auto* info = gh->getItemInfo(slot.item.itemId);
            const uint8_t t = info ? info->inventoryType : 0;
            // A weapon, not a shield or a held book: OFF_HAND in this table
            // is the held-in-off-hand class, which is not one.
            lua_pushboolean(L, (t == game::InvType::ONE_HAND ||
                                t == game::InvType::TWO_HAND) ? 1 : 0);
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
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            std::string uidStr(uid);
            toLowerInPlace(uidStr);
            const uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
            lua_pushboolean(L, (gh && guid != 0 &&
                                gh->getPartyData().leaderGuid == guid) ? 1 : 0);
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
            lua_pushboolean(L, game::isCorpseObject(entity.get()) ? 1 : 0);
            return 1;
        }},
                // Whether the first unit may help the second: 0x0060d690
                // asks 0x007293d0 of unit1, its last argument 0.
                {"UnitCanAssist", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            std::string u1(luaL_optstring(L, 1, "player")), u2(luaL_optstring(L, 2, "target"));
            toLowerInPlace(u1);
            toLowerInPlace(u2);
            const uint64_t g1 = gh ? resolveUnitGuid(gh, u1) : 0;
            const uint64_t g2 = gh ? resolveUnitGuid(gh, u2) : 0;
            lua_pushboolean(L, (g1 != 0 && g2 != 0 && gh->unitMayAssist(g1, g2)) ? 1 : 0);
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
                lua_pushboolean(L, 0);
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
            // A boolean, not a number. The real binding answers true or false
            // and the interface asks `if ( registeredMatch )` - and zero is
            // true in Lua, so answering 0 for "not rated" labelled every arena
            // queue a rated match.
            lua_pushboolean(L, isRated ? 1 : 0);
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
                {"SetBattlefieldScoreFaction", [](lua_State* L) -> int {
            if (lua_isnoneornil(L, 1)) battlefieldScoreFaction() = -1;
            else battlefieldScoreFaction() = static_cast<int>(lua_tonumber(L, 1));
            return 0;
        }},
                // SortBattlefieldScoreData(column) - order the rows.
                //
                // The same column twice reverses, which is the real client's
                // behaviour and has to be remembered here: the interface hands
                // over only the column name and keeps no direction of its own.
                //
                // A name is sensible ascending and a score is not, so a fresh
                // column starts descending for everything but the name.
                {"SortBattlefieldScoreData", [](lua_State* L) -> int {
            std::string column(luaL_optstring(L, 1, ""));
            if (column.empty()) return 0;
            if (column == battlefieldSortColumn()) {
                battlefieldSortDescending() = !battlefieldSortDescending();
            } else {
                battlefieldSortColumn() = column;
                battlefieldSortDescending() = (column != "name");
            }
            return 0;
        }},
                {"GetNumBattlefieldScores", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            lua_pushnumber(L, sb ? static_cast<double>(filteredBgScores(*sb).size()) : 0.0);
            return 1;
        }},
                {"GetBattlefieldScore", [](lua_State* L) -> int {
            // GetBattlefieldScore(index) → name, killingBlows, honorableKills, deaths, honorGained, faction, rank, race, class, classToken, damageDone, healingDone
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (!sb) return luaReturnNil(L);
            const auto rows = filteredBgScores(*sb);
            if (index < 1 || index > static_cast<int>(rows.size())) {
                return luaReturnNil(L);
            }
            const auto& p = *rows[static_cast<size_t>(index - 1)];
            lua_pushstring(L, p.name.c_str());     // name
            lua_pushnumber(L, p.killingBlows);      // killingBlows
            lua_pushnumber(L, p.honorableKills);    // honorableKills
            lua_pushnumber(L, p.deaths);            // deaths
            lua_pushnumber(L, p.bonusHonor);        // honorGained
            lua_pushnumber(L, p.team);              // faction (0=Horde,1=Alliance)
            lua_pushnumber(L, 0);                   // rank
            lua_pushstring(L, "");                  // race
            lua_pushstring(L, "");                  // class
            // Nil, not a class. The packet carries none - AppendToPacket sends
            // a guid and six numbers - and the scoreboard asks for exactly
            // this case: `if (classToken) then ... else buttonClass:Hide()`.
            // So absence is an answer it handles, and "WARRIOR" was not a
            // safer default than nil but a wrong one that looks like data: it
            // drew a warrior's icon beside all forty players in the
            // battleground, every one of them.
            lua_pushnil(L);                         // classToken
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
                {"GetNumBattlefieldStats", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            size_t columns = 0;
            // The widest row rather than the first: a row that arrived short
            // would otherwise decide the column count for the whole table.
            if (sb) for (const auto& p : sb->players) columns = std::max(columns, p.bgStats.size());
            lua_pushnumber(L, static_cast<double>(columns));
            return 1;
        }},
                // GetBattlefieldStatInfo(index) → text, icon, tooltip
                //
                // The icon is "" rather than nil on purpose: the frame tests it
                // with ~= "" and then concatenates the faction onto it, so nil
                // passes the test and dies on the concatenation. This client
                // has no artwork per column, and "" is how the frame is told so.
                {"GetBattlefieldStatInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_checknumber(L, 1));
            const auto* sb = gh ? gh->getBgScoreboard() : nullptr;
            if (!sb || index < 1) return luaReturnNil(L);
            // The label is this client's to supply. BuildObjectivesBlock
            // sends a count and that many bare numbers, so the name this used
            // to read out of the row was never on the wire - it was bytes of
            // the value before it, read as a string.
            //
            // Which column is which follows from the map: Warsong sends flags
            // captured and returned, Alterac Valley five graveyard and tower
            // counts, Eye of the Storm one. The table is beside the world-state
            // one it belongs with.
            const char* label = game::bgObjectiveLabel(
                gh->getCurrentMapId(), static_cast<size_t>(index - 1));
            if (!label) return luaReturnNil(L);
            lua_pushstring(L, label);
            lua_pushstring(L, "");
            lua_pushstring(L, label);
            return 3;
        }},
                // GetBattlefieldStatData(playerIndex, statIndex) → the value.
                //
                // Always a number. The frame compares it against zero the line
                // after it asks, so nil is not "no value" there, it is an error.
                {"GetBattlefieldStatData", [](lua_State* L) -> int {
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
            const uint32_t slot = (idx >= 1 && idx <= 3) ? static_cast<uint32_t>(idx - 1)
                                                         : 0xFFFFFFFFu;
            int accept = lua_toboolean(L, 2);
            if (gh) {
                if (accept) gh->acceptBattlefield(slot);
                else gh->declineBattlefield(slot);
            }
            return 0;
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
                {"UnitClassBase",         [](lua_State* L) -> int {
            lua_getglobal(L, "UnitClass");
            lua_pushvalue(L, 1);
            if (lua_pcall(L, 1, 3, 0) != 0) { lua_pop(L, 1); lua_pushnil(L); return 1; }
            // UnitClass answers name, token, id; the token is what is wanted.
            lua_remove(L, -3);
            lua_pop(L, 1);
            return 1;
        }},
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
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(game::EquipSlot::RANGED);
            bool wand = false;
            if (!sl.empty()) {
                if (const auto* info = gh->getItemInfo(sl.item.itemId); info && info->valid) {
                    wand = (info->itemClass == 2 && info->subClass == 19);
                }
            }
            lua_pushboolean(L, wand);
            return 1;
        }},
                // UnitHasRelicSlot(unit) - the four classes whose ranged slot
                // holds a relic instead of a weapon: paladin librams, death
                // knight sigils, shaman totems and druid idols. Answering no
                // for all of them made the paperdoll label that slot "Ranged"
                // and read its stats as a ranged weapon's.
                {"UnitHasRelicSlot",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "player");
            // Only the player's class is known well enough to answer this.
            if (!gh || std::string(uid) != "player") { lua_pushboolean(L, 0); return 1; }
            const uint8_t c = gh->getPlayerClass();
            lua_pushboolean(L, c == 2 || c == 6 || c == 7 || c == 11);
            return 1;
        }},
                // Whether the repair cursor is up. A flat false meant the
                // merchant's repair button re-armed on every click instead of
                // toggling, and neither the bag nor the paperdoll tooltip ever
                // showed an item's repair cost.
                {"InRepairMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool active = repairCursorUp() && gh && gh->isVendorWindowOpen();
            lua_pushboolean(L, active ? 1 : 0);
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
            lua_pushboolean(L, slot > 0 && cursorEquipSlot() == slot);
            return 1;
        }},
                // GetInventoryItemBroken(unit, slot) - worn out, so the
                // paperdoll draws that slot's icon red. The durability is
                // already tracked and already answered by
                // GetInventoryItemDurability; only this was left saying no.
                {"GetInventoryItemBroken",  [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slotId = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || slotId < 1 || slotId > 19) { lua_pushboolean(L, 0); return 1; }
            const auto& sl = gh->getInventory().getEquipSlot(
                static_cast<game::EquipSlot>(slotId - 1));
            lua_pushboolean(L, !sl.empty() && sl.item.maxDurability > 0 &&
                               sl.item.curDurability == 0);
            return 1;
        }},
                // CursorCanGoInSlot(slot) - whether what the cursor is holding
                // could be worn in that paperdoll slot, which is what makes the
                // slot light up while an item is being dragged. Answering no
                // for everything meant nothing ever lit up.
                //
                // Which slot of a pair, this does not decide: both rings and
                // both trinkets light up, as they do in WoW. That is a
                // different question from the one InventoryScreen answers when
                // it picks a slot to equip into, so this does not go looking
                // for that logic.
                {"CursorCanGoInSlot",       [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            const uint32_t held = cursorItemId();
            if (!gh || slot < 1 || held == 0) { lua_pushboolean(L, 0); return 1; }
            const auto* info = gh->getItemInfo(held);
            if (!info || !info->valid) { lua_pushboolean(L, 0); return 1; }
            // Paperdoll slot ids are the EquipSlot enum plus one.
            using namespace game::InvType;
            bool fits = false;
            switch (info->inventoryType) {
                case HEAD:       fits = (slot == 1);  break;
                case NECK:       fits = (slot == 2);  break;
                case SHOULDERS:  fits = (slot == 3);  break;
                case SHIRT:      fits = (slot == 4);  break;
                case CHEST: case ROBE: fits = (slot == 5); break;
                case WAIST:      fits = (slot == 6);  break;
                case LEGS:       fits = (slot == 7);  break;
                case FEET:       fits = (slot == 8);  break;
                case WRISTS:     fits = (slot == 9);  break;
                case HANDS:      fits = (slot == 10); break;
                case FINGER:     fits = (slot == 11 || slot == 12); break;
                case TRINKET:    fits = (slot == 13 || slot == 14); break;
                case BACK:       fits = (slot == 15); break;
                // A one-hander goes in either hand; a main-hand-only weapon and
                // a two-hander only in the first.
                case ONE_HAND:   fits = (slot == 16 || slot == 17); break;
                case MAIN_HAND: case TWO_HAND: fits = (slot == 16); break;
                case SHIELD: case OFF_HAND: case HOLDABLE: fits = (slot == 17); break;
                case RANGED_BOW: case RANGED_GUN: case THROWN: fits = (slot == 18); break;
                case TABARD:     fits = (slot == 19); break;
                default:         fits = false; break;
            }
            lua_pushboolean(L, fits);
            return 1;
        }},
                // The pet tab of the character sheet, which is handed over -
                // PetPaperDollFrame_SetStats and PetExpBar_Update run whenever
                // it is shown, so with a pet out these raised on opening it.
                //
                // The two modifiers are multipliers, so one is the neutral
                // answer; zero would have read as a pet with no health at all.
                {"GetUnitHealthModifier",   [](lua_State* L) -> int {
            lua_pushnumber(L, 1.0); return 1; }},
                {"GetUnitPowerModifier",    [](lua_State* L) -> int {
            lua_pushnumber(L, 1.0); return 1; }},
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
                {"CallCompanion", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1) return 0;
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
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
                // Putting one away is cancelling its aura, which is the same
                // thing right-clicking the buff does.
                {"DismissCompanion", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            if (!gh) return 0;
            const auto& list = gh->getCompanions(std::string(kind) == "MOUNT");
            for (const auto& c : list) {
                for (const auto& a : gh->getPlayerAuras()) {
                    if (a.spellId != c.spellId) continue;
                    gh->cancelAura(c.spellId);
                    return 0;
                }
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
                // 1..GetNumTitles, and GetNumTitles answers 192, which is the
                // whole space the six dwords cover (kKnownTitleCount), so every
                // title id the mask can name comes through here.
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

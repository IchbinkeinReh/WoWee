// lua_spell_api.cpp - Spell info, casting, auras, and targeting Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/shapeshift_forms.hpp"
#include "game/game_utils.hpp"
#include "game/target_direction.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_taint.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/item_text.hpp"
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include <set>
#include <unordered_map>

namespace wowee::addons {


/// The highest-rank known spell with this name, or 0 when the player knows
/// none by that name.
///
/// This is what `/cast Frostbolt` means: the name alone picks the best rank the
/// player has. Two bindings resolved it with their own copy of the walk, and
/// picking the wrong one is silent - the spell goes off, it is simply the
/// rank-one version.
///
/// Rank arrives from the DBC as text, "Rank 4", so the number is parsed out of
/// it. A spell with no rank string, which is most of them, sorts as rank zero:
/// right, because it is the only one of its name.
uint32_t highestKnownRankByName(game::GameHandler* gh, const std::string& name) {
    std::string wanted(name);
    toLowerInPlace(wanted);

    uint32_t bestId = 0;
    int bestRank = -1;
    for (uint32_t sid : gh->getKnownSpells()) {
        std::string known = gh->getSpellName(sid);
        toLowerInPlace(known);
        if (known != wanted) continue;

        int rank = 0;
        const std::string& rankText = gh->getSpellRank(sid);
        if (!rankText.empty()) {
            std::string lowered = rankText;
            toLowerInPlace(lowered);
            if (lowered.rfind("rank ", 0) == 0) {
                try { rank = std::stoi(lowered.substr(5)); } catch (...) {}
            }
        }
        if (rank > bestRank) { bestRank = rank; bestId = sid; }
    }
    return bestId;
}


// ---- Finishing a spell that is waiting for a target ----
//
// A spell cast with no target leaves the cursor holding it until something is
// clicked; these are what the unit and party frames call when that click lands
// on them.
//
// SpellTargetUnit("unit") (0x0080dc00): the unit, while a spell waits for one,
// offered to it (0x0080bc80); no unit by that name is
// SPELL_FAILED_OUT_OF_RANGE. Nothing is returned.
static int lua_SpellTargetUnit(lua_State* L) {
    std::string token = luaL_checkstring(L, 1);
    if (!taint::allowed(L, 2)) return 0;   // 0x0080dc33
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isUnitTargeting()) return 0;
    toLowerInPlace(token);
    gh->spellTargetUnit(resolveUnitGuid(gh, token));
    return 0;
}
// SpellCanTargetUnit("unit") (0x00804190): 1 where the spell waiting for a
// unit would take it, within its reach (0x00803a50), else nil.
static int lua_SpellCanTargetUnit(lua_State* L) {
    std::string token = luaL_checkstring(L, 1);
    auto* gh = getGameHandler(L);
    toLowerInPlace(token);
    const uint64_t guid = gh ? resolveUnitGuid(gh, token) : 0;
    if (guid != 0 && gh->spellCanTargetUnit(guid)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
// Only spells waiting for a unit are held for a click; an item cursor is
// finished by the bag click instead, so this answers false.
// Asked of the taint check first (0x008007e0, kind 2) like the rest of the
// targeting family.
static int lua_SpellTargetItem(lua_State* L) {
    if (!taint::allowed(L, 2)) return 0;
    lua_pushboolean(L, 0);
    return 1;
}

// Dropping what the cursor holds onto a unit, which opens a trade. The cursor
// carries no item here, so there is nothing to drop.
static int lua_DropItemOnUnit(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// The totem bar's per-slot spell choice, which needs the multi-cast action bar
// this client does not model.
// It asks the taint check first all the same (0x005ab8a0, kind 0xe).
static int lua_SetMultiCastSpell(lua_State* L) {
    taint::allowed(L, 0xe);
    return 0;
}


// --- Totems ---
//
// Four slots, each holding a spell and how long it has left. The interface
// wants the moment one was placed rather than its age, on the same clock
// GetTime() reports - a bar that is drawn from start and duration cannot use a
// figure measured from a different zero.

// GetTotemInfo(slot) → haveTotem, name, startTime, duration, icon
static int lua_GetTotemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "");
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 5;
    }
    const auto& totem = gh->getTotemSlot(slot - 1);
    const bool active = totem.active();
    const double durationSec = totem.durationMs / 1000.0;
    const double remainingSec = totem.remainingMs() / 1000.0;

    lua_pushboolean(L, active ? 1 : 0);
    lua_pushstring(L, active ? gh->getSpellName(totem.spellId).c_str() : "");
    // Placed at however long ago it has already run for.
    lua_pushnumber(L, active ? luaGetTimeNow() - (durationSec - remainingSec) : 0.0);
    lua_pushnumber(L, active ? durationSec : 0.0);
    if (active) {
        const std::string icon = gh->getSpellIconPath(totem.spellId);
        if (!icon.empty()) lua_pushstring(L, icon.c_str()); else lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    return 5;
}

// GetTotemTimeLeft(slot) → seconds
static int lua_GetTotemTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getTotemSlot(slot - 1).remainingMs() / 1000.0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)

/// Whether the player's class can remove a debuff of this dispel type.
///
/// The "RAID" filter FrameXML passes on party frames means "only what I can do
/// something about", and it is what showDispelDebuffs turns on. Without it the
/// filter string was accepted and ignored, so the frames showed every debuff
/// however little the healer could do - and answering the CVar without this
/// would have claimed a filter that does not filter.
///
/// Dispel types are Spell.dbc's: 1 Magic, 2 Curse, 3 Disease, 4 Poison. The
/// class table is 3.3.5's, and it is about what the class can remove from a
/// *friendly* target rather than what it can purge from an enemy.
static bool classCanDispel(uint8_t classId, uint8_t dispelType) {
    if (dispelType == 0) return false;
    constexpr uint8_t kMagic = 1, kCurse = 2, kDisease = 3, kPoison = 4;
    switch (classId) {
        case 2:  // Paladin - Cleanse
            return dispelType == kMagic || dispelType == kPoison || dispelType == kDisease;
        case 5:  // Priest - Dispel Magic, Abolish Disease
            return dispelType == kMagic || dispelType == kDisease;
        case 7:  // Shaman - Cure Toxins, Cleanse Spirit at 51
            return dispelType == kCurse || dispelType == kPoison || dispelType == kDisease;
        case 8:  // Mage - Remove Curse
            return dispelType == kCurse;
        case 11: // Druid - Remove Curse, Abolish Poison
            return dispelType == kCurse || dispelType == kPoison;
        case 6:  // Death Knight - the pet's Leech, and unholy's disease removal
            return dispelType == kDisease;
        case 9:  // Warlock - the Felhunter's Devour Magic
            return dispelType == kMagic;
        default: // Warrior, Hunter, Rogue: nothing
            return false;
    }
}

static int lua_UnitAura(lua_State* L, bool wantBuff) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "player");
    int index = static_cast<int>(luaL_optnumber(L, 2, 1));
    if (index < 1) { return luaReturnNil(L); }

    std::string uidStr(uid);
    toLowerInPlace(uidStr);

    const std::vector<game::AuraSlot>* auras = nullptr;
    if (uidStr == "player")      auras = &gh->getPlayerAuras();
    else if (uidStr == "target") auras = &gh->getTargetAuras();
    else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid != 0) auras = gh->getUnitAuras(guid);
    }
    if (!auras) { return luaReturnNil(L); }

    // "RAID" on a debuff means only what this character can remove. The index
    // counts into the filtered list, so the test has to come before the count
    // - filtering after it would answer the wrong aura for every index past
    // the first one skipped.
    const char* filterArg = luaL_optstring(L, 3, "");
    bool dispellableOnly = false;
    if (!wantBuff && filterArg) {
        std::string f(filterArg);
        for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        dispellableOnly = f.find("RAID") != std::string::npos;
    }
    const uint8_t playerClass = gh->getPlayerClass();

    // Filter to buffs or debuffs and find the Nth one
    int found = 0;
    for (const auto& aura : *auras) {
        if (aura.isEmpty() || aura.spellId == 0) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        if (dispellableOnly &&
            !classCanDispel(playerClass, gh->getSpellDispelType(aura.spellId))) {
            continue;
        }
        found++;
        if (found == index) {
            // Return: name, rank, icon, count, debuffType, duration, expirationTime, ...spellId
            std::string name = gh->getSpellName(aura.spellId);
            if (name.empty()) {
                // An aura this client's Spell.dbc has no row for draws as
                // "Unknown" with no icon, and nothing said which one it was.
                // A realm's own spells are the usual cause - a server can put
                // any id on a unit, and only a patched client knows its name.
                static std::set<uint32_t> saidNoName;
                if (saidNoName.insert(aura.spellId).second) {
                    LOG_WARNING("Aura spell ", aura.spellId, " on ", uidStr,
                                " has no Spell.dbc row, so it shows as 'Unknown' with no "
                                "icon (", (aura.flags & 0x80) ? "debuff" : "buff",
                                ", flags 0x", std::hex, static_cast<int>(aura.flags), std::dec,
                                ", caster 0x", std::hex, aura.casterGuid, std::dec, ")");
                }
            }
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
            lua_pushstring(L, "");           // rank
            std::string iconPath = gh->getSpellIconPath(aura.spellId);
            if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
            else lua_pushnil(L);             // icon texture path
            lua_pushnumber(L, aura.charges); // count
            // debuffType: resolve from Spell.dbc dispel type
            {
                uint8_t dt = gh->getSpellDispelType(aura.spellId);
                switch (dt) {
                    case 1:  lua_pushstring(L, "Magic");  break;
                    case 2:  lua_pushstring(L, "Curse");  break;
                    case 3:  lua_pushstring(L, "Disease"); break;
                    case 4:  lua_pushstring(L, "Poison"); break;
                    default: lua_pushnil(L);              break;
                }
            }
            lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
            // expirationTime: GetTime() + remaining seconds (so addons can compute countdown)
            if (aura.durationMs > 0) {
                uint64_t auraNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remMs = aura.getRemainingMs(auraNowMs);
                lua_pushnumber(L, luaGetTimeNow() + remMs / 1000.0);
            } else {
                lua_pushnumber(L, 0);  // permanent aura
            }
            // caster: return unit ID string if caster is known
            if (aura.casterGuid != 0) {
                if (aura.casterGuid == gh->getPlayerGuid())
                    lua_pushstring(L, "player");
                else if (aura.casterGuid == gh->getTargetGuid())
                    lua_pushstring(L, "target");
                else if (aura.casterGuid == gh->getFocusGuid())
                    lua_pushstring(L, "focus");
                else if (aura.casterGuid == gh->getPetGuid())
                    lua_pushstring(L, "pet");
                else {
                    char cBuf[32];
                    snprintf(cBuf, sizeof(cBuf), "0x%016llX", (unsigned long long)aura.casterGuid);
                    lua_pushstring(L, cBuf);
                }
            } else {
                lua_pushnil(L);
            }
            lua_pushboolean(L, 0);           // isStealable
            lua_pushboolean(L, 0);           // shouldConsolidate
            lua_pushnumber(L, aura.spellId); // spellId
            return 11;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ── The 1.12 buff API ───────────────────────────────────────────────────────
//
// A vanilla interface does not call UnitBuff. It asks for a *slot* and then
// asks that slot four more questions, which is a different shape: UnitBuff
// answers a name and counts into a filtered list, and these answer an opaque
// number the caller hands straight back to the rest of them.
//
// buffframe.lua walks slots upward until it is told there is nothing there,
// and the terminator is -1 rather than nil. Unbound, GetPlayerBuff answered
// nil through the missing-API fallback, so `if buffIndex < 0` compared nil
// with a number on the first line of BuffButton_Update. That raised while the
// file was being read, which loses the file whole: no buff has ever appeared
// on a 1.12 interface, and BuffFrame.xml never built.

/// The player's aura in a slot, or nullptr for an empty or out-of-range one.
///
/// The slot is the index into the player's own aura array rather than a
/// position in a filtered list, so it stays meaningful after the caller has
/// stopped filtering - which is the whole point of handing it back.
static const game::AuraSlot* playerAuraAt(game::GameHandler* gh, int slot) {
    if (!gh || slot < 0) return nullptr;
    const auto& auras = gh->getPlayerAuras();
    if (static_cast<size_t>(slot) >= auras.size()) return nullptr;
    const game::AuraSlot& aura = auras[static_cast<size_t>(slot)];
    return aura.isEmpty() ? nullptr : &aura;
}

/// Whether a filter asks for debuffs. 1.12 spells that "HARMFUL"; the other
/// words it can carry - PASSIVE, CANCELABLE - do not change which half is
/// wanted, so they are not read.
static bool filterWantsHarmful(const char* filter) {
    if (!filter) return false;
    std::string upper(filter);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper.find("HARMFUL") != std::string::npos;
}

/// Milliseconds on the same clock AuraSlot::receivedAtMs was stamped from.
static uint64_t auraNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// GetPlayerBuff(index, filter) -> slot, untilCancelled
//
// index counts from zero, which is how buffframe.lua asks.
static int lua_GetPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int wanted = static_cast<int>(luaL_optnumber(L, 1, 0));
    const bool harmful = filterWantsHarmful(luaL_optstring(L, 2, "HELPFUL"));
    if (gh && wanted >= 0) {
        const auto& auras = gh->getPlayerAuras();
        int seen = 0;
        for (size_t i = 0; i < auras.size(); ++i) {
            const game::AuraSlot& aura = auras[i];
            if (aura.isEmpty()) continue;
            const bool isDebuff = (aura.flags & 0x80) != 0;
            if (isDebuff != harmful) continue;
            if (seen++ != wanted) continue;
            lua_pushnumber(L, static_cast<double>(i));
            // A buff with no duration is one the player dismisses rather than
            // waits out, and the interface draws it without a timer.
            lua_pushnumber(L, aura.durationMs > 0 ? 0 : 1);
            return 2;
        }
    }
    // -1, never nil. The caller compares this with a number before it does
    // anything else, so nil here is an error rather than an empty slot.
    lua_pushnumber(L, -1);
    lua_pushnil(L);
    return 2;
}

// GetPlayerBuffTexture(slot) -> texture
static int lua_GetPlayerBuffTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    const std::string icon = gh->getSpellIconPath(aura->spellId);
    if (icon.empty()) return luaReturnNil(L);
    lua_pushstring(L, icon.c_str());
    return 1;
}

// GetPlayerBuffTimeLeft(slot) -> seconds
static int lua_GetPlayerBuffTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    // Zero rather than nil: buffframe.lua divides and compares this, and a
    // permanent aura is honestly zero time left rather than an absent answer.
    if (!aura || aura->durationMs <= 0) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, aura->getRemainingMs(auraNowMs()) / 1000.0);
    return 1;
}

// GetPlayerBuffApplications(slot) -> count
static int lua_GetPlayerBuffApplications(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    lua_pushnumber(L, aura ? aura->charges : 0);
    return 1;
}

// GetPlayerBuffDispelType(slot) -> "Magic" | "Curse" | "Disease" | "Poison"
static int lua_GetPlayerBuffDispelType(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    switch (gh->getSpellDispelType(aura->spellId)) {
        case 1:  lua_pushstring(L, "Magic");   return 1;
        case 2:  lua_pushstring(L, "Curse");   return 1;
        case 3:  lua_pushstring(L, "Disease"); return 1;
        case 4:  lua_pushstring(L, "Poison");  return 1;
        default: break;
    }
    return luaReturnNil(L);
}

// CancelPlayerBuff(slot)
static int lua_CancelPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (aura) gh->cancelAura(aura->spellId);
    return 0;
}

static int lua_UnitBuff(lua_State* L) { return lua_UnitAura(L, true); }
static int lua_UnitDebuff(lua_State* L) { return lua_UnitAura(L, false); }

// UnitAura(unit, index, filter) - generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    std::string f(filter ? filter : "HELPFUL");
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    bool wantBuff = (f.find("HARMFUL") == std::string::npos);
    return lua_UnitAura(L, wantBuff);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid ? uid : "player");

    // Use shared GetTime() epoch for consistent timestamps
    double nowSec = luaGetTimeNow();

    // Resolve cast state for the unit
    bool isCasting = false;
    bool isChannel = false;
    uint32_t spellId = 0;
    float timeTotal = 0.0f;
    float timeRemaining = 0.0f;
    bool interruptible = true;

    if (uidStr == "player") {
        isCasting = gh->isCasting();
        isChannel = gh->isChanneling();
        spellId = gh->getCurrentCastSpellId();
        timeTotal = gh->getCastTimeTotal();
        timeRemaining = gh->getCastTimeRemaining();
        // Player interruptibility: always true for own casts (server controls actual interrupt)
        interruptible = true;
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnNil(L); }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { return luaReturnNil(L); }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { return luaReturnNil(L); }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { return luaReturnNil(L); }

    // Spell name + icon
    const std::string& name = gh->getSpellName(spellId);
    std::string iconPath = gh->getSpellIconPath(spellId);

    // Time values in milliseconds (WoW API convention)
    double startTimeMs = (nowSec - (timeTotal - timeRemaining)) * 1000.0;
    double endTimeMs   = (nowSec + timeRemaining) * 1000.0;

    // The real signatures, which are one wider than what this used to send:
    //
    //   UnitCastingInfo → name, nameSubtext, text, texture, startTime,
    //                     endTime, isTradeSkill, castID, notInterruptible
    //   UnitChannelInfo → the same without castID
    //
    // nameSubtext is the rank, and leaving it out shifted every value after it
    // by one. castingbarframe.lua destructures all nine by name, so it read the
    // icon path as the bar's text, a timestamp as the texture, and a boolean as
    // endTime - then computed (endTime - startTime) and raised, which is why no
    // cast bar was ever drawn rather than a wrong one.
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                             // nameSubtext
    // What the bar is labelled with, which is the third value and not the
    // first: castingbarframe.lua reads `text` into barText and never touches
    // `name`. An empty string there is a bar that fills up saying nothing -
    // no way to tell a Fireball from a Frostbolt, or a cast from a channel.
    //
    // The spell's own name, because that is what this is for every spell. It
    // differs only for a trade skill, where the real client puts the item
    // being made here, and this client has no such cast in flight to name.
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // text
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_QuestionMark");  // texture
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0); // isTradeSkill
    if (!wantChannel) {
        lua_pushnumber(L, spellId);                              // castID (UnitCastingInfo only)
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 8 : 9;
}

static int lua_UnitCastingInfo(lua_State* L) { return lua_UnitCastInfo(L, false); }
static int lua_UnitChannelInfo(lua_State* L) { return lua_UnitCastInfo(L, true); }

static int lua_CastSpellByName(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* name = luaL_checkstring(L, 1);
    if (!name || !*name) return 0;

    const uint32_t bestId = highestKnownRankByName(gh, name);
    if (bestId != 0) {
        // The same second argument, and the same branch of SECURE_ACTIONS.spell
        // reaches here: a spell named rather than numbered is cast by name, and
        // the unit clicked still says who it lands on.
        uint64_t target = 0;
        if (const char* unit = luaL_optstring(L, 2, nullptr)) {
            std::string u(unit);
            toLowerInPlace(u);
            target = resolveUnitGuid(gh, u);
        }
        if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
        // 0x00540310 -> 0x0080da40 -> 0x0080cce0, which asks the taint check.
        if (spellCastAllowed(L, gh, bestId)) gh->castSpell(bestId, target);
    }
    return 0;
}

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
    return 1;
}

// --- Spell Book Tab API ---

// GetNumSpellTabs() → count
static int lua_GetNumSpellTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getSpellBookTabs().size());
    return 1;
}

// The book's second numbering: one slot per spell at its highest rank, which
// is what it shows with ShowAllSpellRanks off (SpellBook_GetTabInfo takes
// GetSpellTabInfo's last pair, SpellBook_GetSpellID maps back through
// GetKnownSlotFromHighestRankSlot). For each tab, the indices into its list
// that survive: of the spells sharing a name whose rank carries a number
// ("Rank 5"), the highest; every spell without one. Both answered the whole
// tab, so the book showed every rank whatever the box said.
static std::vector<size_t> highestRankIndices(const game::GameHandler& gh,
                                              const std::vector<uint32_t>& spellIds) {
    const auto rankNumber = [&](uint32_t id) {
        int n = 0;
        bool any = false;
        for (char c : gh.getSpellRank(id)) {
            if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); any = true; }
            else if (any) break;
        }
        return any ? n : -1;
    };
    std::unordered_map<std::string, size_t> best;  // name -> index of its highest rank
    for (size_t i = 0; i < spellIds.size(); ++i) {
        const int r = rankNumber(spellIds[i]);
        if (r < 0) continue;
        const std::string& name = gh.getSpellName(spellIds[i]);
        auto [it, inserted] = best.emplace(name, i);
        if (!inserted && r > rankNumber(spellIds[it->second])) it->second = i;
    }
    std::vector<size_t> out;
    out.reserve(spellIds.size());
    for (size_t i = 0; i < spellIds.size(); ++i) {
        if (rankNumber(spellIds[i]) < 0) { out.push_back(i); continue; }
        auto it = best.find(gh.getSpellName(spellIds[i]));
        if (it != best.end() && it->second == i) out.push_back(i);
    }
    return out;
}

// GetSpellTabInfo(tabIndex) → name, texture, offset, numSpells,
//                             highestRankOffset, highestRankNumSpells
// tabIndex is 1-based; offset is 1-based global spell book slot
//
// Six values, and the last two are not optional. SpellBook_GetTabInfo unpacks
// all six and then, when ShowAllSpellRanks is off - which is the default -
// throws away offset and numSpells and uses the highest-rank pair instead. So
// a four-value answer leaves both nil on the ordinary path and the spellbook
// raises on the arithmetic, showing nothing at all.
//
// The comment here used to say four. Cutting the returns to match it would
// have emptied the spellbook, which is exactly what a stubbed four-value
// version did when it was tried.
static int lua_GetSpellTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(luaL_checknumber(L, 1));
    // An empty tab rather than one nil, because the caller does arithmetic on
    // what comes back without checking it.
    //
    // SpellBookFrame's own OnLoad ends with SpellBookSkillLineTab_OnClick(nil,
    // 1), and three frames down that divides numSpells by the page size. In
    // WoW tab one is General and always exists, so FrameXML never guards it -
    // here it does not exist until the spell list has arrived, and a single
    // nil made that a raise inside OnLoad, which loses the whole file.
    // SpellBookFrame.xml was failing to load outright.
    //
    // Six zeroes give a book with no pages, which is what a player with no
    // spells yet has, and the frame loads and fills in when they arrive.
    auto emptyTab = [](lua_State* s) {
        lua_pushstring(s, "");   // name
        lua_pushstring(s, "");   // texture
        lua_pushnumber(s, 0);    // offset
        lua_pushnumber(s, 0);    // numSpells
        lua_pushnumber(s, 0);    // highestRankOffset
        lua_pushnumber(s, 0);    // highestRankNumSpells
        return 6;
    };
    if (!gh || tabIdx < 1) {
        return emptyTab(L);
    }
    const auto& tabs = gh->getSpellBookTabs();
    if (tabIdx > static_cast<int>(tabs.size())) {
        return emptyTab(L);
    }
    // Compute offset: sum of spells in all preceding tabs (1-based)
    int offset = 0;
    int highestOffset = 0;
    for (int i = 0; i < tabIdx - 1; ++i) {
        offset += static_cast<int>(tabs[i].spellIds.size());
        highestOffset += static_cast<int>(highestRankIndices(*gh, tabs[i].spellIds).size());
    }
    const auto& tab = tabs[tabIdx - 1];
    lua_pushstring(L, tab.name.c_str());           // name
    lua_pushstring(L, tab.texture.c_str());        // texture
    lua_pushnumber(L, offset);                     // offset (0-based for WoW compat)
    lua_pushnumber(L, tab.spellIds.size());        // numSpells
    // The highest-rank pair, which is what FrameXML actually reads: with
    // ShowAllSpellRanks off - the default - SpellBook_GetTabInfo throws away
    // the first offset and count and keeps these. Returning four values left
    // numSpells nil and the page count divided by nothing. See
    // highestRankIndices for the numbering.
    lua_pushnumber(L, highestOffset);              // highestRankOffset
    lua_pushnumber(L, highestRankIndices(*gh, tab.spellIds).size());  // highestRankNumSpells
    return 6;
}

// GetSpellBookItemInfo(slot, bookType) → "SPELL", spellId
// slot is 1-based global spell book index
static int lua_GetSpellBookItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) {
        lua_pushstring(L, "SPELL");
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot; // 1-based
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            lua_pushstring(L, "SPELL");
            lua_pushnumber(L, tab.spellIds[idx - 1]);
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, 0);
    return 2;
}

// GetSpellBookItemName(slot, bookType) → name, subName
static int lua_GetSpellBookItemName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) { return luaReturnNil(L); }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            uint32_t spellId = tab.spellIds[idx - 1];
            const std::string& name = gh->getSpellName(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, ""); // subName/rank
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushnil(L);
    return 1;
}


// ── The spellbook's older API ──────────────────────────────────────────────
//
// SpellBookFrame in WotLK uses both generations at once: the newer
// GetSpellBookItem* alongside GetSpellName and CastSpell, which take the same
// slot and book. The newer half was implemented and the older was not, so the
// panel could name a spell and not cast it.


/// GetSpellName(slot, bookType) → name, rank.
static int lua_GetSpellName(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Through spellIdForCall, which reads the book as well as the slot. The
    // spellbook passes SpellBookFrame.bookType to every one of these, and
    // resolving a pet slot through the player's tabs names one of the player's
    // own spells - the wrong answer wearing the shape of a right one.
    const uint32_t id = spellIdForCall(L, gh);
    if (id == 0) { return luaReturnNil(L); }
    const std::string& name = gh->getSpellName(id);
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
    // Its Spell.dbc rank ("Rank 5"), which the book prints under the name.
    lua_pushstring(L, gh->getSpellRank(id).c_str());
    return 2;
}

/// CastSpell(slot, bookType) - cast what is in that slot at the current
/// target, which is what clicking a spellbook entry does.
static int lua_CastSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same resolver, and here it decides what is cast: clicking a pet spell
    // resolved through the player's tabs cast whichever of the player's spells
    // sat at that index.
    const uint32_t id = spellIdForCall(L, gh);
    // 0x00541250 -> 0x0053bc40 -> 0x0080cce0, which asks the taint check.
    if (id != 0 && spellCastAllowed(L, gh, id)) gh->castSpell(id, gh->getTargetGuid());
    return 0;
}

/// IsPassiveSpell(slot, bookType) → whether the spell is passive, which is how
/// the book decides not to draw a cast button for it. Spell attributes are not
/// read from the DBC yet, so this answers no - which draws a button for a
/// passive rather than hiding a real one, the less wrong way round.
/// IsPassiveSpell(id or index, bookType) → whether the spell is never cast.
///
/// It answered no for everything, so the spell book drew a passive with the
/// same cast border as an ability and let it be dragged onto the action bar,
/// where it would sit doing nothing. Spell.dbc's base attribute word carries
/// it - bit 6 - beside the Ex word the client already read for
/// interruptibility.
static int lua_IsPassiveSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    // Both forms: IsPassiveSpell(spellId) and IsPassiveSpell(slot, bookType),
    // which is the one the spellbook uses - and reading a slot as an id
    // answered for whichever spell happens to carry the number 1, 2 or 3.
    // The result decides whether a spell is drawn as castable at all.
    const uint32_t id = spellIdForCall(L, gh);
    lua_pushboolean(L, gh->isSpellPassive(id) ? 1 : 0);
    return 1;
}

/// IsSelectedSpell(slot, bookType) → whether the book is highlighting it.
/// Nothing selects a spell here; the highlight follows the cursor.
static int lua_IsSelectedSpell(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// GetSpellAutocast(slot, bookType) → autocastable, currently autocasting.
/// Pet spells only, and pet action bars are not modelled: false says the
/// little rotating border is not drawn, which is right for every spell a
/// player has.
static int lua_GetSpellAutocast(lua_State* L) {
    lua_pushboolean(L, 0);
    lua_pushboolean(L, 0);
    return 2;
}
/// GetKnownSlotFromHighestRankSlot(slot) → the slot actually known: a slot
/// of the highest-rank numbering (highestRankIndices) back to the book's.
static int lua_GetKnownSlotFromHighestRankSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1) {
        lua_pushnumber(L, slot);
        return 1;
    }
    int bookOffset = 0;
    for (const auto& tab : gh->getSpellBookTabs()) {
        const auto kept = highestRankIndices(*gh, tab.spellIds);
        if (slot <= static_cast<int>(kept.size())) {
            lua_pushnumber(L, bookOffset + static_cast<int>(kept[slot - 1]) + 1);
            return 1;
        }
        slot -= static_cast<int>(kept.size());
        bookOffset += static_cast<int>(tab.spellIds.size());
    }
    lua_pushnumber(L, luaL_optnumber(L, 1, 0));
    return 1;
}

/// UpdateSpells() - redraw the spell book page.
///
/// The list itself needs no rebuilding, which is why this was a no-op. But in
/// WoW the call is what makes the client announce SPELLS_CHANGED, and that
/// event is the only thing that redraws a spell button: SpellButton_OnEvent
/// answers it with SpellButton_UpdateButton, and nothing else calls that.
///
/// Six places in spellbookframe.lua call this, and every one of them is a
/// "now redraw the page" - after a skill-line tab is clicked, after a page
/// turn, after the book type changes. SpellBookFrame_ShowSpells only calls
/// Show() on buttons that are already shown, so no OnShow fires and without
/// the event the page kept whatever it was displaying before. That is the
/// whole of "the spell book does not update when the tab changes": the offset
/// moved and nothing ever re-read it.
static int lua_UpdateSpells(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    // Safe against the obvious loop: SpellBookFrame_OnEvent answers
    // SPELLS_CHANGED with SpellBookFrame_Update() and no argument, and the
    // branch that would call back here only runs when it is passed one.
    if (engine) engine->fireEvent("SPELLS_CHANGED", {});
    return 0;
}

static int lua_GetSpellCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Three values on this path too, like every other. The cooldown frame does
    // `start > 0 and duration > 0 and enable > 0`, and `and` short-circuits, so
    // a missing third value is only ever reached when the first two say a
    // cooldown is running - which this path cannot say, because it answers
    // zero. Safe by accident rather than by design, and the next person to give
    // this branch a real start time would have found out the hard way.
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    // A name, an id, or a book slot with the book beside it - the spellbook
    // asks in the last of those three.
    const uint32_t spellId = spellIdForCall(L, gh);
    const char* book = !lua_isnoneornil(L, 2) && lua_isnumber(L, 1) ? lua_tostring(L, 2) : nullptr;
    // 0x00540e80: the player's history, or the pet's for the pet's book
    // (0x00809000) - the start and length of what runs longest of the
    // spell's cooldown, its category's and its global cooldown, in seconds,
    // and whether it is enabled (not held until an event).
    if (!book || std::string(book) != "pet") return pushHistoryCooldown(L, gh->playerSpellCooldown(spellId));
    return pushHistoryCooldown(L, gh->petSpellCooldown(spellId));
}

// TargetUnit(unitId) - set current target
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    // 0x00525a30 -> 0x005259e0 -> 0x00524bf0, which refuses insecure code
    // a target outright.
    if (guid != 0 && taint::allowedSecureOnly(L)) gh->targetUnit(guid);
    return 0;
}

// ClearTarget() - clear current target
// ClearTarget() (0x00525fc0) - 1 when there was a target to let go, nil
// when there was none. ToggleGameMenu reads it: "elseif ClearTarget() and
// not UnitIsCharmed("player") then" is the step that makes the first Escape
// with a target only clear it. Returning nothing, that step never held, and
// the same press went on to open the game menu.
static int lua_ClearTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool had = gh && gh->getTargetGuid() != 0;
    if (gh) gh->clearTarget();
    if (had) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}

// FocusUnit(unitId) - set focus target
static int lua_FocusUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, nullptr);
    if (!uid || !*uid) return 0;
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    // 0x0051ff20: a change of focus is refused to insecure code.
    if (guid != 0 && (guid == gh->getFocusGuid() || taint::allowedSecureOnly(L)))
        gh->setFocus(guid);
    return 0;
}

// ClearFocus() - clear focus target
static int lua_ClearFocus(lua_State* L) {
    auto* gh = getGameHandler(L);
    // 0x005225e0 -> 0x0051ff20: letting a focus go is a change of it too.
    if (gh && (gh->getFocusGuid() == 0 || taint::allowedSecureOnly(L))) gh->clearFocus();
    return 0;
}

// AssistUnit(unitId) - target whatever the given unit is targeting
static int lua_AssistUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    uint64_t theirTarget = getEntityTargetGuid(gh, guid);
    // 0x00525eb0 -> 0x005259e0 -> 0x00524bf0, refused to insecure code.
    if (theirTarget != 0 && taint::allowedSecureOnly(L)) gh->targetUnit(theirTarget);
    return 0;
}

// TargetTotem(slot) - 0x005262e0: the totem last placed in slot 1-4, by
// the guid SMSG_TOTEM_CREATED gave (0x00bd0b08); none there, nothing.
static int lua_TargetTotem(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: TargetTotem(slot)");
    const int slot = static_cast<int>(lua_tonumber(L, 1));
    if (slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS)
        return luaL_error(L, "Totem slot must be in range of 1 to %d", game::GameHandler::NUM_TOTEM_SLOTS);
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t guid = gh->getTotemSlot(slot - 1).guid;
    if (guid != 0) gh->targetUnit(guid);
    return 0;
}

// TargetLastTarget() - re-target previous target
static int lua_TargetLastTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    // 0x00525d70 -> 0x005259e0 -> 0x00524bf0, refused to insecure code.
    if (gh && taint::allowedSecureOnly(L)) gh->targetLastTarget();
    return 0;
}

// TargetNearestEnemy() - tab-target nearest enemy
// The argument is the direction, and it was being thrown away. Bindings.xml
// binds two keys to each of these - `TargetNearestEnemy()` forwards and
// `TargetNearestEnemy(1)` backwards, with a comment on that line saying so -
// so cycling backwards through targets went forwards instead, and the second
// key of every such pair did what the first did.
static bool wantsReverse(lua_State* L) {
    return lua_toboolean(L, 1) != 0;
}

// Every TargetNearest* is refused to insecure code first thing (0x00524fc0).
static int lua_TargetNearestEnemy(lua_State* L) {
    if (!taint::allowedSecureOnly(L)) return 0;
    auto* gh = getGameHandler(L);
    if (gh) gh->targetEnemy(wantsReverse(L));
    return 0;
}

// TargetNearest([reverse]) - the nearest unit of any kind (0x00525a90 ->
// 0x00524fc0 with no filter); reverse read as the client reads a switch.
static int lua_TargetNearest(lua_State* L) {
    if (!taint::allowedSecureOnly(L)) return 0;   // 0x00524fc0
    if (auto* gh = getGameHandler(L)) gh->targetNearest(luaClientSwitchArg(L, 1, false));
    return 0;
}

// ── TargetDirectionEnemy / TargetDirectionFriend ───────────────────────────
//
// The unit in a direction on screen, chosen as 0x005252d0 chooses it (see
// game/target_direction.hpp). The choice is held (0x00bd07d8) until
// TargetDirectionFinished clears it, and nothing else is chosen until then.

/// 0x00bd07d8: the unit the last scan chose.
static uint64_t& targetDirectionChosen() {
    static uint64_t chosen = 0;
    return chosen;
}

/// CreatureType.dbc's flag 1 (critters, companions, gas clouds): never chosen.
static bool creatureTypeSkipped(game::GameHandler* gh, uint32_t creatureType) {
    static std::unordered_map<uint32_t, uint32_t> flags;
    static bool loaded = false;
    if (!loaded) {
        auto* am = gh ? gh->services().assetManager : nullptr;
        if (am && am->isInitialized()) {
            loaded = true;
            if (auto dbc = am->loadDBCOptional("CreatureType.dbc"); dbc && dbc->getFieldCount() >= 2) {
                const uint32_t last = dbc->getFieldCount() - 1;
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    flags[dbc->getUInt32(i, 0)] = dbc->getUInt32(i, last);
                }
            }
        }
    }
    auto it = flags.find(creatureType);
    return it != flags.end() && (it->second & 1u) != 0;
}

static int targetDirection(lua_State* L, bool enemy) {
    if (!lua_isnumber(L, 1)) {
        return luaL_error(L, enemy ? "Usage: TargetDirectionEnemy(facing)"
                                   : "Usage: TargetDirectionFriend(facing)");
    }
    const float facing = static_cast<float>(lua_tonumber(L, 1));
    const float cone = lua_isnumber(L, 2) ? static_cast<float>(lua_tonumber(L, 2)) : 0.0f;
    if (!taint::allowedSecureOnly(L)) return 0;   // 0x00513530
    if (targetDirectionChosen() != 0) return 0;
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const rendering::Camera* cam = svc && svc->getCamera ? svc->getCamera() : nullptr;
    if (!gh || !cam) return 0;
    auto& em = gh->getEntityManager();
    const uint64_t playerGuid = gh->getPlayerGuid();
    const auto player = em.getEntity(playerGuid);
    if (!player) return 0;
    // World positions are the render space's: canonical with x and y swapped.
    const auto worldOf = [](const game::Entity& e) {
        return core::coords::canonicalToRender(glm::vec3(e.getX(), e.getY(), e.getZ()));
    };
    game::target_direction::View view;
    view.position = cam->getPosition();
    view.forward = cam->getForward();
    view.viewProjection = cam->getForwardDepthProjectionMatrix() * cam->getViewMatrix();
    view.nearPlane = cam->getNearPlane();
    view.farPlane = cam->getFarPlane();
    view.aspect = cam->getAspectRatio();

    // From the target where it is on screen, from the player otherwise.
    const glm::vec3 playerPos = worldOf(*player);
    uint64_t originGuid = playerGuid;
    glm::vec3 origin = playerPos;
    if (const uint64_t target = gh->getTargetGuid(); target != 0) {
        if (auto t = em.getEntity(target)) {
            game::target_direction::ScreenPoint sp;
            if (game::target_direction::project(view, worldOf(*t), sp)) {
                originGuid = target;
                origin = worldOf(*t);
            }
        }
    }

    // 0x00518e40: an enemy one the player may attack and alive; a friend one
    // the player may assist and with health.
    std::vector<game::target_direction::Candidate> candidates;
    for (const auto& [guid, entity] : em.getEntities()) {
        if (!entity || !entity->isUnit()) continue;
        const auto unit = std::static_pointer_cast<game::Unit>(entity);
        if (enemy) {
            if (!gh->unitMayAttack(playerGuid, guid)) continue;
            if (unit->getHealth() == 0 || (unit->getDynamicFlags() & 0x20u) != 0) continue;
        } else {
            if (!gh->unitMayAssist(playerGuid, guid) || unit->getHealth() < 1) continue;
        }
        if (entity->getType() == game::ObjectType::UNIT &&
            creatureTypeSkipped(gh, gh->getCreatureType(unit->getEntry()))) {
            continue;
        }
        candidates.push_back({guid, worldOf(*entity)});
    }
    const uint64_t chosen = game::target_direction::choose(view, facing, cone, originGuid, origin,
                                                           playerGuid, playerPos, candidates);
    if (chosen != 0) {
        gh->targetUnit(chosen);
        targetDirectionChosen() = chosen;
    }
    return 0;
}

// TargetDirectionEnemy(facing[, cone]) (0x00525c30), TargetDirectionFriend
// (0x00525cd0) and TargetDirectionFinished() (0x00515560).
static int lua_TargetDirectionEnemy(lua_State* L) { return targetDirection(L, true); }
static int lua_TargetDirectionFriend(lua_State* L) { return targetDirection(L, false); }
static int lua_TargetDirectionFinished(lua_State* L) {
    (void)L;
    targetDirectionChosen() = 0;
    return 0;
}

// InteractUnit(unit[, exactMatch]) (0x00527f00): the unit targeted and then
// right-clicked (0x005277b0) - 1 when it is there, nil when it is not, and 1
// with no player at all. "mouseover" is the unit under the pointer
// (0x00bd07a0).
static int lua_InteractUnit(lua_State* L) {
    if (!lua_isstring(L, 1))
        return luaL_error(L, "Usage: InteractUnit(unitToken[, exactMatch])");
    auto* gh = getGameHandler(L);
    if (!gh || !gh->getEntityManager().getEntity(gh->getPlayerGuid())) {
        lua_pushnumber(L, 1);
        return 1;
    }
    std::string token = lua_tostring(L, 1);
    toLowerInPlace(token);
    const uint64_t guid = resolveUnitGuid(gh, token);
    // Insecure code is refused once the unit is looked up, and answered
    // nothing (0x00527f00).
    if (!taint::allowedSecureOnly(L)) return 0;
    if (guid == 0 || !gh->getEntityManager().getEntity(guid)) {
        lua_pushnil(L);
        return 1;
    }
    gh->targetUnit(guid);
    auto* svc = getLuaServices(L);
    if (svc && svc->interactWithTarget) svc->interactWithTarget();
    lua_pushnumber(L, 1);
    return 1;
}

// AttackTarget() - the attack binding (0x0051a650): stops a running swing,
// else starts one at the target.
//
// Starting one asks the taint check (0x006e4950, kind 1): insecure code may
// stop a swing and never start one.
static int lua_AttackTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    if (!gh->isAutoAttacking() && gh->hasTarget() && !taint::allowed(L, 1)) return 0;
    gh->attackTarget();
    return 0;
}

// ToggleSheath() - the sheath binding (0x0051a6b0 -> 0x006e23a0), the same
// toggle the key makes.
static int lua_ToggleSheath(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->toggleSheath) svc->toggleSheath();
    return 0;
}

// TargetNearestFriend([reverse]) - target nearest friendly unit
static int lua_TargetNearestFriend(lua_State* L) {
    if (!taint::allowedSecureOnly(L)) return 0;   // 0x00524fc0
    auto* gh = getGameHandler(L);
    if (gh) gh->targetFriend(wantsReverse(L));
    return 0;
}

// GetRaidTargetIndex(unit) → icon index (1-8) or nil
static int lua_GetRaidTargetIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    uint8_t mark = gh->getEntityRaidMark(guid);
    if (mark == 0xFF) { return luaReturnNil(L); }
    lua_pushnumber(L, mark + 1); // WoW uses 1-indexed (1=Star, 2=Circle, ... 8=Skull)
    return 1;
}

// SetRaidTarget(unit, index) - set raid marker (1-8, or 0 to clear)
static int lua_SetRaidTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    int index = static_cast<int>(luaL_checknumber(L, 2));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    if (index >= 1 && index <= 8)
        gh->setRaidMark(guid, static_cast<uint8_t>(index - 1));
    else if (index == 0)
        gh->setRaidMark(guid, 0xFF); // clear
    return 0;
}

// --- GetSpellInfo / GetSpellTexture ---
// GetSpellInfo(spellIdOrName) -> name, rank, icon, castTime, minRange, maxRange, spellId
static int lua_GetSpellInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        spellId = highestKnownRankByName(gh, name);
    }

    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }

    lua_pushstring(L, name.c_str());                        // 1: name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                        // 2: rank
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);                                     // 3: icon texture path
    // The client's order, which this did not follow.
    //
    //   name, rank, icon, cost, isFunnel, powerType, castTime, minRange, maxRange
    //
    // Seven values were returned and only the first three were in the right
    // places. Everything asking for a cost got a cast time in milliseconds,
    // everything asking for a cast time got the spell id - so a spell read as
    // taking several thousand seconds to cast - and the two ranges were nil.
    // The interface unpacks all nine in one line in multicastactionbarframe,
    // and any addon reading a spell does the same.
    //
    // isFunnel is the one value with nothing behind it here. False is right
    // for every spell but a handful of warlock drains, and it is what the
    // callers branch on rather than display.
    auto spellData = gh->getSpellData(spellId);
    lua_pushnumber(L, spellData.manaCost);                   // 4: cost
    lua_pushboolean(L, 0);                                   // 5: isFunnel
    lua_pushnumber(L, spellData.powerType);                  // 6: powerType
    lua_pushnumber(L, spellData.castTimeMs);                 // 7: castTime (ms)
    lua_pushnumber(L, spellData.minRange);                   // 8: minRange (yards)
    lua_pushnumber(L, spellData.maxRange);                   // 9: maxRange (yards)
    return 9;
}

// GetSpellTexture(spellIdOrName) -> icon texture path string
static int lua_GetSpellTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    return 1;
}

// GetItemInfo(itemId) -> name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, vendorPrice

static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }
    const std::string link = game::spellChatLink(spellId, name);
    lua_pushstring(L, link.c_str());
    return 1;
}

/// CancelUnitBuff(unit, indexOrName [, filterOrRank]) - both forms.
///
/// The interface uses both and this took only the index, through
/// luaL_checknumber - so the two name forms raised on the spot rather than
/// cancelling anything. The possess bar cancels the aura holding the player in
/// a vehicle by name, and /cancelaura passes what was typed; both were an
/// error message where an aura should have dropped.
///
/// The third argument is a filter for the index form and a rank for the name
/// form, which is how WoW spells it. Only HELPFUL is answerable - a debuff
/// cannot be cancelled - so the filter decides nothing here beyond confirming
/// buffs are what is being counted.
static int lua_CancelUnitBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    if (uidStr != "player") return 0;   // only one's own auras can be cancelled

    const auto& auras = gh->getPlayerAuras();
    const bool byName = lua_isstring(L, 2) && !lua_isnumber(L, 2);

    if (byName) {
        std::string want(lua_tostring(L, 2));
        toLowerInPlace(want);
        for (const auto& a : auras) {
            if (a.isEmpty() || (a.flags & 0x80) != 0) continue;
            std::string have = gh->getSpellName(a.spellId);
            toLowerInPlace(have);
            if (have == want) { gh->cancelAura(a.spellId); return 0; }
        }
        return 0;
    }

    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    int buffCount = 0;
    for (const auto& a : auras) {
        if (a.isEmpty()) continue;
        if ((a.flags & 0x80) != 0) continue;   // debuffs are not cancellable
        if (++buffCount == index) { gh->cancelAura(a.spellId); return 0; }
    }
    return 0;
}

// CastSpellByID(spellId) - cast spell by numeric ID
static int lua_CastSpellByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (spellId == 0) return 0;
    // The second argument names who it is cast on, and this is the click-cast
    // path: SECURE_ACTIONS.spell does CastSpellByID(spellID, unit) with the
    // unit off the button that was clicked. Ignoring it cast on whatever was
    // targeted instead, so healing a party member through their frame hit the
    // current target.
    uint64_t target = 0;
    if (const char* unit = luaL_optstring(L, 2, nullptr)) {
        std::string u(unit);
        toLowerInPlace(u);
        target = resolveUnitGuid(gh, u);
    }
    if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
    // 0x0053e060 -> 0x0080da40 -> 0x0080cce0, which asks the taint check.
    if (spellCastAllowed(L, gh, spellId)) gh->castSpell(spellId, target);
    return 0;
}


int spellCastCheckKind(game::GameHandler* gh, uint32_t spellId) {
    if (!gh || spellId == 0) return 2;
    gh->loadSpellNameCache();
    const auto it = gh->spellNameCacheRef().find(spellId);
    if (it == gh->spellNameCacheRef().end()) return 2;
    const auto& spell = it->second;
    const uint32_t effect = spell.effectIds[0];
    if (effect == 0x53 || effect == 0x98 || effect == 0xa2) return -1;
    // 0x008009b0: the first effect a mount's aura (78), or a summon into the
    // companion slot - SummonProperties Slot (+0x10) 5.
    if (spell.effectAuraIds[0] == 78) return -1;
    if (effect == 28) {
        auto* am = gh->services().assetManager;
        auto props = am && am->isInitialized() ? am->loadDBCOptional("SummonProperties.dbc")
                                               : nullptr;
        const int32_t row = props && props->getFieldCount() > 4
            ? props->findRecordById(spell.effectMiscValuesB[0]) : -1;
        if (row >= 0 && props->getUInt32(static_cast<uint32_t>(row), 4) == 5) return -1;
    }
    if (effect == 47 || (spell.attr & 0x20u) != 0) return 6;
    return 2;
}

bool spellCastAllowed(lua_State* L, game::GameHandler* gh, uint32_t spellId) {
    const int kind = spellCastCheckKind(gh, spellId);
    return kind < 0 || taint::allowed(L, kind);
}

void registerSpellLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetTotemInfo",     lua_GetTotemInfo},
                // DestroyTotem(slot) - pull one down early. The totem bar's
                // right-click, and the slot is all the request carries.
                {"DestroyTotem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            // The bar counts from one and the request from zero.
            if (gh && slot >= 1) gh->destroyTotem(slot - 1);
            return 0;
        }},
                {"GetTotemTimeLeft", lua_GetTotemTimeLeft},
                {"SpellStopCasting", [](lua_State* L) -> int {
            if (!taint::allowed(L, 2)) return 0;   // 0x00809ea0
            auto* gh = getGameHandler(L);
            // 1 when a cast was stopped, nil when there was none - the next
            // step of ToggleGameMenu runs only on nil.
            const bool casting = gh && gh->isCasting();
            if (gh) gh->cancelCast();
            if (casting) lua_pushnumber(L, 1.0);
            else lua_pushnil(L);
            return 1;
        }},
                {"SpellStopTargeting", [](lua_State* L) -> int {
            // Puts down whatever the cursor holds for a target: a spell
            // waiting for a place on the ground or for a unit, or the item
            // cursor a stone or an oil arms. Asked of the taint check first
            // (0x00809e30, kind 2).
            if (!taint::allowed(L, 2)) return 0;
            auto* gh = getGameHandler(L);
            // 1 when something was waiting for a target, nil when not
            // (0x00809e30), as ToggleGameMenu reads it.
            const bool targeting = gh && (gh->isGroundTargeting() || gh->isAwaitingItemTarget() ||
                                          gh->isAwaitingGlyphTarget());
            if (gh) gh->cancelGroundTargeting();
            if (gh && gh->isAwaitingItemTarget()) gh->cancelItemTargeting();
            if (gh) gh->cancelGlyphTargeting();
            if (targeting) lua_pushnumber(L, 1.0);
            else lua_pushnil(L);
            return 1;
        }},
                {"SpellIsTargeting", [](lua_State* L) -> int {
            // 0x00d3f4e4: a spell is on the cursor waiting for its target.
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && (gh->isGroundTargeting() || gh->isUnitTargeting() ||
                                      gh->isAwaitingItemTarget() || gh->isAwaitingGlyphTarget()));
            return 1;
        }},
                // The world clicks the left and right buttons are bound to
                // (CAMERAORSELECTORMOVE, TURNORACTION). The mouse itself is
                // read by the client; these are what a macro or an addon calls.
                // Releasing the left one with a spell waiting for a place casts
                // it at the ground under the cursor (0x00527360 -> 0x0080c340);
                // releasing the right one puts the spell down.
                //
                // All four ask 0x005191c0(0) first (0x005fc6c0-0x005fc730,
                // 0x005fc610, 0x005fc680): kind 0, refused to insecure code.
                {"CameraOrSelectOrMoveStart", [](lua_State* L) -> int {
            taint::allowed(L, 0);
            return 0;
        }},
                {"CameraOrSelectOrMoveStop", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;
            auto* gh = getGameHandler(L);
            if (gh && gh->isGroundTargeting()) {
                if (auto at = gh->groundTargetCursor()) gh->placeGroundTarget(*at);
            }
            return 0;
        }},
                {"TurnOrActionStart", [](lua_State* L) -> int {
            taint::allowed(L, 0);
            return 0;
        }},
                {"TurnOrActionStop", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;
            auto* gh = getGameHandler(L);
            if (gh) gh->cancelGroundTargeting();
            return 0;
        }},
                {"UnitBuff",          lua_UnitBuff},
                {"UnitDebuff",        lua_UnitDebuff},
                {"UnitAura",          lua_UnitAuraGeneric},
                // The 1.12 names. A vanilla interface reads its buffs through
                // these and through nothing else.
                {"GetPlayerBuff",             lua_GetPlayerBuff},
                {"GetPlayerBuffTexture",      lua_GetPlayerBuffTexture},
                {"GetPlayerBuffTimeLeft",     lua_GetPlayerBuffTimeLeft},
                {"GetPlayerBuffApplications", lua_GetPlayerBuffApplications},
                {"GetPlayerBuffDispelType",   lua_GetPlayerBuffDispelType},
                {"CancelPlayerBuff",          lua_CancelPlayerBuff},
                {"UnitCastingInfo",   lua_UnitCastingInfo},
                {"UnitChannelInfo",   lua_UnitChannelInfo},
                {"CastSpellByName",   lua_CastSpellByName},
                {"CastSpellByID",       lua_CastSpellByID},
                {"IsSpellKnown",      lua_IsSpellKnown},
                {"GetNumSpellTabs",   lua_GetNumSpellTabs},
                {"GetSpellTabInfo",   lua_GetSpellTabInfo},
                {"GetSpellBookItemInfo", lua_GetSpellBookItemInfo},
                {"GetSpellBookItemName", lua_GetSpellBookItemName},
                {"GetSpellName",      lua_GetSpellName},
                {"SpellTargetUnit",   lua_SpellTargetUnit},
                {"SpellCanTargetUnit", lua_SpellCanTargetUnit},
                {"SpellTargetItem",   lua_SpellTargetItem},
                {"DropItemOnUnit",    lua_DropItemOnUnit},
                {"SetMultiCastSpell", lua_SetMultiCastSpell},
                {"CastSpell",         lua_CastSpell},
                {"IsPassiveSpell",    lua_IsPassiveSpell},
                // 1.12's name for the same question - renamed at 2.0, and
                // spellbookframe.lua calls it twice from
                // SpellButton_UpdateButton, which runs every time the book is
                // drawn. Unbound, opening the spellbook raised.
                {"IsSpellPassive",    lua_IsPassiveSpell},
                {"IsSelectedSpell",   lua_IsSelectedSpell},
                {"GetSpellAutocast",  lua_GetSpellAutocast},
                {"GetKnownSlotFromHighestRankSlot", lua_GetKnownSlotFromHighestRankSlot},
                {"UpdateSpells",      lua_UpdateSpells},
                {"GetSpellCooldown",  lua_GetSpellCooldown},
                {"GetSpellInfo",      lua_GetSpellInfo},
                {"GetSpellTexture",   lua_GetSpellTexture},
                {"GetSpellLink",         lua_GetSpellLink},
                {"CancelUnitBuff",      lua_CancelUnitBuff},
                {"TargetUnit",        lua_TargetUnit},
                {"ClearTarget",       lua_ClearTarget},
                {"FocusUnit",         lua_FocusUnit},
                {"ClearFocus",        lua_ClearFocus},
                {"AssistUnit",        lua_AssistUnit},
                {"TargetLastTarget",  lua_TargetLastTarget},
                {"TargetTotem",       lua_TargetTotem},
                {"TargetNearestEnemy",  lua_TargetNearestEnemy},
                {"TargetNearestFriend", lua_TargetNearestFriend},
                {"TargetNearest",       lua_TargetNearest},
                {"TargetDirectionEnemy",  lua_TargetDirectionEnemy},
                {"TargetDirectionFriend", lua_TargetDirectionFriend},
                {"TargetDirectionFinished", lua_TargetDirectionFinished},
                {"AttackTarget",        lua_AttackTarget},
                {"InteractUnit",        lua_InteractUnit},
                {"ToggleSheath",        lua_ToggleSheath},
                {"GetRaidTargetIndex",  lua_GetRaidTargetIndex},
                {"SetRaidTarget",       lua_SetRaidTarget},
                {"IsCurrentSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getCurrentCastSpellId() == spellId ? 1 : 0);
            return 1;
        }},
                {"IsAutoRepeatSpell", [](lua_State* L) -> int {
            (void)L; lua_pushboolean(L, 0); return 1; // Stub
        }},
                {"CastShapeshiftForm", [](lua_State* L) -> int {
            // CastShapeshiftForm(index) - cast the spell for the given form slot
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) return 0;
            uint8_t classId = gh->getPlayerClass();
            const auto forms = game::knownShapeshiftForms(classId, gh->getKnownSpells());
            if (static_cast<size_t>(index) > forms.size()) return 0;
            gh->castSpell(forms[static_cast<size_t>(index) - 1].spellId, 0);
            return 0;
        }},
                {"CancelShapeshiftForm", [](lua_State* L) -> int {
            // 0x0053de60 -> 0x00726ce0: with a form in effect (none while a
            // cancel is pending), cancel each positive aura (0x10) whose
            // shapeshift effect (aura 36) names that form.
            auto* gh = getGameHandler(L);
            if (!gh || game::isPreWotlk() || gh->getShapeshiftFormId() == 0) return 0;
            std::vector<uint32_t> spells;
            for (const auto& aura : gh->getPlayerAuras()) {
                if ((aura.flags & 0x10) != 0) spells.push_back(aura.spellId);
            }
            // The form is read again for each: the first cancel leaves it.
            for (uint32_t spellId : spells) {
                const uint8_t form = gh->getShapeshiftFormId();
                if (form != 0 && gh->spellShapeshiftsInto(spellId, form)) gh->cancelAura(spellId);
            }
            return 0;
        }},
                // GetShapeshiftFormCooldown is defined in the bootstrap, over
                // GetShapeshiftFormInfo and GetSpellCooldown, because those are
                // in two different files and it needs both. A stub here would
                // be dead - the bootstrap runs after these are registered - and
                // framexml_lua_override_check exists to catch exactly that.
                {"GetShapeshiftForm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnZero(L);
            if (game::isPreWotlk()) {
                lua_pushnumber(L, gh->getShapeshiftFormId());
                return 1;
            }
            // 0x0053de10 -> 0x0053d4b0: the active form's place on the bar,
            // not its form id. With a true argument, nothing while the form
            // field is clear.
            const uint8_t formField = gh->getShapeshiftFormId();
            if (lua_toboolean(L, 1) && formField == 0) return luaReturnZero(L);
            const auto forms = game::knownShapeshiftForms(gh->getPlayerClass(), gh->getKnownSpells());
            std::vector<bool> active;
            active.reserve(forms.size());
            for (const auto& form : forms) active.push_back(gh->formSpellInEffect(form.spellId));
            lua_pushnumber(L, game::activeShapeshiftIndex(active, formField));
            return 1;
        }},
                {"GetNumShapeshiftForms", [](lua_State* L) -> int {
            // Return count based on player class
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            uint8_t classId = gh->getPlayerClass();
            // Only the forms this character has actually learned, from the
            // one table GetShapeshiftFormInfo and CastShapeshiftForm also read.
            // The bar walks 1..this and asks the other two about each index, so
            // a count taken from a different list describes one form and casts
            // another.
            lua_pushnumber(L, static_cast<double>(
                game::knownShapeshiftForms(classId, gh->getKnownSpells()).size()));
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons

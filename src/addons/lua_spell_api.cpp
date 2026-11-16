// lua_spell_api.cpp - Spell info, casting, auras, and targeting Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/shapeshift_forms.hpp"
#include "game/game_utils.hpp"
#include "game/target_direction.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/tooltip_setter_rules.hpp"
#include "addons/unit_aura_call.hpp"
#include "game/totem_bar.hpp"
#include "game/spell_rank.hpp"
#include "game/spell_handler.hpp"
#include "game/interface_text.hpp"
#include "addons/lua_taint.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/item_text.hpp"
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
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
/// Rank arrives from the DBC as text in the client's language ("Rank 4",
/// "Rang 4"), so the number is read out of it wherever it stands; only an
/// English "rank " prefix used to count, and a German client cast whichever
/// rank the known-spell set yielded first.
uint32_t highestKnownRankByName(game::GameHandler* gh, const std::string& name) {
    // 0x00540200: the name's highest rank by the number in its rank text
    // (0x0053b540), or "Name(Rank)" for one rank - in any client language.
    std::vector<game::spell_rank::Candidate> known;
    known.reserve(gh->getKnownSpells().size());
    for (uint32_t sid : gh->getKnownSpells())
        known.push_back({sid, gh->getSpellName(sid), gh->getSpellRank(sid)});
    return game::spell_rank::spellByName(known, name);
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


// SetMultiCastSpell(slot, spellID) (0x005ab8a0): one of the totem bar's
// action slots, 133..144, under the taint check (kind 0xe). Spell 0 empties
// it; a spell of the slot's element's totem category is put there
// (0x005aae80); any other is refused with ERR_MULTI_CAST_ACTION_TOTEM_S
// naming the category the slot wants (0x005a7bc0).
static int lua_SetMultiCastSpell(lua_State* L) {
    if (!lua_isnumber(L, 1) && !lua_isnumber(L, 2))
        return luaL_error(L, "Usage: SetMultiCastSpell(slot, spellID)");
    if (!taint::allowed(L, 0xe)) return 0;
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int slot = static_cast<int>(lua_tonumber(L, 1)) - 1;
    if (slot < game::totem_bar::kFirstSlot || slot > game::totem_bar::kLastSlot) return 0;
    const auto spellId = static_cast<uint32_t>(lua_tonumber(L, 2));
    gh->loadSpellNameCache();
    if (spellId == 0) {
        gh->setActionBarSlot(slot, game::ActionBarSlot::EMPTY, 0);
        return 0;
    }
    const auto& cache = gh->spellNameCacheRef();
    const auto it = cache.find(spellId);
    if (it == cache.end()) return 0;
    const uint32_t cats[2] = {it->second.totemCategories[0], it->second.totemCategories[1]};
    if (!game::totem_bar::slotTakes(slot, cats)) {
        const std::string& name = gh->getTotemCategoryName(game::totem_bar::slotCategory(slot));
        gh->addUIError(game::interfaceFormat("ERR_MULTI_CAST_ACTION_TOTEM_S", "Requires %s", {name}));
        return 0;
    }
    gh->setActionBarSlot(slot, game::ActionBarSlot::SPELL, spellId);
    return 0;
}

// GetMultiCastTotemSpells(slot) (0x005a8330) → the element's totem spells:
// slot 1 fire, 2 earth, 3 water, 4 air, taken modulo four.
static int lua_GetMultiCastTotemSpells(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetMultiCastTotemSpells(slot)");
    auto* gh = getGameHandler(L);
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    if (!sh) return 0;
    gh->loadSpellNameCache();
    const int element = (static_cast<int>(lua_tonumber(L, 1)) - 1) & 3;
    const auto ids = sh->multiCastSpells(element);
    if (!lua_checkstack(L, static_cast<int>(ids.size()) + 1)) return 0;
    for (uint32_t id : ids) lua_pushnumber(L, id);
    return static_cast<int>(ids.size());
}


// --- Totems ---
//
// Four slots, each holding a spell and how long it has left. The interface
// wants the moment one was placed rather than its age, on the same clock
// GetTime() reports - a bar that is drawn from start and duration cannot use a
// figure measured from a different zero.

// The slot argument of the three totem calls, as 0x0051d330, 0x00517c60 and
// 0x0051d520 read it: a number or the usage line, truncated, and one to four
// or "Totem slot must be in range of 1 to 4". Zero-based on the way out.
static bool totemSlotArg(lua_State* L, const char* usage, int& slot) {
    if (!lua_isnumber(L, 1)) { luaL_error(L, "%s", usage); return false; }
    const int n = static_cast<int>(lua_tonumber(L, 1));
    if (n < 1 || n > game::GameHandler::NUM_TOTEM_SLOTS) {
        luaL_error(L, "Totem slot must be in range of 1 to %d", game::GameHandler::NUM_TOTEM_SLOTS);
        return false;
    }
    slot = n - 1;
    return true;
}

// GetTotemInfo(slot) → haveTotem, name, startTime, duration, icon
//
// 0x0051d330. With a totem in the slot: true, the totem's own unit name (nil
// until it is known - 0x0072ceb0 fills it in when the unit's name arrives),
// the moment it was placed and its duration, both in whole seconds, and the
// spell's icon. With none: whether the tool for that element is carried
// (TotemCategory 4, 2, 5, 3 - fire, earth, water, air - through 0x007548f0),
// "", 0, 0, "".
static int lua_GetTotemInfo(lua_State* L) {
    int slot = 0;
    if (!totemSlotArg(L, "Usage: GetTotemInfo(slot)", slot)) return 0;   // 0x0051d330
    auto* gh = getGameHandler(L);
    const auto* totem = gh ? &gh->getTotemSlot(slot) : nullptr;
    if (totem && totem->active()) {
        const int64_t remainingMs = totem->remainingMs();
        // The clock GetTime() reads, as the client's start is a stamp of the
        // same counter: placed as long ago as it has already run.
        const double placedMs = luaGetTimeNow() * 1000.0
            - (static_cast<double>(totem->durationMs) - static_cast<double>(remainingMs));
        lua_pushboolean(L, 1);
        const std::string& name = gh->lookupName(totem->guid);
        if (!name.empty()) lua_pushstring(L, name.c_str()); else lua_pushnil(L);
        lua_pushnumber(L, std::floor(std::max(0.0, placedMs) / 1000.0));
        lua_pushnumber(L, totem->durationMs / 1000);
        const std::string icon = gh->getSpellIconPath(totem->spellId);
        if (!icon.empty()) lua_pushstring(L, icon.c_str()); else lua_pushnil(L);
        return 5;
    }
    static constexpr uint32_t kToolCategory[game::GameHandler::NUM_TOTEM_SLOTS] = {4, 2, 5, 3};
    const auto* sh = gh ? gh->getSpellHandler() : nullptr;
    lua_pushboolean(L, sh && sh->carriesTotemCategory(kToolCategory[slot]) ? 1 : 0);
    lua_pushstring(L, "");
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushstring(L, "");
    return 5;
}

// GetTotemTimeLeft(slot) → whole seconds left, 0 for an empty slot (0x00517c60).
static int lua_GetTotemTimeLeft(lua_State* L) {
    int slot = 0;
    if (!totemSlotArg(L, "Usage: GetTotemTimeLeft(slot)", slot)) return 0;
    auto* gh = getGameHandler(L);
    const auto* totem = gh ? &gh->getTotemSlot(slot) : nullptr;
    lua_pushnumber(L, totem && totem->active() ? totem->remainingMs() / 1000 : 0);
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

// ── The order UnitAura counts in ────────────────────────────────────────────
//
// UnitAura, UnitBuff, UnitDebuff, CancelUnitBuff and the tooltip's
// SetUnitAura do not walk a unit's aura slots: they walk one of two ordered
// lists of them the client keeps on the unit (0x0072c9b0), list 0 for every
// filter but RAID and list 1 for RAID. 0x0072c510 keeps the lists: it first
// drops each entry whose slot no longer holds the spell it was taken in with
// (or holds it with none of the effect bits 1|2|4 up), closing the gap; then
// it walks the slots from the last to the first and takes in each aura it
// shows (0x0053d580) that a list does not hold yet. Taking one in
// (0x00727c00) puts an aura the player cast (0x0053bd70) after the player's
// own already there and ahead of everything else, one of the player's spells
// or summons (0x0053d750) after those and ahead of the rest, and any other at
// the end. The lists are kept between calls: an aura stays where it went in
// until it goes, so a new buff of the player's goes in after the player's
// older ones.
namespace {

using AuraOrderEntry = std::pair<uint32_t, uint32_t>;  // slot, spell
struct UnitAuraOrder {
    std::vector<AuraOrderEntry> lists[2];
};

std::unordered_map<uint64_t, UnitAuraOrder>& unitAuraOrders() {
    static std::unordered_map<uint64_t, UnitAuraOrder> orders;
    return orders;
}

const game::GameHandler::SpellNameEntry* auraSpellRow(game::GameHandler* gh, uint32_t spellId) {
    gh->loadSpellNameCache();
    const auto& cache = gh->spellNameCacheRef();
    const auto it = cache.find(spellId);
    return it == cache.end() ? nullptr : &it->second;
}

uint64_t unitGuidField(const game::Entity& e, game::UF field) {
    const uint16_t i = game::fieldIndex(field);
    if (i == 0xFFFF) return 0;
    return (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(i + 1))) << 32) | e.getField(i);
}

/// The area aura effects (0x0076bed0); 0x0076bd50 adds APPLY_AURA (6).
bool isAreaAuraEffect(uint32_t effect) {
    switch (effect) {
        case 27: case 35: case 65: case 119: case 128: case 129: case 143: return true;
        default: return false;
    }
}

bool playerOrPetKnows(game::GameHandler* gh, uint32_t spellId) {
    if (gh->getKnownSpells().count(spellId) != 0) return true;
    const auto& pet = gh->getPetSpells();
    return std::find(pet.begin(), pet.end(), spellId) != pet.end();
}

/// 0x0053bd70: the player cast it - the caster is the player, or a unit
/// whose charm (its summon when it charms nothing) or whose charmer (its
/// creator when nobody charms it) is the player.
bool auraCastByPlayer(game::GameHandler* gh, const game::AuraSlot& a) {
    const uint64_t me = gh->getPlayerGuid();
    if (me == 0 || a.casterGuid == 0) return false;
    if (a.casterGuid == me) return true;
    const auto caster = gh->getEntityManager().getEntity(a.casterGuid);
    if (!caster || !caster->isUnit()) return false;
    uint64_t charm = unitGuidField(*caster, game::UF::UNIT_FIELD_CHARM);
    if (charm == 0) charm = unitGuidField(*caster, game::UF::UNIT_FIELD_SUMMON);
    if (charm == me) return true;
    uint64_t owner = unitGuidField(*caster, game::UF::UNIT_FIELD_CHARMEDBY);
    if (owner == 0) owner = unitGuidField(*caster, game::UF::UNIT_FIELD_CREATEDBY);
    return owner == me;
}

/// The creatures the player's spells summon: the misc value of each known
/// spell whose first effect is SUMMON (28) or SUMMON_PET (56), as learning
/// one lists it (0x00542030).
bool playerSummonsEntry(game::GameHandler* gh, uint32_t entry) {
    if (entry == 0) return false;
    for (uint32_t id : gh->getKnownSpells()) {
        const auto* row = auraSpellRow(gh, id);
        if (row && (row->effectIds[0] == 28 || row->effectIds[0] == 56) && row->effectMiscValues[0] == entry)
            return true;
    }
    return false;
}

/// 0x0053d750: one of the player's (or the pet's) spells that puts an aura
/// on, or cast by a creature of the player's, one its spells summon.
bool auraOfPlayersSpells(game::GameHandler* gh, const game::AuraSlot& a) {
    if (playerOrPetKnows(gh, a.spellId)) {
        if (const auto* row = auraSpellRow(gh, a.spellId)) {
            for (uint32_t effect : row->effectIds) {
                if (effect == 6 || isAreaAuraEffect(effect)) return true;
            }
        }
    }
    if (a.casterGuid == 0) return false;
    const auto caster = gh->getEntityManager().getEntity(a.casterGuid);
    if (!caster || caster->getType() != game::ObjectType::UNIT) return false;
    uint64_t owner = unitGuidField(*caster, game::UF::UNIT_FIELD_CHARMEDBY);
    if (owner == 0) owner = unitGuidField(*caster, game::UF::UNIT_FIELD_CREATEDBY);
    return owner != 0 && playerSummonsEntry(gh, static_cast<const game::Unit&>(*caster).getEntry());
}

/// 0x0053d580: whether the aura is shown at all.
bool auraShown(game::GameHandler* gh, const game::GameHandler::SpellNameEntry& row, const game::AuraSlot& a) {
    if ((row.attr & 0x80u) != 0 || (row.attrEx & 0x10000000u) != 0) return false;
    if ((row.attr & 0x40u) != 0 &&
        std::none_of(std::begin(row.effectIds), std::end(row.effectIds), isAreaAuraEffect))
        return false;
    // Aura flag 0x08: the unit cast it on itself.
    if ((row.attrEx5 & ((a.flags & 0x08u) != 0 ? 0x8000000u : 0x10000000u)) != 0) return false;
    if ((row.attrEx6 & 0x100000u) != 0) {
        if (a.casterGuid == 0) return false;
        if (a.casterGuid != gh->getPlayerGuid() && a.casterGuid != gh->getPetGuid()) return false;
    }
    if (row.requiredAuraVision == 0) return true;
    const uint32_t bit = row.requiredAuraVision <= 8 ? 1u << (row.requiredAuraVision - 1) : 0u;
    return (gh->playerAuraVision() & bit) != 0;
}

/// The RAID list's test (0x0072c510): a buff one of the player's own spells
/// puts on others (0x0053d8a0), or a debuff of a type a spell the player
/// knows dispels from a friend (0x0053bdf0, the list 0x00542030 builds).
bool auraForRaid(game::GameHandler* gh, const game::GameHandler::SpellNameEntry& row, const game::AuraSlot& a) {
    if ((a.flags & 0x80u) == 0) {
        if (!playerOrPetKnows(gh, a.spellId)) return false;
        if ((row.attrEx3 & 0x4000u) != 0 || row.implicitTargetsA[0] == 1) return false;
        return std::none_of(std::begin(row.effectIds), std::end(row.effectIds), isAreaAuraEffect);
    }
    for (uint32_t id : gh->getKnownSpells()) {
        const auto* known = auraSpellRow(gh, id);
        if (!known) continue;
        for (int k = 0; k < 3; ++k) {
            if (known->effectIds[k] == 38 && known->implicitTargetsA[k] != 6 &&
                known->effectMiscValues[k] == row.dispelType)
                return true;
        }
    }
    return false;
}

/// 0x00727c00: where in its list a new aura goes.
void takeAuraIn(game::GameHandler* gh, std::vector<AuraOrderEntry>& list,
                const std::vector<game::AuraSlot>& auras, uint32_t slot) {
    const game::AuraSlot& a = auras[slot];
    auto at = list.size();
    const auto entryAura = [&](size_t i) -> const game::AuraSlot* {
        return list[i].first < auras.size() ? &auras[list[i].first] : nullptr;
    };
    if (auraCastByPlayer(gh, a)) {
        while (at > 0) {
            const auto* e = entryAura(at - 1);
            if (e && auraCastByPlayer(gh, *e)) break;
            --at;
        }
    } else if (auraOfPlayersSpells(gh, a)) {
        while (at > 0) {
            const auto* e = entryAura(at - 1);
            if (e && (auraCastByPlayer(gh, *e) || auraOfPlayersSpells(gh, *e))) break;
            --at;
        }
    }
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), AuraOrderEntry{slot, a.spellId});
}

}  // namespace

/// The unit's auras in the order the client counts them, list 1 for RAID
/// (0x0072c510 bringing the lists up to date first, as 0x0072c9b0 does).
/// Before WotLK the slots, in order.
static std::vector<const game::AuraSlot*> unitAuraWalk(game::GameHandler* gh, uint64_t guid,
                                                const std::vector<game::AuraSlot>& auras, bool raid) {
    std::vector<const game::AuraSlot*> out;
    if (game::isPreWotlk() || guid == 0) {
        for (const auto& a : auras)
            if (!a.isEmpty()) out.push_back(&a);
        return out;
    }
    auto& orders = unitAuraOrders();
    if (orders.size() > 512 && orders.find(guid) == orders.end()) orders.clear();
    UnitAuraOrder& order = orders[guid];
    for (auto& list : order.lists) {
        std::erase_if(list, [&](const AuraOrderEntry& e) {
            return e.first >= auras.size() || auras[e.first].spellId != e.second || (auras[e.first].flags & 7u) == 0;
        });
    }
    for (size_t s = auras.size(); s-- > 0;) {
        const game::AuraSlot& a = auras[s];
        if (a.spellId == 0 || (a.flags & 7u) == 0) continue;
        const auto* row = auraSpellRow(gh, a.spellId);
        if (!row || !auraShown(gh, *row, a)) continue;
        const auto holds = [&](const std::vector<AuraOrderEntry>& list) {
            return std::any_of(list.begin(), list.end(),
                               [&](const AuraOrderEntry& e) { return e.first == s && e.second == a.spellId; });
        };
        if (!holds(order.lists[0])) takeAuraIn(gh, order.lists[0], auras, static_cast<uint32_t>(s));
        if (auraForRaid(gh, *row, a) && !holds(order.lists[1]))
            takeAuraIn(gh, order.lists[1], auras, static_cast<uint32_t>(s));
    }
    for (const auto& e : order.lists[raid ? 1 : 0]) out.push_back(&auras[e.first]);
    return out;
}

UnitAuraAnswer& lastUnitAuraAnswer() {
    static UnitAuraAnswer answer;
    return answer;
}

/// What UnitAura, UnitBuff and UnitDebuff are asked (0x00608390): an index,
/// or a name with an optional rank, and a filter of words split on '|'.
/// `forced` is 1 for UnitBuff (HELPFUL), 2 for UnitDebuff (HARMFUL), 0 for
/// UnitAura (HELPFUL unless the filter says HARMFUL).
static int lua_UnitAura(lua_State* L, int forced) {
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }
    std::string uidStr(lua_tostring(L, 1));
    toLowerInPlace(uidStr);
    int index = 0;
    std::string wantName, wantRank;
    int filterArg = 3;
    if (lua_isnumber(L, 2)) {
        index = static_cast<int>(lua_tointeger(L, 2));
    } else {
        wantName = lua_tostring(L, 2);
        if (lua_isstring(L, 3)) wantRank = lua_tostring(L, 3);
        filterArg = 4;
    }
    bool helpful = true, onlyPlayer = false, raid = false, cancelable = false, notCancelable = false;
    if (lua_isstring(L, filterArg)) {
        std::string f(lua_tostring(L, filterArg));
        for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        size_t at = 0;
        while (at <= f.size()) {
            const size_t bar = f.find('|', at);
            const std::string word = f.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
            if (word == "HELPFUL") helpful = true;
            else if (word == "HARMFUL") helpful = false;
            else if (word == "PLAYER") onlyPlayer = true;
            else if (word == "RAID") raid = true;
            else if (word == "CANCELABLE") cancelable = true;
            else if (word == "NOT_CANCELABLE") notCancelable = true;
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
    }
    if (forced == 1) helpful = true;
    if (forced == 2) helpful = false;
    const bool wantBuff = helpful;
    if (wantName.empty() && index < 1) { return 0; }

    const std::vector<game::AuraSlot>* auras = nullptr;
    uint64_t unitGuid = 0;
    if (uidStr == "player") {
        auras = &gh->getPlayerAuras();
        unitGuid = gh->getPlayerGuid();
    } else if (uidStr == "target") {
        auras = &gh->getTargetAuras();
        unitGuid = gh->getTargetGuid();
    } else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        unitGuid = resolveUnitGuid(gh, uidStr);
        if (unitGuid != 0) auras = gh->getUnitAuras(unitGuid);
    }
    if (!auras) { return 0; }

    // Before WotLK, "RAID" on a debuff means only what this character can
    // remove; from WotLK on the RAID list itself says what is in it.
    const bool dispellableOnly = raid && !wantBuff && game::isPreWotlk();
    const uint8_t playerClass = gh->getPlayerClass();

    // Filter to buffs or debuffs and find the Nth one, in the order of the
    // unit's list (0x0072c9b0).
    int found = 0;
    for (const game::AuraSlot* auraPtr : unitAuraWalk(gh, unitGuid, *auras, raid)) {
        const auto& aura = *auraPtr;
        if (aura.isEmpty() || aura.spellId == 0) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        if (dispellableOnly &&
            !classCanDispel(playerClass, gh->getSpellDispelType(aura.spellId))) {
            continue;
        }
        // PLAYER: cast by the player; CANCELABLE and NOT_CANCELABLE by the
        // aura's flag 0x10 (0x0072c9b0).
        if (onlyPlayer && aura.casterGuid != gh->getPlayerGuid()) continue;
        if ((aura.flags & 0x10) ? notCancelable : cancelable) continue;
        bool hit;
        if (!wantName.empty()) {
            std::string have = gh->getSpellName(aura.spellId), rank = gh->getSpellRank(aura.spellId);
            std::string want = wantName, wantR = wantRank;
            toLowerInPlace(have); toLowerInPlace(rank); toLowerInPlace(want); toLowerInPlace(wantR);
            hit = have == want && (wantR.empty() || rank == wantR);
        } else {
            hit = ++found == index;
        }
        if (hit) {
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
            // 0x006147c0: name, rank, icon, count, debuffType, duration,
            // expirationTime, caster unit (0x0060b420, nil when the caster
            // has no unit token), isStealable and shouldConsolidate (1 or
            // nil), spellId.
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
            lua_pushstring(L, gh->getSpellRank(aura.spellId).c_str()); // rank
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
                else
                    lua_pushnil(L);
            } else {
                lua_pushnil(L);
            }
            lua_pushnil(L);                  // isStealable
            lua_pushnil(L);                  // shouldConsolidate
            lua_pushnumber(L, aura.spellId); // spellId
            lastUnitAuraAnswer() = {true, aura.level, aura.charges};
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

static int lua_UnitBuff(lua_State* L) {
    // 0x00614ca0 (through 0x006110a0): a unit text, then an index or a name - a
    // number is also a string to that test.
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitBuff(\"unit\", [index] or [\"name\", \"rank\"][, \"filter\"])");
    return lua_UnitAura(L, 1);
}
static int lua_UnitDebuff(lua_State* L) {
    // 0x00614cf0 (through 0x006110a0): a unit text, then an index or a name - a
    // number is also a string to that test.
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitDebuff(\"unit\", [index] or [\"name\", \"rank\"][, \"filter\"])");
    return lua_UnitAura(L, 2);
}

// UnitAura(unit, index, filter) - generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    // 0x00614d40 (through 0x006110a0): a unit text, then an index or a name - a
    // number is also a string to that test.
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "Usage: UnitAura(\"unit\", [index] or [\"name\", \"rank\"][, \"filter\"])");
    return lua_UnitAura(L, 0);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    // No cast: nothing (0x00611df0, 0x00612090). The token without case.
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }

    std::string uidStr(lua_tostring(L, 1));
    toLowerInPlace(uidStr);

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
        if (guid == 0) { return 0; }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { return 0; }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { return 0; }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { return 0; }

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
    // The text: a cast's is "" for AttributesEx3 0x4; the player's channel
    // reads CHANNELING unless AttributesEx 0x20000000.
    const auto& cache = gh->spellNameCacheRef();
    const auto entry = cache.find(spellId);
    const uint32_t attr = entry != cache.end() ? entry->second.attr : 0u;
    const uint32_t attrEx = entry != cache.end() ? entry->second.attrEx : 0u;
    const uint32_t attrEx3 = entry != cache.end() ? entry->second.attrEx3 : 0u;
    if (wantChannel) {
        if (uidStr == "player" && !(attrEx & 0x20000000u)) {
            lua_getglobal(L, "CHANNELING");
            if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, name.c_str()); }
        } else {
            lua_pushstring(L, name.c_str());
        }
    } else {
        lua_pushstring(L, (attrEx3 & 0x4u) ? "" : name.c_str());
    }
    lua_pushstring(L, iconPath.c_str());                        // texture, "" when none
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, (attr & 0x20u) ? 1 : 0);                 // isTradeSkill (Attributes 0x20)
    if (!wantChannel) {
        // castID, the cast's count byte (+0xa5c).
        const auto* sh = gh->getSpellHandler();
        lua_pushnumber(L, uidStr == "player" && sh ? sh->runningCastCount() : 0);
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 8 : 9;
}

static int lua_UnitCastingInfo(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitCastingInfo(\"unit\")");   // 0x00611df0
    return lua_UnitCastInfo(L, false);
}
static int lua_UnitChannelInfo(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: UnitChannelInfo(\"unit\")");   // 0x00612090
    return lua_UnitCastInfo(L, true);
}

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

// IsSpellKnown(spellID[, isPet]) (0x0053c3a0): a positive number or the
// usage line; the pet's spells with isPet set.
static int lua_IsSpellKnown(lua_State* L) {
    const long id = lua_isnumber(L, 1) ? static_cast<long>(lua_tointeger(L, 1)) : 0;
    if (id <= 0) return luaL_error(L, "Usage: IsSpellKnown(spellID[, isPet])");
    auto* gh = getGameHandler(L);
    const auto spellId = static_cast<uint32_t>(id);
    bool known = false;
    if (gh && luaClientSwitchArg(L, 2, false)) {
        for (const uint32_t s : gh->getPetSpells()) known = known || s == spellId;
    } else if (gh) {
        known = gh->getKnownSpells().count(spellId) != 0;
    }
    lua_pushboolean(L, known ? 1 : 0);
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
    // 0x0053be70: a number or the usage line, rounded.
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetSpellTabInfo(index)");
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(std::lround(lua_tonumber(L, 1)));
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
    // 0x005407f0: name and rank, or nil, nil for a slot or spell not known.
    const uint32_t id = spellIdForCall(L, gh);
    if (id == 0) { lua_pushnil(L); lua_pushnil(L); return 2; }
    const std::string& name = gh->getSpellName(id);
    lua_pushstring(L, name.c_str());
    // Its Spell.dbc rank ("Rank 5"), which the book prints under the name.
    lua_pushstring(L, gh->getSpellRank(id).c_str());
    return 2;
}

/// CastSpell(slot, bookType[, selfCast]) - cast what is in that slot at the
/// current target, which is what clicking a spellbook entry does.
static int lua_CastSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same resolver, and here it decides what is cast: clicking a pet spell
    // resolved through the player's tabs cast whichever of the player's spells
    // sat at that index.
    const uint32_t id = spellIdForCall(L, gh);
    // The third argument is the ALT-click of spellbookframe.lua's
    // IsModifiedClick("SELFCAST") branch.  0x00541250 takes the target off the
    // client's own pair (DAT_00bd07b0/b4, the pair that elsewhere is looked up
    // as a unit) and, when 0x00815500(L,3) answers a truth, hands the cast
    // routine 0x004d3790 in its place - the value that elsewhere is checked-
    // cast as Player_C.  So an ALT-click in the book is not "the target, if
    // any" but this player, target or no target; and over there the player is
    // named outright rather than spelled as a zero target, which is how some of
    // the self-casts here ask.
    const bool selfCast = lua_toboolean(L, 3) != 0;
    // 0x00541250 -> 0x0053bc40 -> 0x0080cce0, which asks the taint check.
    if (id != 0 && spellCastAllowed(L, gh, id))
        gh->castSpell(id, selfCast ? gh->getPlayerGuid() : gh->getTargetGuid());
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
    if (!gh) { luaPushOneOrNil(L, 0); return 1; }
    // Both forms: IsPassiveSpell(spellId) and IsPassiveSpell(slot, bookType),
    // which is the one the spellbook uses - and reading a slot as an id
    // answered for whichever spell happens to carry the number 1, 2 or 3.
    // The result decides whether a spell is drawn as castable at all.
    const uint32_t id = spellIdForCall(L, gh);
    luaPushOneOrNil(L, gh->isSpellPassive(id) ? 1 : 0);
    return 1;
}

/// IsSelectedSpell(slot, bookType) → whether the book is highlighting it.
/// Nothing selects a spell here; the highlight follows the cursor.
// IsSelectedSpell(slot, bookType) (0x005412c0 -> 0x0053d350): a profession's
// spell (effect 47) while its trade skill window is open, or a form's spell
// (aura 36) while the player is in that form.
static int lua_IsSelectedSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t id = spellIdForCall(L, gh);
    bool selected = false;
    if (gh && id != 0) {
        gh->loadSpellNameCache();
        const auto& cache = gh->spellNameCacheRef();
        if (const auto it = cache.find(id); it != cache.end()) {
            const auto& e = it->second;
            if (e.effectIds[0] == 47 && gh->isCraftingWindowOpen() &&
                gh->getCraftingSkillLine() == gh->spellSkillLine(id)) {
                selected = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    if (e.effectAuraIds[i] != 36) continue;
                    const uint32_t form = e.effectMiscValues[i];
                    selected = form != 0 && gh->getShapeshiftFormId() == form;
                    break;
                }
            }
        }
    }
    luaPushOneOrNil(L, selected ? 1 : 0);
    return 1;
}

/// GetSpellAutocast(slot, bookType) → autocastable, currently autocasting.
/// Pet spells only, and pet action bars are not modelled: false says the
/// little rotating border is not drawn, which is right for every spell a
/// player has.
// GetSpellAutocast(slot, bookType) (0x00541010) → autocastable, autostate:
// the pet book's spell on the pet's bar, each 1 or nil; nil, nil for the
// player's book.
static int lua_GetSpellAutocast(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t id = spellIdForCall(L, gh);
    const char* book = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const bool pet = (book[0] == 'p' || book[0] == 'P') && (book[1] == 'e' || book[1] == 'E') &&
                     (book[2] == 't' || book[2] == 'T') && book[3] == '\0';
    const bool castable = gh && pet && id != 0 && gh->isPetSpellAutocastable(id);
    luaPushOneOrNil(L, castable ? 1 : 0);
    luaPushOneOrNil(L, castable && gh->isPetSpellAutocast(id) ? 1 : 0);
    return 2;
}
/// GetKnownSlotFromHighestRankSlot(slot) → the slot actually known: a slot
/// of the highest-rank numbering (highestRankIndices) back to the book's.
static int lua_GetKnownSlotFromHighestRankSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(lua_tointeger(L, 1));
    if (!gh || slot < 1) {
        lua_pushnumber(L, 0);
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
    // 0x0053b650: 0 for a slot that maps to none.
    lua_pushnumber(L, 0);
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

// TargetUnit(unit, exactMatch) - set current target.
// The first argument goes to the name resolver
// (0x00525a30 -> 0x00520190) with mask 8 - any unit, creature or player, but
// never a doodad - so a typed "/tar Felwolf" reaches the wolf standing nearby
// and not just a unit already named by a token. The second argument is
// exactMatch: the unit menu and "/targetexact" set it and compare whole names,
// "/tar Bo" without it compares as many characters as were typed and takes the
// nearest of what matches.
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    const bool exact = lua_toboolean(L, 2) != 0;
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (!guid) guid = resolveNamedUnit(gh, uidStr, exact, false);
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
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetRaidTargetIndex(unit)");   // 0x00572ab0
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
    // SetRaidTarget (0x00574ab0): a mark for a player outside the group who
    // is not of the player's side and peaceable (0x00729b30) is refused with
    // ERR_INVALID_RAID_TARGET - enemy players are not marked.
    if (index >= 1 && index <= 8 && guid != gh->getPlayerGuid() && !gh->isPlayerOrGroupUnit(guid)) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::PLAYER &&
            !gh->unitsCooperate(gh->getPlayerGuid(), guid)) {
            gh->playGameErrorSound("ERR_INVALID_RAID_TARGET");
            gh->addUIError(game::interfaceString("ERR_INVALID_RAID_TARGET",
                                                 "You can't mark that target."));
            return 0;
        }
    }
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
    // The cost is over the power's divisor (rage and runic power in tenths),
    // and a spell that costs something each second - Health Funnel - shows
    // that instead, with isFunnel true (0x00540a30, 0x007ff100). The power
    // type is signed: -2 is health.
    auto spellData = gh->getSpellData(spellId);
    const int32_t powerType = static_cast<int8_t>(spellData.powerType);
    const uint32_t divisor = powerType >= 0 && powerType < 7 ? kPowerDivisors[powerType] : 1;
    const uint32_t perSecond = spellData.manaPerSecond / divisor;
    const bool funnel = perSecond >= 1;
    lua_pushnumber(L, funnel ? perSecond : spellData.manaCost / divisor);  // 4: cost
    lua_pushboolean(L, funnel ? 1 : 0);                      // 5: isFunnel
    lua_pushnumber(L, powerType);                            // 6: powerType
    // 7: castTime (ms), 0 for a passive spell (Attributes 0x40).
    constexpr uint32_t kAttrPassive = 0x40;
    const auto& spells = gh->spellNameCacheRef();
    const auto row = spells.find(spellId);
    const bool passive = row != spells.end() && (row->second.attr & kAttrPassive);
    lua_pushnumber(L, passive ? 0 : spellData.castTimeMs);
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

/// GetSpellLink(spellId [, bookType]) - the spell link, and a second value this
/// does not answer.
///
/// The reference (0x005408e0) always returns two. The first is this one, built
/// the same way; the second is nil unless the spell belongs to a trade skill,
/// in which case it is a whole other link that 0x005db060 writes:
/// "|cffffd000|Htrade:%d:%d:%d:%I64X:" with the spell, two numbers taken off
/// the trade-skill record that spell resolves to, the player's own guid as hex,
/// and then the recipes of that skill base64'd six to a byte - a "which of this
/// smith's list can you actually make" mask, one bit test per recipe against the
/// player's known spells. spellbookframe.lua:372 unpacks that pair and prefers
/// the second: shift-clicking a recipe in the spell book links the *trade
/// skill*, not the spell.
///
/// That second link is out of reach here for the reason the three
/// GetTradeSkill*Link names are - it is built off a trade-skill record list, and
/// neither dbc folder on this machine holds a Trade table of any name, as the
/// GetTradeSkillListLink row a few files over also records. Answering one value
/// rather than the pair costs nothing in this interface tree: every other caller
/// takes the first value alone (blizzard_combatlog, petpaperdollframe,
/// vehiclemenubar).
static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    // A spell with Attributes 0x80 and 0x100 both has no link (0x005408e0).
    const auto& cache = gh->spellNameCacheRef();
    const auto it = cache.find(spellId);
    constexpr uint32_t kHiddenA = 0x80, kHiddenB = 0x100;
    if (name.empty() || (it != cache.end() && (it->second.attr & kHiddenA) && (it->second.attr & kHiddenB))) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    lua_pushstring(L, game::spellChatLink(spellId, name).c_str());
    // The second: a trade skill's (effect 47 first) list of the player's
    // recipes as a trade link (0x005db060), nil for any other spell.
    constexpr uint32_t kEffectTradeSkill = 47;
    const std::string trade = it != cache.end() && it->second.effectIds[0] == kEffectTradeSkill
        ? tradeSkillLink(gh, spellId) : std::string();
    if (trade.empty()) lua_pushnil(L);
    else lua_pushstring(L, trade.c_str());
    return 2;
}

/// CancelUnitBuff(unit, indexOrName [, filterOrRank]) - both forms.
///
/// The interface uses both and this took only the index, through
/// luaL_checknumber - so the two name forms raised on the spot rather than
/// cancelling anything. The possess bar cancels the aura holding the player in
/// a vehicle by name, and /cancelaura passes what was typed; both were an
/// error message where an aura should have dropped.
///
/// Both forms come out of one argument parser the reference shares with
/// UnitBuff, UnitDebuff, UnitAura and the tooltip's SetUnitAura /
/// SetUnitBuff / SetUnitDebuff (0x006110a0 -> 0x00608390). A number is stored
/// as `index - 1`; a name takes the *next* argument as its rank, which is why
/// the filter sits in the third place after an index and the fourth after a
/// name. Read as a filter it is not one word but a set, split on '|': HELPFUL
/// and HARMFUL replace each other (1 and 2), PLAYER sets 4, RAID 8,
/// CANCELABLE 0x10, NOT_CANCELABLE 0x20.
///
/// Of that set only HELPFUL is reachable here. The buff frame's debuff icons
/// are not clickable at all - DebuffButtonTemplate inherits the AuraButtonTemplate,
/// which carries only OnEnter and OnLeave, and it is BuffButtonTemplate alone
/// that adds the OnClick calling this - so no caller hands over HARMFUL, and
/// counting buffs is what the one caller that passes a filter asks for.
///
/// The rank is not compared: no rank data is loaded here (Spell.dbc 3.3.5 names
/// its ranks through the SpellRank table), and one rank of a spell at a time is
/// all a unit can carry, so a name match picks the same aura either way.
static int lua_CancelUnitBuff(lua_State* L) {
    // The reference parses the unit and the aura's ident in one go, and insists
    // on a unit text and, next to it, a number or a string (0x006110a0 asks for
    // both before 0x00608390 reads the second either way). Given neither it
    // raises its Usage line (0x00804220), before the player is looked at; this
    // returned without touching anything, which hid a typo in a macro as an
    // aura that would not drop.
    if (!lua_isstring(L, 1) || (!lua_isnumber(L, 2) && !lua_isstring(L, 2))) {
        return luaL_error(L, "Usage: CancelUnitBuff(\"unit\", [index] or [\"name\", \"rank\"][, \"filter\"])");
    }
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    if (uidStr != "player") return 0;   // only one's own auras can be cancelled

    // Counted in the order of the player's aura list, as UnitBuff counts
    // (0x00608390 -> 0x0072c9b0), so the button clicked is the buff dropped.
    const auto auras = unitAuraWalk(gh, gh->getPlayerGuid(), gh->getPlayerAuras(), false);
    const bool byName = lua_isstring(L, 2) && !lua_isnumber(L, 2);

    if (byName) {
        std::string want(lua_tostring(L, 2));
        toLowerInPlace(want);
        for (const game::AuraSlot* ap : auras) {
            const auto& a = *ap;
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
    for (const game::AuraSlot* ap : auras) {
        const auto& a = *ap;
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
            int slot = 0;   // 0x0051d520
            if (!totemSlotArg(L, "Usage: DestroyTotem(slot)", slot)) return 0;
            if (auto* gh = getGameHandler(L)) gh->destroyTotem(slot);
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
            // With no cast, a repeating shot is what stops: the client sends
            // CMSG_CANCEL_AUTO_REPEAT_SPELL and answers 1 (0x00807560(1)).
            const bool repeating = gh && !casting && gh->isAutoRepeating();
            if (repeating) gh->stopAutoRepeat(true);
            if (casting || repeating) lua_pushnumber(L, 1.0);
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
            // A spell held for a unit counts as much as one held for the
            // ground: putting either down is this step's answer. Without the
            // unit's, one Escape put the spell down and went on through the
            // windows and the target to the game menu.
            const bool targeting = gh && (gh->isGroundTargeting() || gh->isUnitTargeting() ||
                                          gh->isAwaitingItemTarget() || gh->isAwaitingGlyphTarget());
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
            luaPushOneOrNil(L, gh && (gh->isGroundTargeting() || gh->isUnitTargeting() ||
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
                {"SetMultiCastSpell", lua_SetMultiCastSpell},
                {"GetMultiCastTotemSpells", lua_GetMultiCastTotemSpells},
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
            // 0x00541500 -> 0x00806030 (GameHandler::isCurrentSpell).
            luaPushOneOrNil(L, gh && gh->isCurrentSpell(spellId) ? 1 : 0);
            return 1;
        }},
                // IsAutoRepeatSpell(spell [, book]) (0x005415d0): 1 when the
                // spell is the repeating one running (0x007fe130), else nil.
                {"IsAutoRepeatSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t spellId = gh ? spellIdForCall(L, gh) : 0;
            if (spellId != 0 && spellId == gh->autoRepeatSpellId()) {
                lua_pushnumber(L, 1);
                return 1;
            }
            lua_pushnil(L);
            return 1;
        }},
                {"CastShapeshiftForm", [](lua_State* L) -> int {
            // CastShapeshiftForm(index) (0x0053c0f0): a non-zero number or the
            // usage line, rounded; the spell for that form slot.
            const double asked = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : 0.0;
            if (asked == 0.0) return luaL_error(L, "Usage: CastShapeshiftForm(index)");
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(std::lround(asked));
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
            if (gh && !game::isPreWotlk()) gh->leaveShapeshiftForm();
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

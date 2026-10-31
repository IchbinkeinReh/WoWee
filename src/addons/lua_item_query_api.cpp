// lua_item_query_api.cpp - What the interface asks about an item or a spell
// without using it: whether it could be used now, whether it reaches a unit,
// whether it has a reach at all, which side it is aimed at, what it consumes,
// and an item's stat summary. Each as wow.exe 12340 answers it, read from its
// function.
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/interface_text.hpp"
#include "game/item_stat_summary.hpp"
#include "game/spell_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdlib>
#include <optional>

namespace wowee::addons {

namespace {

using Item = game::ItemQueryResponseData;

std::shared_ptr<pipeline::DBCFile> dbc(game::GameHandler* gh, const char* name) {
    auto* am = gh ? gh->services().assetManager : nullptr;
    if (!am || !am->isInitialized()) return nullptr;
    auto file = am->loadDBCOptional(name);
    return file && file->isLoaded() ? file : nullptr;
}

/// The item an argument names (0x0084df20 / 0x00709de0): a number, or a
/// string - an "item:" link's id, else the name of an item in the cache,
/// compared without case.
uint32_t itemArg(lua_State* L, game::GameHandler* gh, int idx) {
    if (lua_isnumber(L, idx)) return static_cast<uint32_t>(lua_tonumber(L, idx));
    if (!lua_isstring(L, idx) || !gh) return 0;
    std::string s = lua_tostring(L, idx);
    if (const auto pos = s.find("item:"); pos != std::string::npos)
        return static_cast<uint32_t>(std::strtoul(s.c_str() + pos + 5, nullptr, 10));
    toLowerInPlace(s);
    for (const auto& [id, info] : gh->getItemInfoCache()) {
        std::string n = info.name;
        toLowerInPlace(n);
        if (n == s) return id;
    }
    return 0;
}

/// The first use spell (trigger 0) of the template: its index, -1 none.
int useSpellIndex(const Item& info) {
    for (int k = 0; k < 5; ++k)
        if (info.spells[static_cast<size_t>(k)].spellId != 0 && info.spells[static_cast<size_t>(k)].spellTrigger == 0)
            return k;
    return -1;
}

/// 0x00510ef0: the spell using the item casts. Its template's use spell;
/// where it has none, an equipped one's on-use enchantment (0x005a86b0 ->
/// 0x00707c60: SpellItemEnchantment Effect 7, its EffectArg).
uint32_t itemUseSpell(game::GameHandler* gh, uint32_t itemId) {
    const auto* info = gh ? gh->getItemInfo(itemId) : nullptr;
    if (info) {
        if (const int k = useSpellIndex(*info); k >= 0) return info->spells[static_cast<size_t>(k)].spellId;
    }
    auto ench = gh ? dbc(gh, "SpellItemEnchantment.dbc") : nullptr;
    if (!ench || ench->getFieldCount() < 14) return 0;
    for (int slot = 0; slot < static_cast<int>(game::EquipSlot::NUM_SLOTS); ++slot) {
        const auto it = gh->onlineItemsRef().find(gh->getEquipSlotGuid(slot));
        if (it == gh->onlineItemsRef().end() || it->second.entry != itemId) continue;
        const auto& o = it->second;
        const uint32_t ids[5] = {o.permanentEnchantId, o.temporaryEnchantId, o.socketEnchantIds[0],
                                 o.socketEnchantIds[1], o.socketEnchantIds[2]};
        for (const uint32_t id : ids) {
            const int32_t row = id != 0 ? ench->findRecordById(id) : -1;
            if (row < 0) continue;
            for (uint32_t k = 0; k < 3; ++k)
                if (ench->getUInt32(static_cast<uint32_t>(row), 2 + k) == 7)
                    return ench->getUInt32(static_cast<uint32_t>(row), 11 + k);
        }
    }
    return 0;
}

bool spellKnownToClient(game::GameHandler* gh, uint32_t spellId) {
    if (!gh || spellId == 0) return false;
    gh->loadSpellNameCache();
    return gh->spellNameCacheRef().count(spellId) != 0;
}

/// The spell the book arguments name (0x00540670): a place in a book, or
/// a name - its highest rank known - or an id.
uint32_t spellArg(lua_State* L, game::GameHandler* gh) {
    if (!gh) return 0;
    if (lua_isstring(L, 1) && !lua_isnumber(L, 1)) return highestKnownRankByName(gh, lua_tostring(L, 1));
    return spellIdForCall(L, gh);
}

/// Who casts a spell of the book an argument names: the pet for "pet".
uint64_t bookCaster(lua_State* L, game::GameHandler* gh) {
    if (lua_isnumber(L, 1) && !lua_isnoneornil(L, 2)) {
        const char* book = lua_tostring(L, 2);
        if (book && std::string(book) == "pet") return gh->getPetGuid();
    }
    return gh->getPlayerGuid();
}

/// IsUsableItem(item) (0x0051c450) → usable, noMana. Carried (0x00754d00)
/// and its use spell not held (0x0080ac10); in an arena (Map.dbc type 4)
/// an item not flagged 0x200000 is not, spent with use or with a cooldown
/// or its category's over fifteen minutes. Then its spell: none in
/// Spell.dbc is usable; else 0x0080b5d0 with the item, and the power
/// (0x008017e0) - short of it, nil and 1.
int isUsableItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t itemId = itemArg(L, gh, 1);
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    const auto player = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    bool usable = false, noMana = false;
    if (sh && player && player->isUnit() && itemId != 0 && gh->getInventory().countCarried(itemId) != 0) {
        const auto* info = gh->getItemInfo(itemId);
        // 0x0080ac10 asks the cache with a callback: a miss is queried.
        if (!info) gh->queryItemInfo(itemId, 0);
        bool held = false;
        if (info) {
            if (const int k = useSpellIndex(*info); k >= 0) {
                const auto& sp = info->spells[static_cast<size_t>(k)];
                uint32_t category = 0;
                gh->getSpellName(sp.spellId);
                if (const auto it = gh->spellNameCacheRef().find(sp.spellId); it != gh->spellNameCacheRef().end())
                    category = it->second.category;
                if (sp.category > 0) category = sp.category;
                held = sh->historyHeld(sp.spellId, itemId, category);
            }
        }
        bool arenaRefused = false;
        if (info && gh->currentMapInstanceType() == 4 && (info->itemFlags & 0x200000u) == 0) {
            if (const int k = useSpellIndex(*info); k >= 0) {
                const auto& sp = info->spells[static_cast<size_t>(k)];
                arenaRefused = sp.charges < 0 || sp.cooldown > 900000 || sp.categoryCooldown > 900000;
            }
        }
        if (!held && !arenaRefused) {
            const uint32_t spell = itemUseSpell(gh, itemId);
            if (!spellKnownToClient(gh, spell)) {
                usable = true;
            } else if (sh->spellUsable(spell, info)) {
                usable = sh->enoughPower(spell, static_cast<const game::Unit&>(*player));
                noMana = !usable;
            }
        }
    }
    if (usable) lua_pushnumber(L, 1); else lua_pushnil(L);
    if (noMana) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 2;
}

/// IsConsumableItem(item) (0x005173e0): ammunition or a thrown weapon
/// (InventoryType 24, 25), or a use spell with reagents or spent with
/// the item (charges under zero).
int isConsumableItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t itemId = itemArg(L, gh, 1);
    const auto* info = gh && itemId ? gh->getItemInfo(itemId) : nullptr;
    bool yes = false;
    if (info) {
        yes = info->inventoryType == 24 || info->inventoryType == 25;
        for (const auto& sp : info->spells) {
            if (yes) break;
            if (sp.spellId == 0 || sp.spellTrigger != 0 || !spellKnownToClient(gh, sp.spellId)) continue;
            const auto& e = gh->spellNameCacheRef().at(sp.spellId);
            for (const auto& r : e.reagents)
                if (r.itemId != 0 && static_cast<int32_t>(r.count) > 0) yes = true;
            if (sp.charges < 0) yes = true;
        }
    }
    if (yes) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// IsCurrentItem(item) (0x005110d0 -> 0x00805fc0): a spell under way cast
/// from an item of it.
int isCurrentItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t itemId = itemArg(L, gh, 1);
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    bool yes = false;
    if (sh && itemId != 0 && sh->isCasting() && sh->currentCastItemGuid() != 0) {
        const auto it = gh->onlineItemsRef().find(sh->currentCastItemGuid());
        yes = it != gh->onlineItemsRef().end() && it->second.entry == itemId;
    }
    if (yes) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// The 29 inventory types by their tokens (0x00ac7fd8).
constexpr const char* kInvTypes[29] = {
    "", "INVTYPE_HEAD", "INVTYPE_NECK", "INVTYPE_SHOULDER", "INVTYPE_BODY", "INVTYPE_CHEST", "INVTYPE_WAIST",
    "INVTYPE_LEGS", "INVTYPE_FEET", "INVTYPE_WRIST", "INVTYPE_HAND", "INVTYPE_FINGER", "INVTYPE_TRINKET",
    "INVTYPE_WEAPON", "INVTYPE_SHIELD", "INVTYPE_RANGED", "INVTYPE_CLOAK", "INVTYPE_2HWEAPON", "INVTYPE_BAG",
    "INVTYPE_TABARD", "INVTYPE_ROBE", "INVTYPE_WEAPONMAINHAND", "INVTYPE_WEAPONOFFHAND", "INVTYPE_HOLDABLE",
    "INVTYPE_AMMO", "INVTYPE_THROWN", "INVTYPE_RANGEDRIGHT", "INVTYPE_QUIVER", "INVTYPE_RELIC"};

bool sameText(std::string a, std::string b) {
    toLowerInPlace(a);
    toLowerInPlace(b);
    return a == b;
}

/// IsEquippedItemType(type) (0x0051c740): an inventory type's token or
/// its text names the inventory type; anything else is held against the
/// item's class name (ItemClass.dbc), its subclass's two names
/// (ItemSubClass.dbc) and the subclass masks' (ItemSubClassMask.dbc).
/// Asked of the equipment (0x007546f0 with 1), not a broken piece.
int isEquippedItemType(lua_State* L) {
    const char* arg = lua_tostring(L, 1);
    if (!arg || !*arg) return luaL_error(L, "Usage: IsEquippedItemType(\"type\")");
    auto* gh = getGameHandler(L);
    const auto player = gh ? gh->getEntityManager().getEntity(gh->getPlayerGuid()) : nullptr;
    if (!player) return luaReturnNil(L);
    const std::string type = arg;
    int invType = -1;
    for (int i = 0; i < 29; ++i) {
        if (sameText(type, kInvTypes[i]) || (i != 0 && sameText(type, game::interfaceString(kInvTypes[i], "")))) {
            invType = i;
            break;
        }
    }
    auto classes = dbc(gh, "ItemClass.dbc");
    auto subs = dbc(gh, "ItemSubClass.dbc");
    auto masks = dbc(gh, "ItemSubClassMask.dbc");
    const auto matches = [&](const Item& info) {
        if (invType >= 1) return static_cast<int>(info.inventoryType) == invType;
        if (classes && classes->getFieldCount() > 3) {
            const int32_t row = classes->findRecordById(info.itemClass);
            if (row >= 0 && sameText(classes->getString(static_cast<uint32_t>(row), 3), type)) return true;
        }
        if (subs && subs->getFieldCount() > 27) {
            for (uint32_t r = 0; r < subs->getRecordCount(); ++r) {
                if (subs->getUInt32(r, 0) != info.itemClass || subs->getUInt32(r, 1) != info.subClass) continue;
                if (sameText(subs->getString(r, 27), type) || sameText(subs->getString(r, 10), type)) return true;
                break;
            }
        }
        if (masks && masks->getFieldCount() > 2) {
            for (uint32_t r = masks->getRecordCount(); r-- > 0;) {
                if (masks->getUInt32(r, 0) == info.itemClass &&
                    (masks->getUInt32(r, 1) & (1u << (info.subClass & 31))) != 0 &&
                    sameText(masks->getString(r, 2), type))
                    return true;
            }
        }
        return false;
    };
    bool yes = false;
    for (int slot = 0; slot <= static_cast<int>(game::EquipSlot::TABARD) && !yes; ++slot) {
        const auto it = gh->onlineItemsRef().find(gh->getEquipSlotGuid(slot));
        if (it == gh->onlineItemsRef().end()) continue;
        if (it->second.maxDurability != 0 && it->second.curDurability == 0) continue;
        if (const auto* info = gh->getItemInfo(it->second.entry)) yes = matches(*info);
    }
    if (yes) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// 0x00801650 with no unit: a reach at all - a minimum or a maximum.
bool hasReach(game::GameHandler* gh, uint32_t spellId, uint64_t caster) {
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    if (!sh || !spellKnownToClient(gh, spellId)) return false;
    const auto r = sh->spellRange(spellId, caster);
    return r && (std::fabs(r->min) >= 2.3841858e-07f || std::fabs(r->max) >= 2.3841858e-07f);
}

/// ItemHasRange(item) (0x0051c870): the use spell has a reach.
int itemHasRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t itemId = itemArg(L, gh, 1);
    if (gh && hasReach(gh, itemUseSpell(gh, itemId), gh->getPlayerGuid())) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// The answer of 0x00809610 as the range functions give it: nil where
/// the unit is none the spell may be aimed at, else 1 or 0.
int pushReach(lua_State* L, game::GameHandler* gh, uint32_t spellId, uint64_t caster, uint64_t unit) {
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    const auto r = sh && spellKnownToClient(gh, spellId) ? sh->reachAnswer(spellId, caster, unit) : std::nullopt;
    if (!r) return luaReturnNil(L);
    lua_pushnumber(L, *r ? 1 : 0);
    return 1;
}

/// The unit an argument names (0x0060abf0); none where it is not a string.
std::optional<uint64_t> unitArg(lua_State* L, game::GameHandler* gh, int idx) {
    const char* s = lua_tostring(L, idx);
    if (!s) return std::nullopt;
    std::string u(s);
    toLowerInPlace(u);
    return resolveUnitGuid(gh, u);
}

/// IsItemInRange(item, unit) (0x0051c9c0): the use spell's reach to the
/// unit.
int isItemInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t itemId = itemArg(L, gh, 1);
    const auto unit = gh ? unitArg(L, gh, 2) : std::nullopt;
    if (!unit) return luaReturnNil(L);
    return pushReach(L, gh, itemUseSpell(gh, itemId), gh->getPlayerGuid(), *unit);
}

/// IsSpellInRange(index, "bookType", unit) or (name, unit) (0x00541c60):
/// the reach from the book's caster to the unit.
int isSpellInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    const uint32_t spellId = spellArg(L, gh);
    const int unitIdx = lua_isnumber(L, 1) && !lua_isnoneornil(L, 2) ? 3 : 2;
    const auto unit = unitArg(L, gh, unitIdx);
    if (spellId == 0 || !unit) return luaReturnNil(L);
    return pushReach(L, gh, spellId, bookCaster(L, gh), *unit);
}

/// SpellHasRange(index, "bookType") or (name) (0x00541af0).
int spellHasRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spellId = spellArg(L, gh);
    if (spellId != 0 && hasReach(gh, spellId, bookCaster(L, gh))) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsHarmfulItem / IsHelpfulItem (0x005172e0, 0x005171f0): the use
/// spell aimed at enemies (0x007fe1b0 answering 2) or at friends (1).
template <uint32_t Kind>
int itemAimedAt(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = gh ? itemUseSpell(gh, itemArg(L, gh, 1)) : 0;
    if (spellKnownToClient(gh, spell) && gh->getSpellTargetKind(spell) == Kind) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsHarmfulSpell / IsHelpfulSpell (0x005418f0, 0x00541800).
template <uint32_t Kind>
int spellAimedAt(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = spellArg(L, gh);
    if (spellKnownToClient(gh, spell) && gh->getSpellTargetKind(spell) == Kind) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsAttackSpell (0x00541420): its first effect is ATTACK (78).
int isAttackSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = spellArg(L, gh);
    if (spellKnownToClient(gh, spell) && gh->spellNameCacheRef().at(spell).effectIds[0] == 78) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsConsumableSpell (0x005419f0): it has a reagent.
int isConsumableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = spellArg(L, gh);
    bool yes = false;
    if (spellKnownToClient(gh, spell)) {
        for (const auto& r : gh->spellNameCacheRef().at(spell).reagents)
            if (r.itemId != 0 && static_cast<int32_t>(r.count) > 0) yes = true;
    }
    if (yes) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

/// GetSpellCount (0x00540df0 -> 0x007ffa30): how many casts the reagents
/// carried pay for, the fewest of any; 0 with none.
int getSpellCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = spellArg(L, gh);
    if (spell == 0) return 0;
    int32_t casts = 0x7fffffff;
    if (spellKnownToClient(gh, spell) && gh->getEntityManager().getEntity(gh->getPlayerGuid())) {
        for (const auto& r : gh->spellNameCacheRef().at(spell).reagents) {
            const auto need = static_cast<int32_t>(r.count);
            if (r.itemId == 0 || r.itemId == 0xFFFFFFFEu || need <= 0) continue;
            casts = std::min(casts, static_cast<int32_t>(gh->getInventory().countCarried(r.itemId)) / need);
        }
    }
    lua_pushnumber(L, casts == 0x7fffffff ? 0 : casts);
    return 1;
}

/// IsUsableSpell(index, "bookType") or (name) (0x00541680) → usable,
/// noMana: 0x0080b5d0 for the book's caster, then the power.
int isUsableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t spell = spellArg(L, gh);
    auto* sh = gh ? gh->getSpellHandler() : nullptr;
    bool usable = false, noMana = false;
    if (sh && spellKnownToClient(gh, spell)) {
        const uint64_t casterGuid = bookCaster(L, gh);
        const auto caster = gh->getEntityManager().getEntity(casterGuid);
        if (caster && caster->isUnit()) {
            // The pet's own reading of 0x0080b5d0 is not modelled; its power
            // still is.
            const bool lets = casterGuid != gh->getPlayerGuid() || sh->spellUsable(spell, nullptr);
            if (lets) {
                usable = sh->enoughPower(spell, static_cast<const game::Unit&>(*caster));
                noMana = !usable;
            }
        }
    }
    if (usable) lua_pushnumber(L, 1); else lua_pushnil(L);
    if (noMana) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 2;
}

/// FindSpellBookSlotByID(spellID[, isPet]) (0x0053b6c0 -> 0x0053b4e0):
/// the spell's place in the book, the last one holding it; nil for none.
int findSpellBookSlotByID(lua_State* L) {
    if (!lua_isnumber(L, 1) || lua_tonumber(L, 1) <= 0)
        return luaL_error(L, "Usage: FindSpellBookSlotByID(spellID[, isPet])");
    auto* gh = getGameHandler(L);
    const auto id = static_cast<uint32_t>(lua_tonumber(L, 1));
    int found = -1;
    if (gh && lua_toboolean(L, 2)) {
        const auto& pet = gh->getPetSpells();
        for (size_t i = pet.size(); i-- > 0;)
            if (pet[i] == id) { found = static_cast<int>(i) + 1; break; }
    } else if (gh) {
        int slot = 0;
        for (const auto& tab : gh->getSpellBookTabs()) {
            for (const uint32_t s : tab.spellIds) {
                ++slot;
                if (s == id) found = slot;
            }
        }
    }
    if (found < 0) return luaReturnNil(L);
    lua_pushnumber(L, found);
    return 1;
}

// ---- The stat summary (Tooltip.cpp) ----

/// 0x0061b5e0's reading of a spell, by id; nothing for one not in Spell.dbc.
std::optional<game::item_stats::AuraSpell> auraSpell(game::GameHandler* gh, uint32_t spellId) {
    if (!spellKnownToClient(gh, spellId)) return std::nullopt;
    const auto& e = gh->spellNameCacheRef().at(spellId);
    game::item_stats::AuraSpell a;
    a.attr = e.attr;
    for (int k = 0; k < 3; ++k) {
        a.effect[k] = e.effectIds[k];
        a.aura[k] = e.effectAuraIds[k];
        a.basePoints[k] = e.effectBasePoints[k];
        a.misc[k] = static_cast<int32_t>(e.effectMiscValues[k]);
    }
    return a;
}

/// 0x0061f2b0: one enchantment effect of `type` - an equip spell (3), a
/// resistance (4), a stat (5).
void addEnchantEffect(game::GameHandler* gh, game::item_stats::Summary& s, uint32_t type, uint32_t arg,
                      int32_t amount) {
    if (type == 3) {
        if (const auto a = auraSpell(gh, arg)) game::item_stats::addPassiveAuras(s, *a);
    } else if (type == 4) {
        s.add(static_cast<int>(arg), amount);
    } else if (type == 5) {
        game::item_stats::addMod(s, static_cast<int32_t>(arg), amount);
    }
}

/// 0x006207a0: the summary of an "item:" link - its id, suffix (field 7),
/// unique id (field 8, the suffix factor its low half) and level (field
/// 9). Nothing where the link names no item in the cache.
std::optional<game::item_stats::Summary> linkSummary(game::GameHandler* gh, const std::string& link) {
    namespace is = game::item_stats;
    const auto pos = link.find("item:");
    if (!gh || pos == std::string::npos) return std::nullopt;
    int32_t fields[9] = {};
    const char* p = link.c_str() + pos + 5;
    for (int f = 0; f < 9; ++f) {
        fields[f] = static_cast<int32_t>(std::strtol(p, nullptr, 10));
        const char* colon = std::strchr(p, ':');
        if (!colon) break;
        p = colon + 1;
    }
    const int32_t id = fields[0];
    if (id < 1) return std::nullopt;
    const auto* item = gh->getItemInfo(static_cast<uint32_t>(id));
    if (!item) return std::nullopt;
    const int32_t suffix = fields[6];
    const auto factor = static_cast<uint32_t>(fields[7]) & 0xFFFFu;
    const int32_t linkLevel = fields[8];
    is::Summary s;
    const auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    const bool druid = player && gh->getPlayerClass() == 11;
    const auto itemSpells = [&] {  // 0x0061f350
        for (const auto& sp : item->spells)
            if (static_cast<int32_t>(sp.spellId) > 0)
                if (const auto a = auraSpell(gh, sp.spellId)) is::addPassiveAuras(s, *a);
    };
    const auto statPairs = [&] {  // 0x0061b990
        for (const auto& st : item->statPairs) is::addMod(s, static_cast<int32_t>(st.statType), st.statValue);
    };
    const auto armour = [&] {  // 0x0061b8e0
        const int32_t r[7] = {item->armor, item->holyRes, item->fireRes, item->natureRes,
                              item->frostRes, item->shadowRes, item->arcaneRes};
        for (int i = 0; i < 7; ++i) s.add(i, r[i]);
    };
    if (item->scalingStatValue == 0) {
        // 0x0061f4c0.
        statPairs();
        itemSpells();
        armour();
        for (const uint32_t c : item->socketColor) {  // 0x0061b930
            for (int b = 0; b < 4; ++b)
                if ((c & (1u << b)) != 0) s.add(is::kFirstSocket + b, 1);
        }
        s.add(is::kBlockValue, item->block);
        // 0x0061f410: each damage entry's mean over the swing.
        const float d0 = (item->damages[0].max + item->damages[0].min) * 0.5f;
        const float d1 = (item->damages[1].max + item->damages[1].min) * 0.5f;
        const float swing = static_cast<float>(item->delayMs) * 0.001f;
        s.dps = d0 / swing + s.dps;
        s.dps = d1 / swing + s.dps;
        if (druid)
            s.v[is::kFeralAttackPower] += is::feralAttackPower(item->itemClass, item->inventoryType, item->delayMs,
                                                               d0 + d1) + s.v[is::kMeleeAttackPower];
        // 0x0061fa20: the random properties (positive) or suffix
        // (negative) the link names.
        if (suffix > 0) {
            auto props = dbc(gh, "ItemRandomProperties.dbc");
            auto ench = dbc(gh, "SpellItemEnchantment.dbc");
            const int32_t row = props && props->getFieldCount() > 6 ? props->findRecordById(static_cast<uint32_t>(suffix)) : -1;
            for (uint32_t k = 0; row >= 0 && ench && k < 5; ++k) {
                const int32_t e = ench->findRecordById(props->getUInt32(static_cast<uint32_t>(row), 2 + k));
                for (uint32_t j = 0; e >= 0 && j < 3; ++j) {
                    const auto r = static_cast<uint32_t>(e);
                    addEnchantEffect(gh, s, ench->getUInt32(r, 2 + j), ench->getUInt32(r, 11 + j),
                                     (ench->getInt32(r, 5 + j) + ench->getInt32(r, 8 + j)) / 2);
                }
            }
        } else if (suffix < 0) {
            auto sfx = dbc(gh, "ItemRandomSuffix.dbc");
            auto ench = dbc(gh, "SpellItemEnchantment.dbc");
            const int32_t row = sfx && sfx->getFieldCount() > 28 ? sfx->findRecordById(static_cast<uint32_t>(-suffix)) : -1;
            for (uint32_t k = 0; row >= 0 && ench && k < 5; ++k) {
                const int32_t amount = is::suffixAmount(sfx->getInt32(static_cast<uint32_t>(row), 24 + k), factor);
                const int32_t e = ench->findRecordById(sfx->getUInt32(static_cast<uint32_t>(row), 19 + k));
                for (uint32_t j = 0; e >= 0 && j < 3; ++j) {
                    const auto r = static_cast<uint32_t>(e);
                    addEnchantEffect(gh, s, ench->getUInt32(r, 2 + j), ench->getUInt32(r, 11 + j), amount);
                }
            }
        }
    } else {
        // 0x0061f550: an heirloom, at the link's level or the player's
        // (0x0061e740) - at least the item's required level, at most its
        // ScalingStatDistribution's, and at least 1.
        auto dist = dbc(gh, "ScalingStatDistribution.dbc");
        auto values = dbc(gh, "ScalingStatValues.dbc");
        const int32_t distRow = dist && dist->getFieldCount() > 21 ? dist->findRecordById(item->scalingStatDistribution) : -1;
        int32_t level = linkLevel;
        if (level == 0) {
            level = static_cast<int32_t>(gh->getPlayerLevel());
            if (distRow >= 0) {
                const auto req = static_cast<int32_t>(item->requiredLevel);
                const int32_t atLeast = std::max(req, level);
                const auto maxLevel = dist->getInt32(static_cast<uint32_t>(distRow), 21);
                level = atLeast < maxLevel ? std::max(level, req) : maxLevel;
            }
            level = std::max(level, 1);
        }
        const int32_t valueRow = values && values->getFieldCount() > 23 && level >= 1 &&
                                         static_cast<uint32_t>(level) <= values->getRecordCount()
                                     ? level - 1 : -1;
        if (distRow >= 0 && valueRow >= 0) {
            const auto vr = static_cast<uint32_t>(valueRow);
            const auto value = [&](uint32_t field) { return values->getInt32(vr, field); };
            const uint32_t ssv = item->scalingStatValue;
            itemSpells();
            // 0x0061a650: the stat budget for the mask.
            int32_t budget = 0;
            switch (ssv & 0x4001fu) {
                case 0x1: budget = value(2); break;
                case 0x2: budget = value(3); break;
                case 0x4: budget = value(4); break;
                case 0x8: budget = value(17); break;
                case 0x10: budget = value(5); break;
                case 0x40000: budget = value(18); break;
                default: break;
            }
            for (uint32_t k = 0; k < 10; ++k) {
                const int32_t stat = dist->getInt32(static_cast<uint32_t>(distRow), 1 + k);
                if (stat > 0) is::addMod(s, stat, dist->getInt32(static_cast<uint32_t>(distRow), 11 + k) * budget / 10000);
            }
            // 0x0061a6b0: the armour for the mask, else the template's.
            int32_t armourValue = 0;
            bool scaledArmour = true;
            switch (ssv & 0xf801e0u) {
                case 0x20: armourValue = value(6); break;
                case 0x40: armourValue = value(7); break;
                case 0x80: armourValue = value(8); break;
                case 0x100: armourValue = value(9); break;
                case 0x80000: armourValue = value(19); break;
                case 0x100000: armourValue = value(20); break;
                case 0x200000: armourValue = value(21); break;
                case 0x400000: armourValue = value(22); break;
                case 0x800000: armourValue = value(23); break;
                default: scaledArmour = (ssv & 0xf801e0u) != 0; break;
            }
            if (scaledArmour) s.add(0, armourValue); else armour();
            // 0x0061a830: the damage per second for the mask, plus or less
            // its spread; else the template's.
            if ((ssv & 0x7e00u) == 0) {
                const float d0 = (item->damages[0].max + item->damages[0].min) * 0.5f;
                const float d1 = (item->damages[1].max + item->damages[1].min) * 0.5f;
                const float swing = static_cast<float>(item->delayMs) * 0.001f;
                s.dps = d0 / swing + s.dps;
                s.dps = d1 / swing + s.dps;
                if (druid)
                    s.v[is::kFeralAttackPower] += is::feralAttackPower(item->itemClass, item->inventoryType,
                                                                       item->delayMs, d0 + d1) +
                                                  s.v[is::kMeleeAttackPower];
            } else {
                float base = 0.0f, spread = 0.3f;
                switch (ssv & 0x7e00u) {
                    case 0x200: base = static_cast<float>(value(10)); break;
                    case 0x400: base = static_cast<float>(value(11)); spread = 0.2f; break;
                    case 0x800: base = static_cast<float>(value(12)); break;
                    case 0x1000: base = static_cast<float>(value(13)); spread = 0.2f; break;
                    case 0x2000: base = static_cast<float>(value(14)); break;
                    case 0x4000: base = static_cast<float>(value(15)); break;
                    default: break;
                }
                const float lo = base - base * spread, hi = (spread + 1.0f) * base;
                s.dps = (lo + hi) * 0.5f;
                const float perSwing = (lo + hi) * static_cast<float>(item->delayMs) * 0.0005f;
                if (druid)
                    s.v[is::kFeralAttackPower] += is::feralAttackPower(item->itemClass, item->inventoryType,
                                                                       item->delayMs, perSwing) +
                                                  s.v[is::kMeleeAttackPower];
            }
            if ((ssv & 0x8000u) != 0) s.add(56, value(16));
        }
    }
    is::merge(s);
    return s;
}

/// 0x006085d0: the summary into the table on the top of the stack - the
/// damage per second where it is not zero, and each slot but the two
/// attack powers ITEM_MOD folds in (49, 50) that is not.
int pushSummary(lua_State* L, const game::item_stats::Summary& s) {
    if (s.dps > 1.1920929e-07f || s.dps < -1.1920929e-07f) {
        lua_pushstring(L, "ITEM_MOD_DAMAGE_PER_SECOND_SHORT");
        lua_pushnumber(L, s.dps);
        lua_settable(L, -3);
    }
    for (int i = 0; i < game::item_stats::kSlots; ++i) {
        if (i == 0x31 || i == 0x32 || s.v[static_cast<size_t>(i)] == 0) continue;
        lua_pushstring(L, game::item_stats::key(i).c_str());
        lua_pushnumber(L, s.v[static_cast<size_t>(i)]);
        lua_settable(L, -3);
    }
    return 1;
}

/// GetItemStats(itemLink[, statTable]) (0x00608690).
int getItemStats(lua_State* L) {
    if (!lua_isstring(L, 1) || (lua_type(L, 2) > 0 && !lua_istable(L, 2)))
        return luaL_error(L, "Usage: GetItemStats(itemLink[, statTable])");
    const auto s = linkSummary(getGameHandler(L), lua_tostring(L, 1));
    if (!s) return 0;
    if (lua_istable(L, 2)) lua_pushvalue(L, 2); else lua_newtable(L);
    return pushSummary(L, *s);
}

/// GetItemStatDelta(itemLink1, itemLink2[, statTable]) (0x00608760): the
/// first less the second (0x0061dbc0).
int getItemStatDelta(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2) || (lua_type(L, 3) > 0 && !lua_istable(L, 3)))
        return luaL_error(L, "Usage: GetItemStatSummary(itemLink1, itemLink2[, statTable])");
    auto* gh = getGameHandler(L);
    const auto a = linkSummary(gh, lua_tostring(L, 1));
    if (!a) return 0;
    const auto b = linkSummary(gh, lua_tostring(L, 2));
    if (!b) return 0;
    if (lua_istable(L, 3)) lua_pushvalue(L, 3); else lua_newtable(L);
    return pushSummary(L, game::item_stats::delta(*a, *b));
}

/// GetItemGem(item, index) (0x0051c150) → name, link: the gem in socket
/// 1-3 - from a link's gem fields, else an item carried by that name's
/// socket enchantments - by its enchantment's gem (SpellItemEnchantment
/// field 33), when that is in the cache.
int getItemGem(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isnumber(L, 2))
        return luaL_error(L, "Usage: GetItemGem(\"name\"|\"itemlink\", index)");
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(lua_tonumber(L, 2));
    if (!gh || index < 1 || index > 3) return 0;
    const std::string arg = lua_tostring(L, 1);
    uint32_t enchant = 0;
    if (const auto pos = arg.find("item:"); pos != std::string::npos) {
        const char* p = arg.c_str() + pos + 5;
        for (int f = 0; f <= index + 1 && p; ++f) {
            if (f == index + 1) enchant = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
            const char* colon = std::strchr(p, ':');
            p = colon ? colon + 1 : nullptr;
        }
    } else {
        std::string want = arg;
        toLowerInPlace(want);
        uint64_t found = 0;
        gh->getInventory().forEachCarried([&](const game::ItemDef& d) {
            if (found != 0) return;
            const auto* info = gh->getItemInfo(d.itemId);
            std::string n = info ? info->name : d.name;
            toLowerInPlace(n);
            if (n == want) found = d.guid;
        });
        const auto it = found != 0 ? gh->onlineItemsRef().find(found) : gh->onlineItemsRef().end();
        if (it == gh->onlineItemsRef().end()) return 0;
        enchant = it->second.socketEnchantIds[static_cast<size_t>(index - 1)];
    }
    auto ench = dbc(gh, "SpellItemEnchantment.dbc");
    const int32_t row = ench && ench->getFieldCount() > 33 && enchant != 0 ? ench->findRecordById(enchant) : -1;
    const uint32_t gem = row >= 0 ? ench->getUInt32(static_cast<uint32_t>(row), 33) : 0u;
    const auto* info = gem != 0 ? gh->getItemInfo(gem) : nullptr;
    if (!info) return 0;
    lua_pushstring(L, info->name.c_str());
    lua_pushstring(L, game::itemChatLink(gem, info->quality, info->name, {}, gh->getPlayerLevel()).c_str());
    return 2;
}

/// GetItemUniqueness(item) (0x00516ed0) → category, count: flagged
/// unique-equipped (0x80000), -1 and 1; else its ItemLimitCategory and
/// that row's quantity; nothing for neither.
int getItemUniqueness(lua_State* L) {
    if (!lua_isnumber(L, 1) && !lua_isstring(L, 1))
        return luaL_error(L, "Usage: GetItemUniqueness(itemID|\"name\"|\"itemlink\")");
    auto* gh = getGameHandler(L);
    const auto* info = gh ? gh->getItemInfo(itemArg(L, gh, 1)) : nullptr;
    if (!info) return 0;
    if ((info->itemFlags & 0x80000u) != 0) {
        lua_pushnumber(L, -1);
        lua_pushnumber(L, 1);
        return 2;
    }
    auto limits = dbc(gh, "ItemLimitCategory.dbc");
    const int32_t row = limits && info->itemLimitCategory != 0 && limits->getFieldCount() > 18
                            ? limits->findRecordById(info->itemLimitCategory) : -1;
    if (row < 0) return 0;
    lua_pushnumber(L, info->itemLimitCategory);
    lua_pushnumber(L, limits->getInt32(static_cast<uint32_t>(row), 18));
    return 2;
}

}  // namespace

void registerItemQueryLuaAPI(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"IsUsableItem",            isUsableItem},
        {"IsConsumableItem",        isConsumableItem},
        {"IsCurrentItem",           isCurrentItem},
        {"IsEquippedItemType",      isEquippedItemType},
        {"ItemHasRange",            itemHasRange},
        {"IsItemInRange",           isItemInRange},
        {"IsSpellInRange",          isSpellInRange},
        {"SpellHasRange",           spellHasRange},
        {"IsHarmfulItem",           itemAimedAt<2>},
        {"IsHelpfulItem",           itemAimedAt<1>},
        {"IsHarmfulSpell",          spellAimedAt<2>},
        {"IsHelpfulSpell",          spellAimedAt<1>},
        {"IsAttackSpell",           isAttackSpell},
        {"IsConsumableSpell",       isConsumableSpell},
        {"GetSpellCount",           getSpellCount},
        {"IsUsableSpell",           isUsableSpell},
        {"FindSpellBookSlotByID",   findSpellBookSlotByID},
        {"GetItemStats",            getItemStats},
        {"GetItemStatDelta",        getItemStatDelta},
        {"GetItemGem",              getItemGem},
        {"GetItemUniqueness",       getItemUniqueness},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* fn = fns; fn->name; ++fn) {
        lua_pushcfunction(L, fn->func);
        lua_setglobal(L, fn->name);
    }
}

}  // namespace wowee::addons

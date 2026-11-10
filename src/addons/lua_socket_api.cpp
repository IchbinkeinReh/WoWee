// lua_socket_api.cpp - putting gems into an item's sockets.
//
// This client has never had a socketing window. It parses socket colours and
// draws them on a tooltip, it has a handler for the server's reply, and there
// was nothing in between: no way to open an item, no way to place a gem, and
// nothing anywhere that sent CMSG_SOCKET_GEMS. FrameXML ships the whole panel -
// Blizzard_ItemSocketingUI, plus the right-click entries in the paperdoll and
// the bags that open it - and wanted six calls this client did not answer.
// tools/framexml_nil_arithmetic.py had been reporting them as the one live
// carrier left: ItemSocketingFrame_Update, reached from UIParent_OnEvent.
//
// What is real here and what is not:
//
//   * The sockets are real, read from the item template this client already
//     parses. So is the gem in a socket: the item's enchantment fields carry an
//     enchantment id, and SpellItemEnchantment names the gem it came out of.
//   * Whether a gem *matches* its socket is real, out of GemProperties.dbc -
//     the colour mask of the gem against the colour mask of the socket.
//   * The refund and bound-tradeable windows are what this client knows of
//     them: a refund record the server answered CMSG_ITEM_REFUND_INFO with,
//     and ITEM_FIELD_FLAGS 0x100 for an item bound but still tradeable.
#include "addons/lua_api_helpers.hpp"
#include "game/inventory_slots.hpp"
#include "game/game_utils.hpp"
#include "game/interface_text.hpp"
#include "addons/lua_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/gem_conditions.hpp"

#include <array>
#include <cstring>
#include <string>

namespace wowee::addons {

namespace {

/// The four socket colours FrameXML knows, keyed by the mask an item carries.
/// GEM_TYPE_INFO in blizzard_itemsocketingui.lua is indexed by exactly these
/// strings, and an unknown one indexes nothing and takes the panel down.
const char* socketColorName(uint32_t mask) {
    if (mask & 0x1) return "Meta";
    if (mask & 0x2) return "Red";
    if (mask & 0x4) return "Yellow";
    if (mask & 0x8) return "Blue";
    return "";
}

/// A gem's own colour mask, from GemProperties.dbc.
///
/// An item's socket has a colour; a gem does not. What a gem has is a
/// GemProperties id - the field straight after socketBonus in the item query,
/// which this client kept nothing of until the socket block was fixed - and
/// that row's Type is the mask. The compound colours matter: an orange gem is
/// 6, red|yellow, and fits either a red or a yellow socket.
uint32_t gemColorMask(game::GameHandler* gh, uint32_t gemItemId) {
    if (!gh || gemItemId == 0) return 0;
    const auto* info = gh->getItemInfo(gemItemId);
    if (!info || info->gemProperties == 0) return 0;

    auto* am = gh->services().assetManager;
    if (!am || !am->isInitialized()) return 0;
    auto dbc = am->loadDBC("GemProperties.dbc");
    if (!dbc || !dbc->isLoaded()) return 0;
    const int32_t row = dbc->findRecordById(info->gemProperties);
    if (row < 0) return 0;
    // ID, Enchant_Id, MaxCountInv, MaxCountItem, Type - five columns in every
    // shape that has gems at all, so the last one is the colour.
    if (dbc->getFieldCount() < 5) return 0;
    return dbc->getUInt32(static_cast<uint32_t>(row), 4);
}

/// The template of the item whose sockets are on screen.
const game::ItemQueryResponseData* socketItemInfo(game::GameHandler* gh) {
    if (!gh || !gh->isSocketingOpen()) return nullptr;
    const uint32_t itemId = gh->getSocketItemId();
    if (itemId == 0) return nullptr;
    const auto* info = gh->getItemInfo(itemId);
    return (info && info->valid) ? info : nullptr;
}

/// The colour mask of socket `index`, or zero when there is no such socket.
uint32_t socketMaskAt(game::GameHandler* gh, int index) {
    const auto* info = socketItemInfo(gh);
    if (!info || index < 0 || index > 2) return 0;
    return info->socketColor[index];
}

/// name, icon, gemMatchesSocket for a gem item in a given socket - the three
/// returns both socket-info calls share. Pushes nothing and returns 0 when
/// there is no gem, which is how FrameXML asks "is this socket filled".
int pushGemInfo(lua_State* L, game::GameHandler* gh, uint32_t gemItemId, int socketIndex) {
    // Three nils rather than nothing when the socket is empty. Lua reads the
    // two the same way and every caller here guards on the first, but a
    // binding that answers fewer values than the interface unpacks is a shape
    // tools/framexml_short_returns.py counts, and it is right to: the ones that
    // are deliberate are indistinguishable from the ones that are a bug.
    auto pushEmpty = [L]() { lua_pushnil(L); lua_pushnil(L); lua_pushnil(L); return 3; };
    if (gemItemId == 0) return pushEmpty();
    const auto* gem = gh->getItemInfo(gemItemId);
    if (!gem || !gem->valid) {
        // Not queried yet. Ask, and answer nothing this frame - the answer
        // raises SOCKET_INFO_UPDATE (0x005c44d0) and the panel redraws with
        // the name filled in.
        askItemForEvent(gh, gemItemId, "SOCKET_INFO_UPDATE");
        return pushEmpty();
    }
    lua_pushstring(L, gem->name.c_str());
    lua_pushstring(L, gh->getItemIconPath(gem->displayInfoId).c_str());
    const uint32_t socketMask = socketMaskAt(gh, socketIndex);
    const uint32_t gemMask = gemColorMask(gh, gemItemId);
    // A meta socket takes only a meta gem, and a meta gem fits nothing else.
    // Every other colour is a mask test: an orange gem is red|yellow and fits
    // either. Both sides of that are already masks, so one AND says it.
    lua_pushboolean(L, (socketMask & gemMask) != 0 ? 1 : 0);
    return 3;
}

/// An item link for the gem in a socket, or nothing when the socket is empty.
///
/// The panel's own tooltips work off the socket index, but a link is what a
/// shift-click puts in the chat box and what an addon hands around. Reachable
/// at all only since chat began drawing links.
int pushGemLink(lua_State* L, game::GameHandler* gh, uint32_t gemItemId) {
    if (!gh || gemItemId == 0) return 0;
    const auto* gem = gh->getItemInfo(gemItemId);
    if (!gem || !gem->valid) {
        askItemForEvent(gh, gemItemId, "SOCKET_INFO_UPDATE");
        return 0;
    }
    lua_pushstring(L, game::buildItemLink(gemItemId, gem->quality, gem->name, gh->getPlayerLevel()).c_str());
    return 1;
}

/// GetExistingSocketLink(index) - the gem already in the socket.
int lua_GetExistingSocketLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    if (!gh || !gh->isSocketingOpen() || index < 0 || index > 2) return 0;
    const auto enchants = gh->getItemSocketEnchantIds(gh->getSocketItemGuid());
    return pushGemLink(L, gh, gh->getEnchantGemItem(enchants[static_cast<size_t>(index)]));
}

/// GetNewSocketLink(index) - the gem waiting to go in.
int lua_GetNewSocketLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    if (!gh || !gh->isSocketingOpen() || index < 0 || index > 2) return 0;
    return pushGemLink(L, gh, gh->getSocketPendingGemItemId(index));
}

/// SocketInventoryItem(invSlot) - the paperdoll's right-click entry.
int lua_SocketInventoryItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slotId = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slotId < 1 || slotId > 19) return 0;
    gh->openSocketing(gh->getEquipSlotGuid(slotId - 1));
    return 0;
}

/// SocketContainerItem(bag, slot) - the same entry on a bag item.
int lua_SocketContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int bag = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh) return 0;
    gh->openSocketing(containerSlotGuid(gh, bag, slot));
    return 0;
}

int lua_GetNumSockets(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* info = socketItemInfo(gh);
    int count = 0;
    if (info) {
        // Count sockets, not filled ones, and stop at the first empty colour:
        // the template lists them in order and a gap would mean a socket that
        // is not there.
        for (int i = 0; i < 3 && info->socketColor[i] != 0; ++i) ++count;
    }
    lua_pushnumber(L, count);
    return 1;
}

/// GetSocketTypes(index) → the colour name, which indexes GEM_TYPE_INFO.
int lua_GetSocketTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    lua_pushstring(L, socketColorName(socketMaskAt(gh, index)));
    return 1;
}

/// GetExistingSocketInfo(index) → the gem already in the socket.
///
/// Which is not in the item template: a gem an owner put in lives in the item's
/// enchantment fields, three of which are sockets, and each holds an
/// enchantment id rather than an item. SpellItemEnchantment.Src_ItemID is the
/// way back to the gem.
int lua_GetExistingSocketInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    if (!gh || !gh->isSocketingOpen() || index < 0 || index > 2) {
        return pushGemInfo(L, gh, 0, index);
    }
    const auto enchants = gh->getItemSocketEnchantIds(gh->getSocketItemGuid());
    return pushGemInfo(L, gh, gh->getEnchantGemItem(enchants[index]), index);
}

/// GetNewSocketInfo(index) → the gem the player has put in but not committed.
int lua_GetNewSocketInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    if (!gh || !gh->isSocketingOpen() || index < 0 || index > 2) {
        return pushGemInfo(L, gh, 0, index);
    }
    return pushGemInfo(L, gh, gh->getSocketPendingGemItemId(index), index);
}

/// GetSocketItemInfo() → name, icon, quality of the item being socketed. The
/// panel's portrait and title.
int lua_GetSocketItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* info = socketItemInfo(gh);
    if (!info) { lua_pushnil(L); lua_pushnil(L); lua_pushnil(L); return 3; }
    lua_pushstring(L, info->name.c_str());
    lua_pushstring(L, gh->getItemIconPath(info->displayInfoId).c_str());
    lua_pushnumber(L, info->quality);
    return 3;
}

/// ClickSocketButton(index) - put down what is carried, or take back what is
/// in the socket when nothing is.
///
/// Only a *pending* gem comes back out. One already socketed is in the item on
/// the server and cannot be removed by clicking it; the panel's own artwork
/// says so by drawing closed brackets around it.
int lua_ClickSocketButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
    if (!gh || !gh->isSocketingOpen() || index < 0 || index > 2) return 0;

    uint8_t bag = 0, slot = 0;
    if (!wowee::ui::frameXmlCursorWireSlot(bag, slot)) {
        gh->setSocketGem(index, 0, 0);
        return 0;
    }

    // The cursor speaks in wire (container, slot) pairs; the request speaks in
    // guids. Backpack slots arrive as 0xFF plus a wire slot, worn bags as their
    // container number.
    uint64_t gemGuid = 0;
    if (bag == 0xFF) {
        const int index0 = static_cast<int>(slot) - game::slots::backpackWireSlot(0);
        gemGuid = gh->getBackpackItemGuid(index0);
    } else {
        for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
            if (bag != static_cast<uint8_t>(game::slots::wornBagContainer(b))) continue;
            gemGuid = gh->getBagItemGuid(b, slot);
            break;
        }
    }
    if (gemGuid == 0) return 0;

    const uint32_t gemItemId = gh->getItemEntryByGuid(gemGuid);
    // Only a gem goes in a socket. Without this any item on the cursor could be
    // dropped into one, and the request would be refused by the server with
    // nothing on screen to say why.
    const auto* gem = gh->getItemInfo(gemItemId);
    if (!gem || gem->gemProperties == 0) return 0;

    // 0x005c4780: a meta socket takes only a meta gem and a meta gem goes only
    // in a meta socket - the Meta bit (1) of the socket's colour against the
    // GemProperties row's Type. A gem whose row is not in the file is let in.
    if (const auto* item = socketItemInfo(gh); item && index >= 0 && index < 3) {
        auto* am = gh->services().assetManager;
        auto dbc = am && am->isInitialized() ? am->loadDBC("GemProperties.dbc") : nullptr;
        if (dbc && dbc->isLoaded() && dbc->findRecordById(gem->gemProperties) >= 0) {
            const bool metaSocket = (item->socketColor[static_cast<size_t>(index)] & 1u) != 0;
            const bool metaGem = (gemColorMask(gh, gemItemId) & 1u) != 0;
            if (metaSocket != metaGem) {
                const char* key = metaSocket ? "ERR_SOCKETING_REQUIRES_META_GEM"
                                             : "ERR_SOCKETING_META_GEM_ONLY_IN_METASLOT";
                gh->playGameErrorSound(key);
                gh->addUIError(game::interfaceString(
                    key, metaSocket ? "Only a Meta gem can be placed in this socket."
                                    : "Meta gems can only be placed in a Meta socket."));
                return 0;
            }
        }
    }

    if (gh->setSocketGem(index, gemGuid, gemItemId)) {
        wowee::ui::frameXmlPutCursorDown();
    }
    return 0;
}

int lua_AcceptSockets(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->acceptSockets();
    return 0;
}

int lua_CloseSocketInfo(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->closeSocketing();
    return 0;
}

/// GetSocketItemRefundable() (0x005c5470 -> 0x005c50e0): 1 while the item
/// on the socketing frame has a refund record, carries no enchant or gem yet
/// (0x00708ac0) and is inside its two hours of played time; else nil.
int lua_GetSocketItemRefundable(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = gh ? gh->getSocketItemGuid() : 0;
    const auto* refund = guid ? gh->getItemRefundInfo(guid) : nullptr;
    bool refundable = false;
    if (refund) {
        const auto sockets = gh->getItemSocketEnchantIds(guid);
        const bool modified = gh->getItemEnchantIds(guid).first != 0 ||
                              sockets[0] != 0 || sockets[1] != 0 || sockets[2] != 0;
        refundable = !modified && refund->playedSincePurchase < 2 * 60 * 60;
    }
    if (refundable) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}

/// GetSocketItemBoundTradeable() (0x005c4ea0 -> 0x005c4970): 1 while that
/// item is bound but still tradeable (0x00708520 without 0x00709550) -
/// ITEM_FIELD_FLAGS 0x1 with 0x100, which the server takes away when the
/// trade window closes; else nil.
int lua_GetSocketItemBoundTradeable(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t guid = gh ? gh->getSocketItemGuid() : 0;
    const uint32_t flags = guid ? gh->getItemFieldFlags(guid) : 0;
    if ((flags & 0x1u) && (flags & 0x100u)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}

}  // namespace

namespace {

/// The SpellItemEnchantment columns a socketed gem's line reads past its name:
/// ConditionID (+0x48), RequiredSkillID (+0x4c), RequiredSkillRank (+0x50)
/// and MinLevel (+0x54). WotLK keeps them straight after Src_ItemID.
struct GemEnchantRow {
    uint32_t conditionId = 0;
    uint32_t skillId = 0;
    uint32_t skillRank = 0;
    uint32_t minLevel = 0;
};

GemEnchantRow gemEnchantRow(game::GameHandler* gh, uint32_t enchantId) {
    GemEnchantRow out;
    auto* am = gh ? gh->services().assetManager : nullptr;
    if (!am || !am->isInitialized() || enchantId == 0) return out;
    auto dbc = am->loadDBC("SpellItemEnchantment.dbc");
    if (!dbc || !dbc->isLoaded()) return out;
    const auto* sieL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
    const uint32_t gemField = pipeline::detectEnchantmentGemItemField(dbc.get(), sieL);
    if (gemField == 0 || gemField + 4 >= dbc->getFieldCount()) return out;
    const int32_t row = dbc->findRecordById(enchantId);
    if (row < 0) return out;
    const auto r = static_cast<uint32_t>(row);
    out.conditionId = dbc->getUInt32(r, gemField + 1);
    out.skillId     = dbc->getUInt32(r, gemField + 2);
    out.skillRank   = dbc->getUInt32(r, gemField + 3);
    out.minLevel    = dbc->getUInt32(r, gemField + 4);
    return out;
}

/// SpellItemEnchantmentCondition row `id`, as the file packs it (64 bytes).
bool conditionRow(game::GameHandler* gh, uint32_t id, game::gem_conditions::Row& row) {
    auto* am = gh ? gh->services().assetManager : nullptr;
    if (!am || !am->isInitialized() || id == 0) return false;
    auto dbc = am->loadDBC("SpellItemEnchantmentCondition.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getRecordSize() < 64) return false;
    const int32_t r = dbc->findRecordById(id);
    if (r < 0) return false;
    const uint8_t* rec = dbc->getRecord(static_cast<uint32_t>(r));
    if (!rec) return false;
    for (size_t i = 0; i < 5; ++i) {
        row.color[i] = rec[4 + i];
        row.comparator[i] = rec[29 + i];
        row.compareColor[i] = rec[34 + i];
        int32_t v = 0;
        std::memcpy(&v, rec + 39 + i * 4, sizeof(v));
        row.value[i] = v;
        row.logic[i] = rec[59 + i];
    }
    return true;
}

/// The gems the player wears, per colour (0x006de4d0): every socket of every
/// equipped item that is not broken, each colour bit of a gem counted.
game::gem_conditions::Counts equippedGemCounts(game::GameHandler* gh) {
    game::gem_conditions::Counts counts{};
    if (!gh) return counts;
    const auto& inv = gh->getInventory();
    for (int slot = 0; slot < 19; ++slot) {
        const auto& s = inv.getEquipSlot(static_cast<game::EquipSlot>(slot));
        if (s.empty()) continue;
        if (s.item.maxDurability > 0 && s.item.curDurability == 0) continue;
        const auto gems = gh->getItemSocketEnchantIds(gh->getEquipSlotGuid(slot));
        for (uint32_t ench : gems) {
            const uint32_t gem = gh->getEnchantGemItem(ench);
            if (gem) game::gem_conditions::addGem(counts, gemColorMask(gh, gem));
        }
    }
    return counts;
}

void pushConditionLines(lua_State* L, const game::gem_conditions::Result& res) {
    lua_newtable(L);
    int n = 0;
    auto colorName = [](uint8_t c) {
        const char* k = game::gem_conditions::colorKey(c);
        return k ? k : "UNKNOWN";
    };
    for (const auto& line : res.lines) {
        lua_newtable(L);
        if (line.key) { lua_pushstring(L, line.key); lua_setfield(L, -2, "key"); }
        lua_pushboolean(L, line.valueForm ? 1 : 0);
        lua_setfield(L, -2, "valueForm");
        lua_pushnumber(L, line.number);
        lua_setfield(L, -2, "number");
        lua_pushstring(L, colorName(line.colorA));
        lua_setfield(L, -2, "colorA");
        lua_pushstring(L, colorName(line.colorB));
        lua_setfield(L, -2, "colorB");
        lua_pushboolean(L, line.met ? 1 : 0);
        lua_setfield(L, -2, "met");
        lua_rawseti(L, -2, ++n);
    }
}

}  // namespace

void pushSocketedGems(lua_State* L, game::GameHandler* gh, uint64_t itemGuid,
                      uint32_t itemId) {
    if (!gh || itemGuid == 0) return;
    pushSocketedGems(L, gh, gh->getItemSocketEnchantIds(itemGuid), itemId);
}

void pushSocketedGems(lua_State* L, game::GameHandler* gh,
                      const std::array<uint32_t, 3>& enchants, uint32_t itemId) {
    if (!gh) return;
    const auto* info = gh->getItemInfo(itemId);
    bool any = false;
    for (uint32_t e : enchants) any = any || e != 0;
    bool allMatch = true;
    if (info) {
        for (size_t i = 0; i < 3; ++i)
            if (info->socketColor[i] != 0 && enchants[i] == 0) allMatch = false;
    }
    if (any) {
        const auto counts = equippedGemCounts(gh);
        const uint32_t level = gh->getPlayerLevel();
        lua_newtable(L);
        for (size_t i = 0; i < 3; ++i) {
            const uint32_t ench = enchants[i];
            if (ench == 0) continue;
            const std::string text = gh->getEnchantName(ench);
            if (text.empty()) continue;
            lua_newtable(L);
            lua_pushstring(L, text.c_str());
            lua_setfield(L, -2, "text");
            const uint32_t gem = gh->getEnchantGemItem(ench);
            // A gem out of colour leaves the bonus unearned (0x005c48d0).
            if (info && info->socketColor[i] != 0 &&
                (gemColorMask(gh, gem) & info->socketColor[i]) == 0)
                allMatch = false;
            const GemEnchantRow row = gemEnchantRow(gh, ench);
            bool skillOk = true;
            if (row.skillId != 0) {
                const auto& skills = gh->getPlayerSkills();
                auto it = skills.find(row.skillId);
                const int have = (it != skills.end() && it->second.value != 0)
                    ? it->second.effectiveValue() : 0;
                skillOk = static_cast<int>(row.skillRank) <= have;
                const std::string& sn = gh->getSkillName(row.skillId);
                lua_pushstring(L, sn.empty() ? "UNKNOWN" : sn.c_str());
                lua_setfield(L, -2, "skillName");
                lua_pushnumber(L, row.skillRank);
                lua_setfield(L, -2, "skillRank");
            }
            const bool levelOk = row.minLevel <= level;
            lua_pushboolean(L, skillOk ? 1 : 0);
            lua_setfield(L, -2, "skillOk");
            lua_pushboolean(L, levelOk ? 1 : 0);
            lua_setfield(L, -2, "levelOk");
            lua_pushnumber(L, row.minLevel);
            lua_setfield(L, -2, "minLevel");
            bool condOk = true;
            game::gem_conditions::Row cond;
            if (gem != 0 && conditionRow(gh, row.conditionId, cond)) {
                const auto res = game::gem_conditions::evaluate(cond, counts);
                condOk = res.allMet;
                pushConditionLines(L, res);
                lua_setfield(L, -2, "conditions");
            }
            lua_pushboolean(L, condOk ? 1 : 0);
            lua_setfield(L, -2, "conditionsMet");
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_setfield(L, -2, "gems");
    }
    lua_pushboolean(L, allMatch ? 1 : 0);
    lua_setfield(L, -2, "socketBonusMatched");
}

void registerSocketLuaAPI(lua_State* L) {
    static const luaL_Reg fns[] = {
        {"SocketInventoryItem",        lua_SocketInventoryItem},
        {"SocketContainerItem",        lua_SocketContainerItem},
        {"GetNumSockets",              lua_GetNumSockets},
        {"GetSocketTypes",             lua_GetSocketTypes},
        {"GetExistingSocketInfo",      lua_GetExistingSocketInfo},
        {"GetExistingSocketLink",      lua_GetExistingSocketLink},
        {"GetNewSocketLink",           lua_GetNewSocketLink},
        {"GetNewSocketInfo",           lua_GetNewSocketInfo},
        {"GetSocketItemInfo",          lua_GetSocketItemInfo},
        {"GetSocketItemRefundable",    lua_GetSocketItemRefundable},
        {"GetSocketItemBoundTradeable", lua_GetSocketItemBoundTradeable},
        {"ClickSocketButton",          lua_ClickSocketButton},
        {"AcceptSockets",              lua_AcceptSockets},
        {"CloseSocketInfo",            lua_CloseSocketInfo},
        {.name = nullptr, .func = nullptr},
    };
    for (const luaL_Reg* fn = fns; fn->name; ++fn) {
        lua_pushcfunction(L, fn->func);
        lua_setglobal(L, fn->name);
    }
}

}  // namespace wowee::addons

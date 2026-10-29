// lua_lfg_api.cpp - the dungeon finder.
//
// Split out rather than added to a neighbour because it is a whole panel's
// worth of surface: lfdframe.lua, lfgframe.lua and lfrframe.lua between them
// called forty-one globals and not one of them was bound. The LFD micro button
// sits on the main bar, which is handed over by default, so clicking it raised
// on the first line of LFDParentFrame's OnShow.
//
// What is real here and what is not:
//
//   * The dungeon list is real, read from LFGDungeons.dbc through
//     GameHandler::getLfgDungeons(). Names, levels, groups, difficulty and
//     faction all come from the file.
//   * The queue is real - joining, leaving, roles and the queued list go
//     through the LFG verbs this client has had all along.
//   * Locks and rewards are real, off SMSG_LFG_PLAYER_INFO.
//   * The boot vote is real, including who is being voted on and whether this
//     player has answered - the victim arrives as a guid, not a name.
//   * The role check knows which dungeons it is for and how many are
//     answering it.
//   * The proposal's per-member detail and party backfill are NOT modelled. They
//     answer empty, and each says why where it is bound. The raid browser's search
//     is modelled — the rows, how they pair up, how they sort, the bosses they count
//     and the class column — and the block above the bindings says what of it still
//     answers nothing, and why.
//
// That list is worth keeping honest. It read "locks, rewards, the boot vote's
// detail ... are NOT modelled" for some time after all three had been written,
// and a note like this is what the next pass reads instead of the code.
//
// An empty answer is chosen deliberately over a plausible one. Every caller
// here guards, and a fabricated reward or lock reads as fact.
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_taint.hpp"
#include "addons/lua_engine.hpp"
#include "game/game_utils.hpp"
#include "game/interface_text.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::addons {

namespace {

/// FrameXML numbers a category header with the negative of its group id, and
/// keeps headers and dungeons in one key space - LFGIsIDHeader is `id < 0`,
/// while LFGListUpdateHeaderEnabledAndLockedStates indexes the same lists by
/// the groupID it read out of the info table. So the groupID reported for a
/// dungeon has to be the header's id, not the raw number from the DBC.
int headerIdFor(uint32_t groupId) { return -static_cast<int>(groupId); }

/// LFGDungeons.dbc has no name for a category. The real client carries these
/// itself; they are the standard names for the groups the file actually uses,
/// and group 10 is absent from the data rather than omitted here.
const char* groupName(uint32_t groupId) {
    switch (groupId) {
        case 1:  return "Classic Dungeons";
        case 2:  return "Burning Crusade Dungeons";
        case 3:  return "Burning Crusade Heroic Dungeons";
        case 4:  return "Wrath of the Lich King Dungeons";
        case 5:  return "Wrath of the Lich King Heroic Dungeons";
        case 6:  return "Classic Raids";
        case 7:  return "Burning Crusade Raids";
        case 8:  return "Wrath of the Lich King Raids (10)";
        case 9:  return "Wrath of the Lich King Raids (25)";
        case 11: return "Holiday Dungeons";
        default: return "Dungeons";
    }
}

/// How many the group is built for. The file does not say, so it comes from
/// what the group is: five for a dungeon, and the two Wrath raid groups are
/// split ten from twenty-five, which is the whole reason they are two groups.
int maxPlayersFor(const game::LfgDungeon& d) {
    if (d.typeId == static_cast<uint32_t>(game::LfgTypeId::Raid)) {
        return d.groupId == 9 ? 25 : 10;
    }
    return 5;
}

/// The dungeon the raid browser's rows belong to: the list held with its type
/// masked off, as its row of LFGDungeons. That row's map id and difficulty are the
/// pair the client hands the encounter table for both the boss counts a row shows
/// and the boss its tooltip names (0x00555fd0:4915, 0x00553c30:3235 and 3258), and
/// no count and no boss comes out without it.
const game::LfgDungeon* browserDungeon(game::GameHandler* gh) {
    const uint32_t id = gh->getLfgSearchListId() & 0xffffffu;
    if (id == 0) return nullptr;
    for (const auto& d : gh->getLfgDungeons()) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

/// The dungeon finder lists dungeons and the raid browser lists raids, so each
/// asks for its own half of the same file.
bool isDungeonSide(const game::LfgDungeon& d) {
    return d.typeId == static_cast<uint32_t>(game::LfgTypeId::Dungeon) ||
           d.typeId == static_cast<uint32_t>(game::LfgTypeId::Heroic) ||
           d.typeId == static_cast<uint32_t>(game::LfgTypeId::Random);
}
bool isRaidSide(const game::LfgDungeon& d) {
    return d.typeId == static_cast<uint32_t>(game::LfgTypeId::Raid);
}

/// Which dungeons the player has ticked, and which headers are folded up.
/// Both are the panel's own state - FrameXML says so in a comment, "we
/// maintain this list in Lua" - and the only reason they are here is that it
/// asks the client for the starting value.
std::unordered_map<int, bool>& enabledDungeons() {
    static std::unordered_map<int, bool> enabled;
    return enabled;
}
std::unordered_map<int, bool>& collapsedHeaders() {
    static std::unordered_map<int, bool> collapsed;
    return collapsed;
}

int luaReturnTrue(lua_State* L)    { lua_pushboolean(L, 1); return 1; }
int luaReturnNothing(lua_State* L) { (void)L; return 0; }

/// The roles the player has offered. Kept on the handler now - the ready
/// dialog names the role it found you a group for, and a static in this file
/// was not somewhere GetLFGProposal could reach.
uint8_t offeredRoles(game::GameHandler* gh) {
    return gh ? gh->getLfgOfferedRoles() : 0;
}

/// Build the ordered id list one side of the panel shows: a header, then the
/// dungeons under it, then the next header.
void pushChoiceOrder(lua_State* L, game::GameHandler* gh, bool dungeonSide) {
    lua_newtable(L);
    if (!gh) return;
    int n = 0;
    uint32_t lastGroup = 0;
    for (const auto& d : gh->getLfgDungeons()) {
        // groupId 0 is a row that belongs under no header - the old
        // per-zone entries - and LFDList_DefaultFilterFunction drops them
        // anyway. Leaving them out here keeps the list honest either way.
        if (d.groupId == 0) continue;
        if (dungeonSide ? !isDungeonSide(d) : !isRaidSide(d)) continue;
        if (d.groupId != lastGroup) {
            lastGroup = d.groupId;
            lua_pushinteger(L, headerIdFor(d.groupId));
            lua_rawseti(L, -2, ++n);
        }
        lua_pushinteger(L, static_cast<lua_Integer>(d.id));
        lua_rawseti(L, -2, ++n);
    }
}

/// One row of the info table: the twelve values LFG_RETURN_VALUES names, in
/// its order. Getting this order wrong is invisible - every read is by index
/// through that table - so it is written out here field by field.
void pushInfoRow(lua_State* L, const char* name, int typeId, int minLevel,
                 int maxLevel, int recLevel, int minRecLevel, int maxRecLevel,
                 int expansion, int groupId, const char* texture,
                 int difficulty, int maxPlayers) {
    lua_newtable(L);
    auto set = [&](int idx, auto push) { push(); lua_rawseti(L, -2, idx); };
    set(1,  [&]{ lua_pushstring(L, name); });
    set(2,  [&]{ lua_pushinteger(L, typeId); });
    set(3,  [&]{ lua_pushinteger(L, minLevel); });
    set(4,  [&]{ lua_pushinteger(L, maxLevel); });
    set(5,  [&]{ lua_pushinteger(L, recLevel); });
    set(6,  [&]{ lua_pushinteger(L, minRecLevel); });
    set(7,  [&]{ lua_pushinteger(L, maxRecLevel); });
    set(8,  [&]{ lua_pushinteger(L, expansion); });
    set(9,  [&]{ lua_pushinteger(L, groupId); });
    set(10, [&]{ lua_pushstring(L, texture); });
    set(11, [&]{ lua_pushinteger(L, difficulty); });
    set(12, [&]{ lua_pushinteger(L, maxPlayers); });
}

}  // namespace

void registerLfgLuaAPI(lua_State* L) {
    static const std::pair<const char*, lua_CFunction> api[] = {

    // GetLFGTypes() (0x00552dd0): the seven LFG_TYPE_* strings, NONE to
    // RANDOM_DUNGEON, as the interface names them.
    {"GetLFGTypes", [](lua_State* L) -> int {
        static constexpr const char* kTypes[7] = {
            "LFG_TYPE_NONE", "LFG_TYPE_DUNGEON", "LFG_TYPE_RAID", "LFG_TYPE_QUEST",
            "LFG_TYPE_ZONE", "LFG_TYPE_HEROIC_DUNGEON", "LFG_TYPE_RANDOM_DUNGEON"};
        for (const char* key : kTypes) {
            lua_getglobal(L, key);
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                lua_pushstring(L, key);
            }
        }
        return 7;
    }},

    // ---- The catalogue ----

    // GetLFDChoiceInfo(t) / GetLFRChoiceInfo - every listable row keyed by id,
    // headers included. FrameXML calls this once and keeps the result: "this
    // will never change (without a patch)".
    {"GetLFDChoiceInfo", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        lua_newtable(L);
        if (!gh) return 1;
        std::vector<uint32_t> groupsSeen;
        for (const auto& d : gh->getLfgDungeons()) {
            if (d.groupId == 0) continue;
            if (std::find(groupsSeen.begin(), groupsSeen.end(), d.groupId) == groupsSeen.end()) {
                groupsSeen.push_back(d.groupId);
                // The header's own row. Its groupID is itself, which is how
                // LFDList_SetHeaderCollapsed matches children to the header
                // that was clicked.
                pushInfoRow(L, groupName(d.groupId), 0, 0, 0, 0, 0, 0,
                            static_cast<int>(d.expansion), headerIdFor(d.groupId),
                            "", 0, 0);
                lua_rawseti(L, -2, headerIdFor(d.groupId));
            }
            pushInfoRow(L, d.name.c_str(), static_cast<int>(d.typeId),
                        static_cast<int>(d.minLevel), static_cast<int>(d.maxLevel),
                        static_cast<int>(d.recLevel), static_cast<int>(d.minRecLevel),
                        static_cast<int>(d.maxRecLevel), static_cast<int>(d.expansion),
                        headerIdFor(d.groupId), d.texture.c_str(),
                        static_cast<int>(d.difficulty), maxPlayersFor(d));
            lua_rawseti(L, -2, static_cast<lua_Integer>(d.id));
        }
        return 1;
    }},

    {"GetLFDChoiceOrder", [](lua_State* L) -> int {
        pushChoiceOrder(L, getGameHandler(L), /*dungeonSide=*/true);
        return 1;
    }},
    {"GetLFRChoiceOrder", [](lua_State* L) -> int {
        pushChoiceOrder(L, getGameHandler(L), /*dungeonSide=*/false);
        return 1;
    }},

    // ---- The three state tables the panel keeps ----
    //
    // Returned as tables keyed by id. FrameXML maintains them itself after the
    // first read; these are the starting values.

    {"GetLFDChoiceEnabledState", [](lua_State* L) -> int {
        lua_newtable(L);
        for (const auto& [id, on] : enabledDungeons()) {
            lua_pushboolean(L, on ? 1 : 0);
            lua_rawseti(L, -2, id);
        }
        return 1;
    }},
    {"GetLFDChoiceCollapseState", [](lua_State* L) -> int {
        lua_newtable(L);
        for (const auto& [id, folded] : collapsedHeaders()) {
            lua_pushboolean(L, folded ? 1 : 0);
            lua_rawseti(L, -2, id);
        }
        return 1;
    }},
    // Which dungeons this character or the party may not queue for, and why:
    // the party's locks first - not a raid's - then the player's own, each
    // dungeon's reason by its id (0x005576b0).
    {"GetLFDChoiceLockedState", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        lua_newtable(L);
        if (!gh) return 1;
        constexpr uint32_t kRaidType = 2;
        if (gh->hasLfgPartyLocks()) {
            for (const auto& member : gh->getLfgPartyLocks()) {
                for (const auto& [entry, status] : member.locks) {
                    if ((entry >> 24) == kRaidType) continue;
                    lua_pushnumber(L, static_cast<double>(status));
                    lua_rawseti(L, -2, static_cast<lua_Integer>(entry & 0x00FFFFFFu));
                }
            }
        }
        if (gh->hasLfgPlayerLocks()) {
            for (const auto& [dungeonId, status] : gh->getLfgLocks()) {
                lua_pushnumber(L, static_cast<double>(status));
                lua_rawseti(L, -2, static_cast<lua_Integer>(dungeonId));
            }
        }
        return 1;
    }},
    // GetLFDLockPlayerCount() - the player, and each party member whose locks
    // the server has sent: none before the player's own have arrived, one
    // until the party's have (0x00557890).
    {"GetLFDLockPlayerCount", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        lua_Integer count = 0;
        if (gh && gh->hasLfgPlayerLocks())
            count = gh->hasLfgPartyLocks()
                        ? static_cast<lua_Integer>(gh->getLfgPartyLocks().size()) + 1 : 1;
        lua_pushinteger(L, count);
        return 1;
    }},
    // GetLFDLockInfo(dungeonID, index) -> playerName, lockedReason
    //
    // Index 1 is the player, 2 on the party members in the order the server
    // listed them (0x00557930). The name is the name cache's, or UNKNOWN -
    // which it always is for the player, whose lock set carries no guid. The
    // reason is the lock status, indexing LFG_INSTANCE_INVALID_CODES, and 0
    // for a dungeon that is not locked: the interface tests `lockedReason ~=
    // 0`, so nil there formatted a reason for every unlocked dungeon.
    {"GetLFDLockInfo", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const lua_Integer dungeonId = luaL_optinteger(L, 1, 0);
        const lua_Integer index = luaL_optinteger(L, 2, 0);
        if (!gh || dungeonId < 0) return 0;
        const auto unknown = []() { return wowee::game::interfaceString("UNKNOWN", "Unknown"); };
        uint32_t reason = 0;
        if (index == 1) {
            if (!gh->hasLfgPlayerLocks()) return 0;
            const auto& locks = gh->getLfgLocks();
            auto it = locks.find(static_cast<uint32_t>(dungeonId));
            if (it != locks.end()) reason = it->second;
            lua_pushstring(L, unknown().c_str());
        } else {
            if (!gh->hasLfgPartyLocks() || index < 2) return 0;
            const auto& party = gh->getLfgPartyLocks();
            if (static_cast<size_t>(index - 1) > party.size()) return 0;
            const auto& member = party[static_cast<size_t>(index - 2)];
            for (const auto& [entry, status] : member.locks) {
                if ((entry & 0x00FFFFFFu) != static_cast<uint32_t>(dungeonId)) continue;
                reason = status;
                break;
            }
            const std::string& name = gh->lookupName(member.guid);
            lua_pushstring(L, name.empty() ? unknown().c_str() : name.c_str());
        }
        lua_pushnumber(L, static_cast<double>(reason));
        return 2;
    }},

    {"SetLFGDungeonEnabled", [](lua_State* L) -> int {
        const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
        if (id != 0) enabledDungeons()[id] = lua_toboolean(L, 2) != 0;
        return 0;
    }},
    {"SetLFGHeaderCollapsed", [](lua_State* L) -> int {
        const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
        if (id != 0) collapsedHeaders()[id] = lua_toboolean(L, 2) != 0;
        return 0;
    }},
    // ClearLFGDungeon(type, id) (0x00559500 -> 0x005593a0): one slot taken out
    // of the local join record - a type the client knows (under 7) and an id
    // LFGDungeons.dbc has.
    {"ClearLFGDungeon", [](lua_State* L) -> int {
        if (!lua_isnumber(L, 1) && !lua_isnumber(L, 2)) {
            return luaL_error(L, "Usage: ClearLFGDungeon(type, id)");
        }
        const int64_t type = std::llround(lua_tonumber(L, 1));
        const int64_t id = std::llround(lua_tonumber(L, 2));
        auto* gh = getGameHandler(L);
        if (!gh || type < 0 || type >= 7) return 0;
        for (const auto& d : gh->getLfgDungeons()) {
            if (static_cast<int64_t>(d.id) != id) continue;
            gh->lfgRemoveSlot((static_cast<uint32_t>(id) & 0x00FFFFFFu) |
                              (static_cast<uint32_t>(type) << 24));
            break;
        }
        return 0;
    }},
    // ClearAllLFGDungeons() (0x005595d0 -> 0x00559340).
    {"ClearAllLFGDungeons", [](lua_State* L) -> int {
        if (auto* gh = getGameHandler(L)) gh->lfgClearSlots();
        return 0;
    }},
    // SetLFGDungeon(id) (0x00559f50 -> 0x00559b70): the dungeon's slot - its
    // LFGDungeons type in the top byte - added to the local join record.
    // FrameXML calls it once per dungeon ticked, after ClearAllLFGDungeons.
    {"SetLFGDungeon", [](lua_State* L) -> int {
        if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetLFGDungeon(type)");
        auto* gh = getGameHandler(L);
        if (!gh) return 0;
        const uint32_t id = static_cast<uint32_t>(std::llround(lua_tonumber(L, 1)));
        for (const auto& d : gh->getLfgDungeons()) {
            if (d.id != id) continue;
            gh->lfgAddSlot((d.typeId << 24) | (id & 0x00FFFFFFu));
            break;
        }
        return 0;
    }},

    // ---- Roles ----

    // GetLFGRoles() → leader, tank, healer, damage: the four role bits as
    // SetLFGRoles kept them.
    {"GetLFGRoles", [](lua_State* L) -> int {
        const uint8_t roles = offeredRoles(getGameHandler(L));
        lua_pushboolean(L, (roles & 0x01) ? 1 : 0);   // leader (0x00552e10)
        lua_pushboolean(L, (roles & 0x02) ? 1 : 0);   // tank
        lua_pushboolean(L, (roles & 0x04) ? 1 : 0);   // healer
        lua_pushboolean(L, (roles & 0x08) ? 1 : 0);   // damage
        return 4;
    }},
    {"SetLFGRoles", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh) return 0;
        // 0x00553d60: leader, tank, healer, damage as bits 1, 2, 4, 8.
        uint8_t roles = 0;
        if (lua_toboolean(L, 1)) roles |= 0x01;
        if (lua_toboolean(L, 2)) roles |= 0x02;
        if (lua_toboolean(L, 3)) roles |= 0x04;
        if (lua_toboolean(L, 4)) roles |= 0x08;
        gh->lfgSetRoles(roles);
        return 0;
    }},
    // Which roles this character may offer. Every class can deal damage; tank
    // and healer are gated by class rather than by spec, because this client
    // does not read the active tree here and offering too much is a queue that
    // is declined rather than a lie the player cannot see.
    {"GetAvailableRoles", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint8_t cls = gh ? gh->getPlayerClass() : 0;
        // WARRIOR 1, PALADIN 2, PRIEST 5, SHAMAN 7, DRUID 11, DEATH KNIGHT 6
        const bool tank   = cls == 1 || cls == 2 || cls == 11 || cls == 6;
        const bool healer = cls == 2 || cls == 5 || cls == 7 || cls == 11;
        lua_pushboolean(L, tank ? 1 : 0);
        lua_pushboolean(L, healer ? 1 : 0);
        lua_pushboolean(L, 1);
        return 3;
    }},
    // CompleteLFGRoleCheck(accept) (0x00553e20): the role check answered -
    // CMSG_LFG_SET_ROLES with the roles kept, or none when declined - true
    // when it went, false refused for want of a role.
    {"CompleteLFGRoleCheck", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh) return 0;
        const int r = gh->lfgCompleteRoleCheck(lua_toboolean(L, 1) != 0);
        if (r < 0) return 0;
        lua_pushboolean(L, r);
        return 1;
    }},
    // GetLFGRoleUpdateSlot(index) → dungeonType, dungeonID
    //
    // The dungeons a role check is being run for. The popup names the one when
    // there is one and lists them in a tooltip when there are several; with
    // nothing to answer, every check read as "multiple dungeons".
    {"GetLFGRoleUpdateSlot", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
        if (!gh || index < 1) return luaReturnNil(L);
        const auto& dungeons = gh->getLfgRoleCheckDungeons();
        if (index > static_cast<int>(dungeons.size())) return luaReturnNil(L);
        const auto& d = dungeons[static_cast<size_t>(index) - 1];
        lua_pushnumber(L, d.typeId);
        lua_pushnumber(L, d.dungeonId);
        return 2;
    }},

    // ---- The queue ----

    // JoinLFG() (0x00557660): the local join record sent, as SetLFGDungeon
    // built it - refused without a slot or a role.
    {"JoinLFG", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (gh) gh->lfgJoinLocal(offeredRoles(gh));
        return 0;
    }},
    // GetLFGInfoLocal() (0x00552e90) → joined, noPartialClear, achievements,
    // comment, slotCount, and the three needs. The two flags and the needs
    // are never set by anything in the client and stay as they start.
    {"GetLFGInfoLocal", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const auto* join = gh ? gh->getLfgLocalJoin() : nullptr;
        lua_pushboolean(L, join && join->joined);
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        lua_pushstring(L, join ? join->comment.c_str() : "");
        lua_pushnumber(L, join ? static_cast<double>(join->slots.size()) : 0.0);
        for (int i = 0; i < 3; ++i) lua_pushnumber(L, 0);
        return 8;
    }},
    // GetLastQueueStatusIndex() (0x00559ff0 -> 0x00559c40) → where, among the
    // queued slots, the one 0x00beaa0c names sits, counted from one; nil when
    // it is not there. Nothing in 12340 writes 0x00beaa0c (the queue status
    // keeps its dungeon at 0x00beaa28), so what is looked for is a zero slot.
    {"GetLastQueueStatusIndex", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const auto* queued = gh ? gh->getLfgQueuedSlots() : nullptr;
        constexpr uint32_t kLastQueueStatusSlot = 0;
        if (queued) {
            for (size_t i = 0; i < queued->size(); ++i) {
                if ((*queued)[i] != kLastQueueStatusSlot) continue;
                lua_pushnumber(L, static_cast<double>(i + 1));
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }},
    // The two buttons on the dungeon-ready dialog, and the same pair on the
    // minimap's queue menu. Neither was bound, so a pop that did appear could
    // not be answered - and it did not appear either, because LFG_PROPOSAL_SHOW
    // was not fired.
    //
    // Both answer through 0x00554450, which asks the taint check (kind 9:
    // insecure code only inside a hardware event).
    {"AcceptProposal", [](lua_State* L) -> int {
        if (!taint::allowed(L, 9)) return 0;
        auto* gh = getGameHandler(L);
        if (gh && gh->getLfgProposalId() != 0) {
            gh->lfgAcceptProposal(gh->getLfgProposalId(), true);
        }
        return 0;
    }},
    {"RejectProposal", [](lua_State* L) -> int {
        if (!taint::allowed(L, 9)) return 0;
        auto* gh = getGameHandler(L);
        if (gh && gh->getLfgProposalId() != 0) {
            gh->lfgAcceptProposal(gh->getLfgProposalId(), false);
        }
        return 0;
    }},

    // GetLFGQueuedList([table]) (0x00557520) → the queued dungeons as keys,
    // each true: the party's in a group, the player's otherwise. A table
    // passed in is emptied and filled rather than a new one made.
    {"GetLFGQueuedList", [](lua_State* L) -> int {
        if (lua_type(L, 1) != LUA_TTABLE && lua_gettop(L) > 0 && !lua_isnil(L, 1)) {
            return luaL_error(L, "Usage: GetLFDQueuedList([table])");
        }
        if (lua_type(L, 1) == LUA_TTABLE) {
            lua_settop(L, 1);
            lua_pushnil(L);
            while (lua_next(L, 1) != 0) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                lua_pushnil(L);
                lua_rawset(L, 1);
            }
        } else {
            lua_settop(L, 0);
            lua_newtable(L);
        }
        auto* gh = getGameHandler(L);
        if (const auto* queued = gh ? gh->getLfgQueuedSlots() : nullptr) {
            for (const uint32_t slot : *queued) {
                lua_pushnumber(L, slot & 0x00FFFFFFu);
                lua_pushboolean(L, 1);
                lua_rawset(L, -3);
            }
        }
        return 1;
    }},
    // Whether a row can be queued for at all. Nothing is locked here - see
    // GetLFDChoiceLockedState - so the answer is yes and the server decides.
    {"IsLFGDungeonJoinable", luaReturnTrue},
    // GetLFGQueueStats() → hasData, leaderNeeds, tankNeeds, healerNeeds,
    //   dpsNeeds, instanceType, instanceName, averageWait, tankWait,
    //   healerWait, damageWait, myWait, queuedTime
    //
    // This answered nil because the numbers never arrived: the queue-status
    // handler required a longer packet than the server sends and returned
    // before reading any of it. With that fixed the whole row is real - how
    // many of each role the queue still wants, and how long each has been
    // waiting.
    {"GetLFGQueueStats", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh || !gh->isLfgQueued()) { lua_pushboolean(L, 0); return 1; }
        const uint32_t dungeonId = gh->getLfgDungeonId();
        const game::LfgDungeon* d = nullptr;
        for (const auto& e : gh->getLfgDungeons()) {
            if (e.id == dungeonId) { d = &e; break; }
        }
        // Seconds on the wire; the panel prints them with SecondsToTime.
        auto wait = [](int32_t v) { return v >= 0 ? static_cast<double>(v) : 0.0; };

        lua_pushboolean(L, 1);                                   // 1: hasData
        lua_pushnumber(L, 0);                                    // 2: leaderNeeds
        lua_pushnumber(L, gh->getLfgNeedTank());                 // 3: tankNeeds
        lua_pushnumber(L, gh->getLfgNeedHealer());               // 4: healerNeeds
        lua_pushnumber(L, gh->getLfgNeedDps());                  // 5: dpsNeeds
        lua_pushnumber(L, d ? d->typeId : 1);                    // 6: instanceType
        lua_pushstring(L, d ? d->name.c_str() : "");             // 7: instanceName
        lua_pushnumber(L, gh->getLfgAvgWaitSec());               // 8: averageWait
        lua_pushnumber(L, wait(gh->getLfgWaitTank()));           // 9: tankWait
        lua_pushnumber(L, wait(gh->getLfgWaitHealer()));         // 10: healerWait
        lua_pushnumber(L, wait(gh->getLfgWaitDps()));            // 11: damageWait
        // The player's own estimate, which is the one the panel prints as
        // "Average Wait Time" - it was being given the time already waited, so
        // the line described the past rather than the wait. -1 passes through:
        // the panel hides the line for it, which is what the server not
        // knowing yet should look like.
        lua_pushnumber(L, gh->getLfgMyWaitSec());                // 12: myWait
        // A moment, not a duration: LFDSearchStatus_Update computes
        // GetTime() - queuedTime for the timer it counts up. Handed the
        // duration, it subtracted a minute from the clock and reported a queue
        // entered when the client started.
        lua_pushnumber(L, luaGetTimeNow() -
                          static_cast<double>(gh->getLfgQueuedSeconds())); // 13: queuedTime
        return 13;
    }},

    // ---- Detail this client does not keep ----
    //
    // Each of these is a real feature answered empty, not a feature that does
    // not exist. Named separately so a later pass can see what is left.

    // GetLFGProposalMember(index) → isLeader, role, level, responded,
    //                                accepted, name, class
    //
    // The proposal does carry its group - role, whether it is you, whether the
    // player has answered and what they said - once the header is read at the
    // right offsets. What it does not carry is names, levels or classes: the
    // server sends none, so those stay nil rather than being invented, and the
    // dialog draws a role icon and a tick per row, which is what it is for.
    {"GetLFGProposalMember", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const int index = static_cast<int>(luaL_optinteger(L, 1, 0));
        if (!gh || index < 1) return luaReturnNil(L);
        const auto& members = gh->getLfgProposalMembers();
        if (index > static_cast<int>(members.size())) return luaReturnNil(L);
        const auto& m = members[static_cast<size_t>(index) - 1];
        lua_pushboolean(L, 0);                       // 1: isLeader - not sent
        // The same tokens GetTexCoordsForRole indexes by. The mask is the one
        // SetLFGRoles sends: 2 tank, 4 healer, 8 damage.
        lua_pushstring(L, (m.role & 0x02) ? "TANK"
                        : (m.role & 0x04) ? "HEALER"
                                          : "DAMAGER");   // 2: role
        lua_pushnil(L);                              // 3: level - not sent
        lua_pushboolean(L, m.answered ? 1 : 0);      // 4: responded
        lua_pushboolean(L, m.accepted ? 1 : 0);      // 5: accepted
        lua_pushnil(L);                              // 6: name - not sent
        lua_pushnil(L);                              // 7: class - not sent
        return 7;
    }},
    {"GetLFGProposalEncounter", [](lua_State* L) -> int { return luaReturnNil(L); }},
    // GetLFGBootProposal() → inProgress, didVote, myVote, targetName,
    //                          totalVotes, bootVotes, timeLeft, reason
    //
    // Answered nil under a note saying the target's name and the reason were
    // not parsed. The reason was parsed all along, and the name is the victim
    // guid the same handler reads - the packet carries no name, which is why
    // looking for one found nothing. The three flags in front of the guid were
    // read and dropped; without them the dialog cannot tell a vote it has
    // already answered from one it has not.
    {"GetLFGBootProposal", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh || !gh->isLfgBootInProgress()) return luaReturnNil(L);
        lua_pushboolean(L, 1);
        lua_pushboolean(L, gh->hasLfgBootVoted() ? 1 : 0);
        lua_pushboolean(L, gh->getLfgBootMyVote() ? 1 : 0);
        // Sent as it is even when the guid has not resolved yet. lfdframe gates
        // the popup on this being truthy, and an empty string is truthy in Lua
        // - so a name still in flight costs a blank in the question rather than
        // the vote itself, and the victim is a party member whose name the
        // roster almost always already has.
        lua_pushstring(L, gh->getLfgBootTargetName().c_str());
        lua_pushnumber(L, gh->getLfgBootTotal());
        lua_pushnumber(L, gh->getLfgBootVotes());
        lua_pushnumber(L, gh->getLfgBootTimeLeft());
        lua_pushstring(L, gh->getLfgBootReason().c_str());
        return 8;
    }},
    // GetLFGDungeonRewards(dungeonID) → doneToday, moneyBase, moneyVar,
    //                                    experienceBase, experienceVar, numRewards
    //
    // Numbers, not nil, and that matters more than the values:
    // LFDDungeonReadyDialog_UpdateRewards does
    // `moneyBase + moneyVar * numRandoms` on the line after asking, so a nil
    // raises - and it runs from the same branch of the ready dialog that draws
    // the rest, which is the branch a fresh pop opens in.
    //
    // The server sends one money and one experience figure with the variable
    // parts zeroed, so the totals come out right either way.
    {"GetLFGDungeonRewards", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t dungeonId = static_cast<uint32_t>(luaL_optinteger(L, 1, 0));
        const game::LfgReward* r = nullptr;
        if (gh) {
            for (const auto& e : gh->getLfgRewards()) {
                if (e.dungeonId == dungeonId) { r = &e; break; }
            }
        }
        lua_pushboolean(L, r && r->done ? 1 : 0);                    // doneToday
        lua_pushnumber(L, r ? r->money : 0);                         // moneyBase
        lua_pushnumber(L, 0);                                        // moneyVar
        lua_pushnumber(L, r ? r->experience : 0);                    // experienceBase
        lua_pushnumber(L, 0);                                        // experienceVar
        lua_pushnumber(L, r ? static_cast<double>(r->items.size()) : 0);  // numRewards
        return 6;
    }},
    // GetLFGDungeonRewardInfo(dungeonID, index) → name, texturePath, quantity
    //
    // The caller guards the texture with "we may be waiting on the item data
    // to come from the server", which is exactly this client's position before
    // an item query answers.
    // The four the dungeon finder calls and nothing answered.
    //
    // Two of them run on their own: LFDDungeonReadyPopup asks for both lock
    // infos in its OnShow, and suppressing that popup does not stop its
    // handlers - a hidden frame still runs them - so with LFG_PROPOSAL_SHOW
    // fired, forming a group raised on a nil global whether or not this
    // element had been handed over.
    // SearchLFGSort("type") - 0x0055d2b0, an OnClick on one of the browse panel's
    // seven column headers (lfrframe.xml:697-805 gives each its `sortType` word).
    // "group" swaps which of the two arrays of rows is walked first and sorts nothing
    // (0x0055d2b0:10645, 10709-10711); any other word moves that key to the front of
    // the seven, and reverses it if it was already front (:10684-10705). With no word
    // at all the client answers `Usage: SearchLFGSort("type")` and changes nothing
    // (:10638-10642), which is what happens here too - the printer being the one thing
    // still missing.
    {"SearchLFGSort", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh || !lua_isstring(L, 1)) return 0;
        gh->sortLfgBrowser(lua_tostring(L, 1));
        return 0;
    }},
    // LeaveLFG() (0x00554160 -> 0x00553ab0).
    {"LeaveLFG", [](lua_State* L) -> int {
        if (auto* gh = getGameHandler(L)) gh->lfgLeaveLocal();
        return 0;
    }},
    {"RequestLFDPlayerLockInfo", [](lua_State* L) -> int {
        if (auto* gh = getGameHandler(L)) gh->requestLfgPlayerLockInfo();
        return 0;
    }},
    {"RequestLFDPartyLockInfo", [](lua_State* L) -> int {
        if (auto* gh = getGameHandler(L)) gh->requestLfgPartyLockInfo();
        return 0;
    }},
    // GetLFGDungeonRewardLink(dungeonID, index) → the item link for a reward,
    // which shift-clicking a reward icon puts in chat. Same row
    // GetLFGDungeonRewardInfo below reads, said as a link.
    {"GetLFGDungeonRewardLink", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t dungeonId = static_cast<uint32_t>(luaL_optinteger(L, 1, 0));
        const int index = static_cast<int>(luaL_optinteger(L, 2, 0));
        if (!gh || index < 1) return luaReturnNil(L);
        for (const auto& e : gh->getLfgRewards()) {
            if (e.dungeonId != dungeonId) continue;
            if (index > static_cast<int>(e.items.size())) return luaReturnNil(L);
            const auto& item = e.items[static_cast<size_t>(index) - 1];
            gh->ensureItemInfo(item.itemId);
            const auto* info = gh->getItemInfo(item.itemId);
            if (!info || !info->valid) return luaReturnNil(L);
            lua_pushstring(L, game::buildItemLink(item.itemId, info->quality,
                                                  info->name).c_str());
            return 1;
        }
        return luaReturnNil(L);
    }},
    {"GetLFGDungeonRewardInfo", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t dungeonId = static_cast<uint32_t>(luaL_optinteger(L, 1, 0));
        const int index = static_cast<int>(luaL_optinteger(L, 2, 0));
        if (!gh || index < 1) return luaReturnNil(L);
        for (const auto& e : gh->getLfgRewards()) {
            if (e.dungeonId != dungeonId) continue;
            if (index > static_cast<int>(e.items.size())) return luaReturnNil(L);
            const auto& item = e.items[static_cast<size_t>(index) - 1];
            gh->ensureItemInfo(item.itemId);
            const auto* info = gh->getItemInfo(item.itemId);
            lua_pushstring(L, info ? info->name.c_str() : "");
            const std::string icon = gh->getItemIconPath(item.displayInfoId);
            if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
            lua_pushnumber(L, item.count);
            return 3;
        }
        return luaReturnNil(L);
    }},
    // The random dungeon the picker opens on. The first one offered that is
    // not locked, which is what "best choice" means with nothing better to
    // rank by; nil rather than zero when there is none, because the caller
    // tests it and falls back to the specific-dungeon list.
    {"GetRandomDungeonBestChoice", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh) return luaReturnNil(L);
        const auto& locks = gh->getLfgLocks();
        for (const auto& r : gh->getLfgRewards()) {
            auto it = locks.find(r.dungeonId);
            if (it != locks.end() && it->second != 0) continue;
            lua_pushinteger(L, static_cast<lua_Integer>(r.dungeonId));
            return 1;
        }
        return luaReturnNil(L);
    }},
    // The random dungeon's own row, which the picker shows above the list.
    // GetNumRandomDungeons() - how many of the catalogue are the random kind.
    //
    // Unbound, so opening the dungeon finder's type dropdown called a nil
    // global and raised. Type 6 is random, which is the same field
    // GetLFGRandomDungeonInfo below reports.
    {"GetNumRandomDungeons", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        int n = 0;
        if (gh) {
            for (const auto& d : gh->getLfgDungeons()) {
                if (d.typeId == 6) ++n;
            }
        }
        lua_pushinteger(L, n);
        return 1;
    }},
    // GetLFGRandomDungeonInfo(index) → id, name
    //
    // An index into the random dungeons, counted the same way as the call
    // above, and the id first. This took a dungeon id and answered name and
    // type - so the one caller, which walks 1..GetNumRandomDungeons and reads
    // id and name, looked up dungeon 1, then dungeon 2, and put the name into
    // the id and the type into the name. IsLFGDungeonJoinable was then asked
    // about a string.
    {"GetLFGRandomDungeonInfo", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const int index = static_cast<int>(luaL_optinteger(L, 1, 0));
        if (!gh || index < 1) return luaReturnNil(L);
        int n = 0;
        for (const auto& d : gh->getLfgDungeons()) {
            if (d.typeId != 6) continue;
            if (++n != index) continue;
            lua_pushinteger(L, static_cast<lua_Integer>(d.id));
            lua_pushstring(L, d.name.c_str());
            return 2;
        }
        return luaReturnNil(L);
    }},
    // GetLFGDungeonInfo(id) - the same twelve values as a row of the info
    // table, asked one at a time. Answered from the same catalogue.
    {"GetLFGDungeonInfo", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t id = static_cast<uint32_t>(luaL_optinteger(L, 1, 0));
        if (!gh || id == 0) return luaReturnNil(L);
        for (const auto& d : gh->getLfgDungeons()) {
            if (d.id != id) continue;
            lua_pushstring(L, d.name.c_str());
            lua_pushinteger(L, static_cast<lua_Integer>(d.typeId));
            lua_pushinteger(L, static_cast<lua_Integer>(d.minLevel));
            lua_pushinteger(L, static_cast<lua_Integer>(d.maxLevel));
            lua_pushinteger(L, static_cast<lua_Integer>(d.recLevel));
            lua_pushinteger(L, static_cast<lua_Integer>(d.minRecLevel));
            lua_pushinteger(L, static_cast<lua_Integer>(d.maxRecLevel));
            lua_pushinteger(L, static_cast<lua_Integer>(d.expansion));
            lua_pushinteger(L, headerIdFor(d.groupId));
            lua_pushstring(L, d.texture.c_str());
            lua_pushinteger(L, static_cast<lua_Integer>(d.difficulty));
            lua_pushinteger(L, maxPlayersFor(d));
            // Fourteen, not twelve. LFG_RETURN_VALUES names the first dozen and
            // stops, but LFDQueueFrameRandom_UpdateFrame reads two more off the
            // end - and it is the last of them that decides whether a dungeon
            // is drawn as a holiday at all, so without it the seasonal bosses
            // were listed as ordinary random dungeons with the wrong artwork.
            lua_pushstring(L, d.description.c_str());   // 13: description
            lua_pushboolean(L, d.isHoliday ? 1 : 0);    // 14: isHoliday
            return 14;
        }
        return luaReturnNil(L);
    }},
    // Per-boss lockouts inside a raid the character is saved to. The saved
    // instance list has no encounter breakdown in it.
    {"GetInstanceLockTimeRemainingEncounter", [](lua_State* L) -> int { return luaReturnNil(L); }},
    // The countdown on the bind-or-leave dialog, read every frame by its
    // OnUpdate. A zero closes the dialog, which is what should happen when
    // there is nothing pending.
    {"GetInstanceLockTimeRemaining", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        float secondsLeft = 0.0f;
        bool previouslySaved = false;
        uint32_t completedMask = 0;
        if (!gh || !gh->getInstanceLockPrompt(secondsLeft, previouslySaved, completedMask)) {
            lua_pushnumber(L, 0);
            lua_pushboolean(L, 0);
            lua_pushinteger(L, 0);
            lua_pushinteger(L, 0);
            return 4;
        }
        // The mask says which encounters are done; its population count is how
        // many, and the total comes from the map's own encounter list.
        uint32_t complete = 0;
        for (uint32_t bit = completedMask; bit; bit &= bit - 1) ++complete;
        const uint32_t total = gh->getDungeonEncounterCount(gh->getCurrentMapId(),
                                                            gh->getInstanceDifficulty());
        lua_pushnumber(L, secondsLeft);
        lua_pushboolean(L, previouslySaved ? 1 : 0);
        lua_pushinteger(L, static_cast<lua_Integer>(total));
        lua_pushinteger(L, static_cast<lua_Integer>(complete));
        return 4;
    }},
    // Accept and be saved to the instance, or decline and go to the graveyard.
    // The server does neither until one of the two arrives.
    {"RespondInstanceLock", [](lua_State* L) -> int {
        if (auto* gh = getGameHandler(L)) gh->respondInstanceLock(lua_toboolean(L, 1) != 0);
        return 0;
    }},
    // Whether the character is sitting out a deserter or random-dungeon
    // cooldown. Both are auras; this client does not check for them, and
    // answering yes would grey out a queue the server would have allowed.
    {"UnitHasLFGDeserter",       luaReturnFalse},
    {"UnitHasLFGRandomCooldown", luaReturnFalse},
    // Filling an empty slot in a party already inside. Needs the party's own
    // queue state, which is not tracked.
    {"CanPartyLFGBackfill",      luaReturnFalse},
    {"GetPartyLFGBackfillInfo",  [](lua_State* L) -> int { return luaReturnNil(L); }},
    {"PartyLFGStartBackfill",    luaReturnNothing},
    // A note shown beside the queue. Nothing carries it.
    // SetLFGComment([text]) (0x00553f50 -> 0x005539a0): kept for the next
    // join, and sent at once while joined.
    {"SetLFGComment", [](lua_State* L) -> int {
        const char* text = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
        if (auto* gh = getGameHandler(L)) gh->lfgSetComment(text ? text : "");
        return 0;
    }},

    // ---- The raid browser ----
    //
    // A separate system from the queue: it searches for groups already forming
    // rather than building one. None of it is implemented, and the counts answer
    // zero so every loop over results runs zero times. What is answered here is
    // shaped to the client's own answers rather than to "nothing", because a
    // placeholder that answers the wrong *kind* of nothing switches the interface
    // off somewhere else:
    //
    //   SearchLFGGetNumResults    two numbers (this client's returns both the
    //                             shown count and the total, and lfrframe.lua
    //                             names both: `numResults, totalResults`).
    //   SearchLFGGetJoinedID      the joined id, or nothing while none is joined
    //                             - see the binding itself for why zero will not
    //                             do.
    //
    // The requests and the rows are both read now: this client sends
    // CMSG_SEARCH_LFG_JOIN (0x35E) and CMSG_SEARCH_LFG_LEAVE (0x35F) at
    // 0x00558ed0 and 0x00558f90, each with the joined id packed as type<<24|id,
    // and SMSG_UPDATE_LFG_LIST is parsed into rows by parseLfgList (0x0055b770).
    // Those rows are then paired the way 0x00553530 resolves an index — a player
    // record plus the listing attached to it, whose other listings are its
    // members (buildLfgBrowserRows) — and what a pair answers is the field the
    // client's own getter pushes from that record, 0x00555fd0 for the fourteen
    // below. Where that getter falls back, this falls back the same way:
    //
    //   SearchLFGGetNumResults    the rows shown, then the total. The client's
    //                             second number is the sum of two counters whose
    //                             writers were never read (0x00beab28, 0x00beab2c),
    //                             so both numbers here are the rows held; both are
    //                             zero while no list is held, which is the client's
    //                             own guard on 0x00beab20.
    //   SearchLFGGetResults       fourteen of the thirty-eight this getter pushes
    //                             (0x005529e0 adds 28 values that lfrframe.lua:539
    //                             never asks for), at the row index the UI passes
    //                             down (it counts from one). Name, level, comment,
    //                             partyMembers and status come out of the packet:
    //                             the name from the listing's own guid, falling
    //                             back to "UNKNOWN" as that getter does
    //                             (0x00819d40), the comment from the group record
    //                             when the row has one and from the listing
    //                             otherwise (+0x24 against +0x91), partyMembers as
    //                             the client counts it — one under the members held,
    //                             the row's own listing excluded (0x005534c0).
    //                             areaName stays "UNKNOWN": 0x0065c290 is the generic
    //                             store getter, and the call that fetches this record
    //                             lost both its store and its key in the decompile
    //                             (:4865, :4901). What its records hold can be read off
    //                             how they are used — a name at +0x10 that the "class"
    //                             sort key collates (:4628), a male and a female form
    //                             at +0x14 and +0x18 that 0x007159e0 picks between by
    //                             gender, and a string at +0x1c pushed as this
    //                             getter's eighth value — which adds up to a class
    //                             table record, so the local lfrframe.lua:539 calls
    //                             `areaName` is named for a value this client's getter
    //                             does not answer an area with. It is never read, so
    //                             the fallback costs nothing visible and nothing is
    //                             claimed about which store it is. The class column is
    //                             not that store's: it comes from the row's guid, and
    //                             see the two notes in the binding for what a row with
    //                             no name known answers. The two boss counts are the rows
    //                             DungeonEncounter.dbc holds for the joined
    //                             dungeon's instance — the client walks that table
    //                             by the map id and difficulty its own LFGDungeons
    //                             row carries, and counts among them the bits the
    //                             row's mask holds (0x00553830, fed at
    //                             0x00555fd0:4907-4916). The last four are false: 0x005529e0
    //                             pushes four booleans then 24 numbers, every one
    //                             of those pushes lost its arguments in the
    //                             decompile, and which record field answers
    //                             isLeader / isTank / isHealer / isDamage is not
    //                             established.
    //                             A row is answered for an index the client would
    //                             have hidden — a player record with no listing
    //                             attached, for which 0x00555fd0 returns no values
    //                             and lfrframe.lua would SetText a nil — never:
    //                             buildLfgBrowserRows drops those pairs outright.
    //   SearchLFGGetPartyResults  the six lfrframe.lua:628 names, in that getter's
    //                             order (name, level, relationship, className,
    //                             areaName, comment — the two name fields swap
    //                             against the row getter), from the member's own
    //                             listing. The relationship is what the contact
    //                             list answers by guid, "ignored" ahead of "friend"
    //                             as 0x006b52e0 and 0x006b3920 are tried in that
    //                             order, and nothing for a stranger: the tooltip
    //                             shows only members whose relationship is a
    //                             string. A member no name is known for gets the
    //                             client's own answer for that case — nil, an empty
    //                             string, three nils (0x00556280:5010-5016).
    //   SearchLFGGetEncounterResults the boss's name, nothing for its texture and
    //                             whether it is done — see the binding. Nothing for
    //                             the texture is what the client itself answers: the
    //                             spell icon column of the copy on disk is zero in
    //                             every row, so its store lookup answers nothing
    //                             (0x005538b0:3050-3059), and lfrframe.lua:669 keeps
    //                             three names but reads two of them.
    {"SearchLFGGetNumResults",     [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t rows = gh ? gh->getLfgBrowserRowCount() : 0;
        lua_pushinteger(L, rows);
        lua_pushinteger(L, rows);
        return 2;
    }},
    {"SearchLFGGetResults",        [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const lua_Integer index = lua_isnumber(L, 1) ? lua_tointeger(L, 1) : 0;
        const game::LfgBrowserRow* pair = (gh && index > 0)
            ? gh->getLfgBrowserRow(static_cast<uint32_t>(index - 1)) : nullptr;
        if (!pair) return 0;   // as the getter returns for an index out of its range
        const auto* listing = gh->getLfgListRow(pair->listing);
        if (!listing) return 0;
        const auto* group = pair->player == game::kLfgNoRow
            ? nullptr : gh->getLfgListRow(pair->player);
        // The bosses counted are the joined dungeon's instance's, against the
        // row's own mask - the group record's when the row has one and the
        // listing's otherwise, which is the choice at 0x00555fd0:4907-4912.
        const game::LfgDungeon* dungeon = browserDungeon(gh);
        const uint32_t mask = group ? group->encounterMask : listing->encounterMask;
        const game::LfgEncounterCounts bosses = dungeon
            ? game::countLfgEncounters(gh->getDungeonEncounters(), dungeon->mapId,
                                       dungeon->difficulty, mask)
            : game::LfgEncounterCounts{};
        std::string name = gh->lookupName(listing->guid);
        const bool nameKnown = !name.empty();
        if (name.empty()) name = "UNKNOWN";
        lua_pushstring(L, name.c_str());                             // name
        lua_pushinteger(L, listing->level);                          // level
        lua_pushstring(L, "UNKNOWN");                                // areaName
        // The class is not in the list at all. The getter resolves the row's guid
        // through the name cache and reads the class and the gender off the record
        // that answer brought — +0x148 and +0x144 past the name pointer the lookup
        // hands back (0x00555fd0:4873 through 0x0072aab0, and the same read behind the
        // "class" sort key at 0x00555a70:4618-4630). A row whose name is not known
        // therefore has no class either, and the two fall back differently: the class
        // column gets the client's "UNKNOWN" (0x00819d40 at :4874) and the token gets
        // nothing at all (0x0084e280 at :4902), which is the guard lfrframe.lua:553
        // is written around. Where a class is known the name is the store record's,
        // which the client picks between a male and a female form by that gender
        // (0x007159e0: +0x18 then +0x14, falling back to +0x10); this answers the one
        // class name it has, as UnitClass does.
        const uint8_t classId = nameKnown ? gh->lookupPlayerClass(listing->guid) : 0;
        const char* token = luaClassToken(classId);
        if (token) lua_pushstring(L, kLuaClasses[classId]);
        else       lua_pushstring(L, "UNKNOWN");                     // className
        lua_pushstring(L, (group ? group->comment : listing->comment).c_str());
        lua_pushinteger(L, static_cast<lua_Integer>(pair->members.size()));  // partyMembers
        lua_pushboolean(L, listing->listed ? 1 : 0);                 // status
        if (token) lua_pushstring(L, token); else lua_pushnil(L);     // class
        lua_pushinteger(L, bosses.total);                            // encountersTotal
        lua_pushinteger(L, bosses.complete);                         // encountersComplete
        lua_pushboolean(L, 0);                                       // isLeader
        lua_pushboolean(L, 0);                                       // isTank
        lua_pushboolean(L, 0);                                       // isHealer
        lua_pushboolean(L, 0);                                       // isDamage
        return 14;
    }},
    // 0x00556280 wants both of its numbers and answers nothing without them,
    // member rows from member[1] up (0x00553490 skips the row's own listing).
    {"SearchLFGGetPartyResults",   [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh || !lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return 0;
        const lua_Integer index = lua_tointeger(L, 1);
        const lua_Integer which = lua_tointeger(L, 2);
        const game::LfgBrowserRow* pair = index > 0
            ? gh->getLfgBrowserRow(static_cast<uint32_t>(index - 1)) : nullptr;
        const game::LfgListRow* member =
            (pair && which > 0 && which <= static_cast<lua_Integer>(pair->members.size()))
            ? gh->getLfgListRow(pair->members[static_cast<uint32_t>(which - 1)]) : nullptr;
        std::string name = member ? gh->lookupName(member->guid) : std::string();
        if (name.empty()) {
            lua_pushnil(L);
            lua_pushstring(L, "");
            lua_pushnil(L);
            lua_pushnil(L);
            lua_pushnil(L);
            return 5;
        }
        const char* relationship = nullptr;
        for (const auto& c : gh->getContacts())
            if (c.guid == member->guid) {
                if (c.isIgnored()) relationship = "ignored";
                else if (c.isFriend()) relationship = "friend";
                break;
            }
        lua_pushstring(L, name.c_str());
        lua_pushinteger(L, member->level);
        if (relationship) lua_pushstring(L, relationship); else lua_pushnil(L);
        // The member's class comes from the same place the row's does — the record its
        // guid resolves to, +0x148 — and its fallback is the empty string the client
        // answers with here (DAT_009e14ff at 0x00556280:4990), not the "UNKNOWN" the
        // row getter uses. Not a name the panel shows for a member: lfrframe.lua:628
        // keeps six values and reads the name, the level, the relationship and the
        // comment.
        const uint8_t memberClass = gh->lookupPlayerClass(member->guid);
        if (luaClassToken(memberClass)) lua_pushstring(L, kLuaClasses[memberClass]);
        else                            lua_pushstring(L, "");       // className
        lua_pushstring(L, "UNKNOWN");                                // areaName
        lua_pushstring(L, member->comment.c_str());
        return 6;
    }},
    // SearchLFGGetEncounterResults(index, encounterIndex) - 0x00553c30, which
    // wants both numbers and answers nothing at all when the joined list's dungeon
    // has no row of LFGDungeons to read a map and difficulty off (3229-3237). The
    // boss asked for is the encounterIndex-th of that instance in the order the
    // table stores them, and whether it is done comes from the same mask the row's
    // two counts were counted with. Past the end of either - no such row, or no
    // such boss - the client pushes three nils (0x005538b0:3069-3071); its own loop
    // is bounded by encountersTotal, the count of this same walk, so lfrframe.lua
    // never reaches the nil name it would SetText.
    {"SearchLFGGetEncounterResults", [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        if (!gh || !lua_isnumber(L, 1) || !lua_isnumber(L, 2)) return 0;
        const game::LfgDungeon* dungeon = browserDungeon(gh);
        if (!dungeon) return 0;
        const lua_Integer index = lua_tointeger(L, 1);
        const lua_Integer which = lua_tointeger(L, 2);
        const game::LfgBrowserRow* pair = index > 0
            ? gh->getLfgBrowserRow(static_cast<uint32_t>(index - 1)) : nullptr;
        const auto* listing = pair ? gh->getLfgListRow(pair->listing) : nullptr;
        if (!listing) {
            lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
            return 3;
        }
        const auto* group = pair->player == game::kLfgNoRow
            ? nullptr : gh->getLfgListRow(pair->player);
        const uint32_t mask = group ? group->encounterMask : listing->encounterMask;
        const auto* boss = which > 0
            ? game::lfgEncounterAt(gh->getDungeonEncounters(), dungeon->mapId,
                                   dungeon->difficulty, static_cast<uint32_t>(which - 1))
            : nullptr;
        if (!boss) {
            lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
            return 3;
        }
        lua_pushstring(L, boss->name.c_str());
        lua_pushnil(L);                                        // texture: see the row note
        lua_pushboolean(L, game::lfgEncounterDone(mask, *boss) ? 1 : 0);
        return 3;
    }},
    // The joined id, low twenty-four bits only, and nothing at all when no list
    // is joined - this client pushes it only when that id is non-zero
    // (0x005529a0 reading 0x00beaa14), and the browser's dropdown does
    // `SearchLFGGetJoinedID() or "none"`, so a zero would be a number true in
    // Lua and the dropdown would select a value matching no button.
    {"SearchLFGGetJoinedID",       [](lua_State* L) -> int {
        auto* gh = getGameHandler(L);
        const uint32_t id = gh ? gh->getLfgJoinedListId() & 0x00FFFFFFu : 0;
        if (id == 0) return 0;
        lua_pushinteger(L, id);
        return 1;
    }},
    // The two requests, checked the way 0x00559400 checks them: two numbers, a
    // type under the seven this client knows, and an id that names an
    // LFGDungeonData row (its FUN_0065c290 refuses one that does not). Both ask
    // the taint check (kind 9) - and a refused one does nothing at all.
    {"SearchLFGJoin",              [](lua_State* L) -> int {
        if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
            return luaL_error(L, "Usage: SearchLFGJoin(typeID, lfgID)");
        if (!taint::allowed(L, 9)) return 0;
        const uint32_t typeId = static_cast<uint32_t>(lua_tointeger(L, 1));
        const uint32_t lfgId  = static_cast<uint32_t>(lua_tointeger(L, 2));
        auto* gh = getGameHandler(L);
        if (!gh || typeId >= 7) return 0;
        bool knownRow = false;
        for (const auto& d : gh->getLfgDungeons())
            if (d.id == (lfgId & 0x00FFFFFFu)) { knownRow = true; break; }
        if (!knownRow) return 0;
        gh->lfgSearchJoin(typeId, lfgId);
        return 0;
    }},
    {"SearchLFGLeave",             [](lua_State* L) -> int {
        if (!taint::allowed(L, 9)) return 0;
        if (auto* gh = getGameHandler(L)) gh->lfgSearchLeave();
        return 0;
    }},
    };

    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

}  // namespace wowee::addons

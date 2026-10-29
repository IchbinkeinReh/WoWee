#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/**
 * Party/Group member data
 */
struct GroupMember {
    std::string name;
    uint64_t guid = 0;
    uint8_t isOnline = 0;      // 0 = offline, 1 = online
    uint8_t subGroup = 0;      // Raid subgroup (0 for party)
    uint8_t flags = 0;         // Assistant, main tank, etc.
    uint8_t roles = 0;         // LFG roles (3.3.5a)

    // Party member stats (from SMSG_PARTY_MEMBER_STATS)
    uint32_t curHealth = 0;
    uint32_t maxHealth = 0;
    uint8_t powerType = 0;
    uint16_t curPower = 0;
    uint16_t maxPower = 0;
    uint16_t level = 0;
    uint16_t zoneId = 0;
    int16_t posX = 0;
    int16_t posY = 0;
    uint16_t onlineStatus = 0;   // GROUP_UPDATE_FLAG_STATUS bitmask
    bool hasPartyStats = false;  // true once we've received stats
};

/**
 * Full group/party data from SMSG_GROUP_LIST
 */
/// What a loot method is called, as it is written to chat.
///
/// Five, not the three the field's comment used to name. The party window and
/// the /loot command each wrote the five out; a parallel table of lowercase
/// tokens lives in lua_api_helpers.hpp for parsing the command's argument, and
/// the two are in the same order on purpose - the token's index is the wire
/// value this names.
inline const char* lootMethodName(uint8_t method) {
    static constexpr const char* kByMethod[] = {
        "Free for All", "Round Robin", "Master Looter",
        "Group Loot", "Need Before Greed",
    };
    return method < 5 ? kByMethod[method] : "Unknown";
}

/// What an instance difficulty is called, or null for a value that is not one.
///
/// Three places wrote the four words out, and they disagreed about the answer
/// for a value outside the four: two said "Normal" and one said "Unknown".
/// Null here so each keeps its own - the Lua bindings hand this to FrameXML,
/// which compares it against names, while the minimap is only labelling a
/// corner and can say it does not know.
inline const char* instanceDifficultyName(uint32_t difficulty) {
    static constexpr const char* kByDifficulty[] = {
        "Normal", "Heroic", "25 Normal", "25 Heroic",
    };
    return difficulty < 4 ? kByDifficulty[difficulty] : nullptr;
}

/// How many raid target markers there are.
///
/// GameHandler and SocialHandler each declared their own kRaidMarkCount and
/// the icon loader a third, kRaidTargetIconCount. All three are the count of
/// the marks the server indexes, so it is written here and the others name it.
inline constexpr uint32_t kRaidMarkCount = 8;

/// What a raid target marker is called, or null for a value that is not one.
///
/// Written out capitalised for the minimap's tooltip and lowercase for what
/// /mark matches, plus a third time inside that command's help line - one set
/// of eight words under two spellings. The display spelling lives here and
/// the command lowercases what it compares against it.
inline const char* raidMarkName(uint8_t mark) {
    static constexpr const char* kByMark[] = {
        "Star", "Circle", "Diamond", "Triangle",
        "Moon", "Square", "Cross", "Skull",
    };
    return mark < kRaidMarkCount ? kByMark[mark] : nullptr;
}

/// Whether a group's type says raid.
///
/// The field changed meaning between expansions and the client tested it as a
/// number. 1.12 writes 0 for a party and 1 for a raid; 3.3.5 writes a set of
/// flags - 0x01 battleground, 0x02 raid, and above those two the dungeon
/// finder's pair, 0x04 for the group's restriction and 0x08 for the five extra
/// bytes its roster carries (see GroupListData::lfgRestricted) -
/// so `== 1` read a WotLK raid as no raid at all, and every test for a party
/// written as `== 0` read a finder group (0x0C) as no party either. A dungeon
/// group had neither kind of frame.
///
/// The low two bits cover both conventions: 1.12's raid is 1, WotLK's is 2, and
/// a battleground group is a raid as well. The finder's bits mean neither.
inline bool groupIsRaid(uint8_t groupType) {
    return (groupType & 0x03) != 0;
}

struct GroupListData {
    uint8_t groupType = 0;       // 0 = party, 1 = raid
    uint8_t subGroup = 0;
    uint8_t flags = 0;
    uint8_t roles = 0;
    uint8_t lootMethod = 0;      ///< 0..4; see lootMethodName
    uint64_t looterGuid = 0;
    uint8_t lootThreshold = 0;
    uint8_t difficultyId = 0;
    uint8_t raidDifficultyId = 0;
    /// WotLK: the player's difficulty, 0 normal and 1 heroic, the last byte
    /// of a group that has members (0x006d8870 stores it in 0xbd1980).
    bool hasPlayerDifficulty = false;
    uint8_t playerDifficulty = 0;
    uint32_t memberCount = 0;
    std::vector<GroupMember> members;
    uint64_t leaderGuid = 0;
    /// WotLK: the dungeon finder's dungeon of a finder group, the entry its
    /// roster carries (0x006d8870 -> 0x0052c0a0, 0xbd19a4); IsPartyLFG asks
    /// whether there is one (0x0052c310). 0 for any other group, and for
    /// none - leaving or losing the group forgets it (0x0052d6e0).
    uint32_t lfgDungeonEntry = 0;
    /// WotLK: bit 2 of the same type byte, and **not** a finder flag — it is the
    /// group's restriction. The client hands it to 0x0052c0d0, which keeps it in
    /// 0xbd19a8 and fires PARTY_LFG_RESTRICTED whenever the stored state and the
    /// incoming bit differ (:9104-9114). The roster handler empties the party
    /// through 0x0052d6e0 first, which zeroes that byte (:10375), so a restricted
    /// group's every roster re-fires it. What reads the flag: `raidframe.lua:18,
    /// :61` (its roster refresh), the name-taking group command at
    /// 0x006c0000.c:10131/10138 (which picks between errors 0x45/0x46 and
    /// 0x47/0x48 on it), and 0x0052c350 -> 0x0084e4d0.
    bool lfgRestricted = false;

    [[nodiscard]] bool isValid() const { return true; }
    [[nodiscard]] bool isEmpty() const { return memberCount == 0; }
};

/// Whether a worldport forgets the party's finder dungeon (0x0052cc30, from
/// 0x00403b70 once the new map is current): there is one, the player's party
/// slots are empty - no other member in the player's own subgroup, which is
/// what 0x006d8870 fills them from - and the map arrived on is not the
/// dungeon's (LFGDungeons.dbc MapID, +0x1c, of the entry's low 24 bits). An
/// entry with no row is kept. `dungeonMapId` is that row's map, or nothing.
[[nodiscard]] inline bool worldportForgetsLfgDungeon(const GroupListData& party, uint64_t playerGuid,
                                                     std::optional<uint32_t> dungeonMapId,
                                                     uint32_t mapId) {
    if (party.lfgDungeonEntry == 0 || !dungeonMapId) return false;
    for (const GroupMember& m : party.members) {
        if (m.guid != 0 && m.guid != playerGuid && m.subGroup == party.subGroup) return false;
    }
    return *dungeonMapId != mapId;
}

/// Who sits in party slot `slot` (1-4), or 0 for nobody.
///
/// The client keeps four party guids (0xbd1948): "party1".."party4" and
/// GetPartyMember (0x0052c1d0) both read them. They are the other members of
/// the player's own party - in a raid, of the player's own subgroup, which is
/// what GetNumPartyMembers counts as well - in roster order.
inline uint64_t partySlotGuid(const GroupListData& group, uint64_t playerGuid, int slot) {
    if (slot < 1 || slot > 4) return 0;
    const bool raid = groupIsRaid(group.groupType);
    int found = 0;
    for (const auto& m : group.members) {
        if (m.guid == 0 || m.guid == playerGuid) continue;
        if (raid && m.subGroup != group.subGroup) continue;
        if (++found == slot) return m.guid;
    }
    return 0;
}

/**
 * Party command types
 */
enum class PartyCommand : uint32_t {
    INVITE = 0,
    UNINVITE = 1,
    LEAVE = 2,
    // 3 is not an operation. The server's PARTY_OP_SWAP is 4, and SWAP was
    // declared 3, so the operation field of SMSG_PARTY_COMMAND_RESULT could
    // never be recognised as a swap.
    SWAP = 4
};

/**
 * Party command result codes
 */
enum class PartyResult : uint32_t {
    OK = 0,
    BAD_PLAYER_NAME = 1,
    TARGET_NOT_IN_GROUP = 2,
    TARGET_NOT_IN_INSTANCE = 3,
    GROUP_FULL = 4,
    ALREADY_IN_GROUP = 5,
    NOT_IN_GROUP = 6,
    NOT_LEADER = 7,
    PLAYER_WRONG_FACTION = 8,
    IGNORING_YOU = 9,
    LFG_PENDING = 12,
    INVITE_RESTRICTED = 13
};

} // namespace game
} // namespace wowee

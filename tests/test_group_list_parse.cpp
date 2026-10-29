// SMSG_GROUP_LIST, built the way the server writes it and read back.
//
// The packet has two shapes: a plain group, and a dungeon finder group, which
// puts the LFG state and the dungeon entry - five bytes - between the roles
// byte and the group guid. The finder shape was being read six bytes long, on
// a guess that a seventh field "may or may not" be there, tested as "is there
// more than thirteen bytes left" - which there always is. So every member of
// every dungeon group was read one byte late.
//
// Nothing failed loudly. The member count came out of the last three bytes of
// the real count plus the first letter of the first member's name, was clamped
// to forty, and forty members were then read out of the packet's own bytes.
// The party frames drew forty strangers, or nobody.
//
// These build the bytes rather than capture them, so the layout is written
// down in the test as well as in the parser, and the two have to agree. The
// oracle is AzerothCore's Group::SendUpdate.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/group_defines.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
// The packet layer reaches for the running application to ask which expansion
// is active. There is none here, and the parse under test is told which
// expansion it is reading for.
Application* Application::instance = nullptr;
}
}

using wowee::game::GroupListData;
using wowee::game::GroupListParser;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void str(const std::string& s) {
        for (char c : s) bytes.push_back(static_cast<uint8_t>(c));
        bytes.push_back(0);
    }
};

struct Member {
    std::string name;
    uint64_t guid;
    uint8_t roles;
};

/// One SMSG_GROUP_LIST as a 3.3.5 server writes it.
std::vector<uint8_t> groupList(uint8_t groupType, const std::vector<Member>& members,
                               uint64_t leaderGuid, bool lfg) {
    Writer w;
    w.u8(groupType);
    w.u8(0);            // subgroup
    w.u8(0);            // flags
    w.u8(0);            // roles
    if (lfg) {
        w.u8(0);        // lfg state
        w.u32(285);     // lfg dungeon entry
    }
    w.u64(0x1234);      // group guid
    w.u32(7);           // update counter
    w.u32(static_cast<uint32_t>(members.size()));
    for (const auto& m : members) {
        w.str(m.name);
        w.u64(m.guid);
        w.u8(1);        // online
        w.u8(0);        // subgroup
        w.u8(0);        // flags
        w.u8(m.roles);
    }
    w.u64(leaderGuid);
    if (!members.empty()) {
        w.u8(2);        // loot method
        w.u64(leaderGuid);
        w.u8(2);        // loot threshold
        w.u8(1);        // dungeon difficulty
        w.u8(0);        // raid difficulty
        w.u8(1);        // player difficulty (heroic)
    }
    return w.bytes;
}

const std::vector<Member>& fourOthers() {
    // A name beginning with T, because that letter is what the misread count
    // was made of: 0x54000000 is three zero bytes of the real count and a 'T'.
    static const std::vector<Member> members = {
        {"Tholan", 0x1001, 2}, {"Cetrix", 0x1002, 4},
        {"Veleya", 0x1003, 8}, {"Jackaroe", 0x1004, 8},
    };
    return members;
}

GroupListData parsed(const std::vector<uint8_t>& bytes, bool& ok) {
    wowee::network::Packet packet(0, bytes);
    GroupListData data;
    ok = GroupListParser::parse(packet, data, /*hasRoles=*/true,
                                /*hasBattleGroupFlag=*/false);
    return data;
}

}  // namespace

TEST_CASE("a plain party reads back as it was written", "[group-list]") {
    bool ok = false;
    const GroupListData data = parsed(groupList(0x00, fourOthers(), 0x1001, false), ok);
    REQUIRE(ok);
    REQUIRE(data.memberCount == 4);
    REQUIRE(data.members.size() == 4);
    CHECK(data.members[0].name == "Tholan");
    CHECK(data.members[0].guid == 0x1001);
    CHECK(data.members[3].name == "Jackaroe");
    CHECK(data.leaderGuid == 0x1001);
    CHECK(data.lootMethod == 2);
}

TEST_CASE("a dungeon finder group reads back as it was written", "[group-list]") {
    // The whole bug: the same four members, five bytes further along. Which of
    // the two finder-ish bits puts them there is settled by the client, not by
    // what a core happens to write: 0x006d8870 reads the five bytes when
    // `local_8 & 0x800` (:6811), and the way the type byte was folded into that
    // local (:6799-6806) leaves bits 8-15 holding it, so the gate is **bit 3** -
    // 0x08. 0x04 is something else entirely (see the next test), and taking
    // "either" bit read five bytes that were never written for a restricted
    // group that is not a finder one.
    for (const uint8_t groupType : {uint8_t{0x08}, uint8_t{0x0C}}) {
        INFO("groupType 0x" << std::hex << static_cast<int>(groupType));
        bool ok = false;
        const GroupListData data = parsed(groupList(groupType, fourOthers(), 0x1002, true), ok);
        REQUIRE(ok);
        REQUIRE(data.memberCount == 4);
        REQUIRE(data.members.size() == 4);
        CHECK(data.members[0].name == "Tholan");
        CHECK(data.members[1].name == "Cetrix");
        CHECK(data.members[1].guid == 0x1002);
        CHECK(data.members[3].roles == 8);
        CHECK(data.leaderGuid == 0x1002);
    }
}

TEST_CASE("a count no group can have is refused", "[group-list]") {
    // What a shifted read produces. Clamping it to forty and carrying on is
    // what put forty members made of packet bytes in front of the player.
    std::vector<uint8_t> bytes = groupList(0x00, fourOthers(), 0x1001, false);
    // The member count sits after type, subgroup, flags, roles, guid, counter.
    const size_t countAt = 4 + 8 + 4;
    bytes[countAt + 3] = 0x54;
    bool ok = true;
    const GroupListData data = parsed(bytes, ok);
    CHECK_FALSE(ok);
    CHECK(data.members.empty());
}

TEST_CASE("a solo group carries no loot block", "[group-list]") {
    // Members-1 is nought when the player is alone, and the server writes the
    // leader guid and stops. Reading a loot method there would take it from
    // past the end.
    bool ok = false;
    const GroupListData data = parsed(groupList(0x00, {}, 0x1001, false), ok);
    REQUIRE(ok);
    CHECK(data.memberCount == 0);
    CHECK(data.leaderGuid == 0x1001);
}

TEST_CASE("the last byte is the player's difficulty", "[group-list]") {
    // 0x006d8870 reads dungeon, raid and player difficulty in that order
    // after the loot threshold, and stores the last in 0xbd1980.
    bool ok = false;
    const GroupListData data = parsed(groupList(0x02, fourOthers(), 0x1001, false), ok);
    REQUIRE(ok);
    CHECK(data.difficultyId == 1);
    CHECK(data.raidDifficultyId == 0);
    CHECK(data.hasPlayerDifficulty);
    CHECK(data.playerDifficulty == 1);

    // A solo group has no such block, and sets nothing.
    const GroupListData solo = parsed(groupList(0x00, {}, 0x1001, false), ok);
    REQUIRE(ok);
    CHECK_FALSE(solo.hasPlayerDifficulty);
}

TEST_CASE("a finder group's roster carries its dungeon", "[group-list]") {
    // 0x006d8870 hands the entry to 0x0052c0a0 (0xbd19a4); IsPartyLFG
    // (0x0052c310) is whether there is one.
    bool ok = false;
    const GroupListData lfg = parsed(groupList(0x08, fourOthers(), 0x1002, true), ok);
    REQUIRE(ok);
    CHECK(lfg.lfgDungeonEntry == 285);

    const GroupListData plain = parsed(groupList(0x00, fourOthers(), 0x1001, false), ok);
    REQUIRE(ok);
    CHECK(plain.lfgDungeonEntry == 0);
}

TEST_CASE("bit 2 of the type byte restricts the group, and finds nothing", "[group-list]") {
    // The other bit of the pair. 0x006d8870:6994 hands 0x0052c0d0 the type
    // byte's bit 2 (`local_8 >> 10 & 1`), which keeps it in 0xbd19a8 and fires
    // PARTY_LFG_RESTRICTED (event 518) on it; HasLFGRestrictions (0x0052c350)
    // is that byte, while IsPartyLFG (0x0052c310) is the dungeon above. A
    // restricted group is a group the server will not let lead off into a raid
    // or a guild bank - `uiparent.lua:3562` and the ten unitpopup lines - and it
    // is a state a plain party can be in without the finder being involved.
    //
    // Reading it as a finder flag was a live misread: taking "either" bit made
    // this roster consume the five bytes that only a finder group writes, so
    // every member of a restricted non-finder group came out of the wrong bytes.
    bool ok = false;
    const GroupListData restricted = parsed(groupList(0x04, fourOthers(), 0x1001, false), ok);
    REQUIRE(ok);
    REQUIRE(restricted.members.size() == 4);
    CHECK(restricted.members[0].name == "Tholan");
    CHECK(restricted.members[3].roles == 8);
    CHECK(restricted.lfgDungeonEntry == 0);
    CHECK(restricted.lfgRestricted);

    // Both bits, which is what a server writes for a finder group.
    const GroupListData both = parsed(groupList(0x0C, fourOthers(), 0x1002, true), ok);
    REQUIRE(ok);
    CHECK(both.lfgDungeonEntry == 285);
    CHECK(both.lfgRestricted);

    // The finder bit alone: a finder group, unrestricted.
    const GroupListData finder = parsed(groupList(0x08, fourOthers(), 0x1002, true), ok);
    REQUIRE(ok);
    CHECK(finder.lfgDungeonEntry == 285);
    CHECK_FALSE(finder.lfgRestricted);

    // And a party has neither.
    const GroupListData party = parsed(groupList(0x00, fourOthers(), 0x1001, false), ok);
    REQUIRE(ok);
    CHECK_FALSE(party.lfgRestricted);
}

TEST_CASE("a worldport forgets the finder dungeon only ungrouped and off its map", "[group-list]") {
    // 0x0052cc30: no one in the party slots (the player's own subgroup), a
    // dungeon with a row, and the new map not its map.
    using wowee::game::GroupMember;
    using wowee::game::worldportForgetsLfgDungeon;
    constexpr uint64_t kSelf = 0x10;
    GroupListData party;
    party.lfgDungeonEntry = 285;
    party.subGroup = 0;

    // Alone in the group, ported off the dungeon's map: forgotten.
    CHECK(worldportForgetsLfgDungeon(party, kSelf, 43u, 1));
    // On its map: kept.
    CHECK_FALSE(worldportForgetsLfgDungeon(party, kSelf, 43u, 43));
    // No LFGDungeons row: kept.
    CHECK_FALSE(worldportForgetsLfgDungeon(party, kSelf, std::nullopt, 1));
    // No dungeon: nothing to forget.
    GroupListData none = party;
    none.lfgDungeonEntry = 0;
    CHECK_FALSE(worldportForgetsLfgDungeon(none, kSelf, 43u, 1));

    // Another member in the player's subgroup fills a party slot: kept.
    GroupMember other;
    other.guid = 0x20;
    other.subGroup = 0;
    party.members.push_back(other);
    CHECK_FALSE(worldportForgetsLfgDungeon(party, kSelf, 43u, 1));
    // One in another subgroup of a raid does not.
    party.members.back().subGroup = 3;
    CHECK(worldportForgetsLfgDungeon(party, kSelf, 43u, 1));
    // Nor does the player's own entry, should a roster list it.
    party.members.back().guid = kSelf;
    party.members.back().subGroup = 0;
    CHECK(worldportForgetsLfgDungeon(party, kSelf, 43u, 1));
}

// Who sits in party slots 1-4, as "party1".."party4" and GetPartyMember read
// them (0x0052c1d0 reads the client's four party guids at 0xbd1948).
//
// GetPartyMember answered false for every slot, and PartyMemberFrame_UpdateMember
// shows a party frame on that answer alone, so no party frame ever appeared.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "game/group_defines.hpp"

using wowee::game::GroupListData;
using wowee::game::GroupMember;
using wowee::game::partySlotGuid;

namespace {

GroupMember member(uint64_t guid, uint8_t subGroup = 0) {
    GroupMember m;
    m.guid = guid;
    m.subGroup = subGroup;
    return m;
}

constexpr uint64_t kPlayer = 0x10;

}  // namespace

TEST_CASE("party slots are the other members in roster order", "[party]") {
    GroupListData g;
    g.memberCount = 3;
    g.members = {member(0x21), member(0x22), member(0x23)};
    CHECK(partySlotGuid(g, kPlayer, 1) == 0x21);
    CHECK(partySlotGuid(g, kPlayer, 2) == 0x22);
    CHECK(partySlotGuid(g, kPlayer, 3) == 0x23);
    CHECK(partySlotGuid(g, kPlayer, 4) == 0);
}

TEST_CASE("a pair has its one other member in slot 1", "[party]") {
    GroupListData g;
    g.memberCount = 1;
    g.members = {member(0x21)};
    CHECK(partySlotGuid(g, kPlayer, 1) == 0x21);
    CHECK(partySlotGuid(g, kPlayer, 2) == 0);
}

TEST_CASE("the player never takes a party slot", "[party]") {
    GroupListData g;
    g.members = {member(kPlayer), member(0x21)};
    CHECK(partySlotGuid(g, kPlayer, 1) == 0x21);
    CHECK(partySlotGuid(g, kPlayer, 2) == 0);
}

TEST_CASE("slots outside 1-4 hold nobody", "[party]") {
    GroupListData g;
    g.members = {member(0x21), member(0x22), member(0x23), member(0x24), member(0x25)};
    CHECK(partySlotGuid(g, kPlayer, 0) == 0);
    CHECK(partySlotGuid(g, kPlayer, 5) == 0);
    CHECK(partySlotGuid(g, kPlayer, -1) == 0);
}

TEST_CASE("in a raid the slots are the player's own subgroup", "[party]") {
    GroupListData g;
    g.groupType = 0x02;  // WotLK raid flag
    g.subGroup = 1;
    g.members = {member(0x21, 0), member(0x22, 1), member(0x23, 2), member(0x24, 1)};
    CHECK(partySlotGuid(g, kPlayer, 1) == 0x22);
    CHECK(partySlotGuid(g, kPlayer, 2) == 0x24);
    CHECK(partySlotGuid(g, kPlayer, 3) == 0);
}

TEST_CASE("a dungeon finder party is not a raid", "[party]") {
    GroupListData g;
    g.groupType = 0x0C;
    g.subGroup = 0;
    g.members = {member(0x21, 0), member(0x22, 3)};
    CHECK(partySlotGuid(g, kPlayer, 2) == 0x22);
}

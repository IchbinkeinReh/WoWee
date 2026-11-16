// A group member's pet as partypetN (0x0052c6e0) and raidpetN (0x00518ce0)
// answer it: the member's own object first, then the stats packets' pet guid.
#include <catch_amalgamated.hpp>

#include "game/group_defines.hpp"

using wowee::game::GroupMember;
using wowee::game::groupMemberPetGuid;

TEST_CASE("A known member names its charm, else its summon", "[group]") {
    GroupMember m;
    m.guid = 0x10;
    m.petGuid = 0x99;
    CHECK(groupMemberPetGuid(m, false, true, 0x20, 0x30) == 0x20);
    CHECK(groupMemberPetGuid(m, false, true, 0, 0x30) == 0x30);
    CHECK(groupMemberPetGuid(m, true, true, 0, 0) == 0);  // the object wins even with nothing
}

TEST_CASE("Out of range, the stats pet guid answers", "[group]") {
    GroupMember m;
    m.guid = 0x10;
    m.petGuid = 0x99;
    // A party slot wants the member's online bit.
    CHECK(groupMemberPetGuid(m, false, false, 0, 0) == 0);
    m.onlineStatus = 0x0001;
    CHECK(groupMemberPetGuid(m, false, false, 0, 0) == 0x99);
    // A raid record does not.
    m.onlineStatus = 0;
    CHECK(groupMemberPetGuid(m, true, false, 0, 0) == 0x99);
}

TEST_CASE("An empty slot has no pet", "[group]") {
    GroupMember m;
    m.petGuid = 0x99;
    m.onlineStatus = 1;
    CHECK(groupMemberPetGuid(m, true, false, 0, 0) == 0);
}

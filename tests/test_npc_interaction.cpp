// What a right-click on a unit in reach does past the NPC hellos (0x006ddbb0):
// the spell click (0x0071c570) and a group member's vehicle (0x00723050).
#include <catch_amalgamated.hpp>

#include "game/npc_interaction.hpp"

using namespace wowee::game::npc_interaction;

TEST_CASE("a hello flag is answered before the spell click", "[npc][interact]") {
    Unit u;
    u.npcFlags = kSpellClickFlag | 0x80u;  // a vendor as well
    CHECK(decide(u) == Action::Hello);
    u.npcFlags = kSpellClickFlag | 0x1u;   // gossip
    CHECK(decide(u) == Action::Hello);
}

TEST_CASE("a questgiver says hello only with something to say", "[npc][interact]") {
    Unit u;
    u.npcFlags = kSpellClickFlag | kQuestGiverFlag;
    u.questStatus = 1;  // unavailable
    CHECK(decide(u) == Action::SpellClick);
    u.questStatus = 8;  // available
    CHECK(decide(u) == Action::Hello);
}

TEST_CASE("the spell click is refused or kept to the group by FLAGS_2", "[npc][interact]") {
    Unit u;
    u.npcFlags = kSpellClickFlag;
    CHECK(decide(u) == Action::SpellClick);
    u.flags2 = kFlag2PreventSpellClick;
    CHECK(decide(u) == Action::Nothing);
    u.flags2 = kFlag2RestrictPartyInteraction;
    CHECK(decide(u) == Action::Nothing);
    u.inGroup = true;
    CHECK(decide(u) == Action::SpellClick);
}

TEST_CASE("a group member's vehicle with a free seat is entered", "[npc][interact]") {
    Unit u;
    u.isPlayer = true;
    u.emptySeats = 0x02;
    CHECK(decide(u) == Action::Nothing);  // not in the group
    u.inGroup = true;
    CHECK(decide(u) == Action::VehicleEnter);
    u.emptySeats = 0;
    CHECK(decide(u) == Action::Nothing);  // full
    u.emptySeats = 0x02;
    u.isSelf = true;
    CHECK(decide(u) == Action::Nothing);  // one's own
    u.isSelf = false;
    u.isPlayer = false;
    CHECK(decide(u) == Action::Nothing);  // a creature is a spell click or nothing
}

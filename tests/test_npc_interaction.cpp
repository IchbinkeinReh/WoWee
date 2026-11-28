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

// 0x00729620: whether the client offers a unit's services at all.
namespace {
ServiceAsk vendor() {
    ServiceAsk a;
    a.npcFlags = 0x80u;
    a.reactionToPlayer = 4;
    a.playerReaction = 4;
    return a;
}
}  // namespace

TEST_CASE("a friendly vendor is offered", "[npc][service]") {
    CHECK(serviceOffered(vendor()));
}

TEST_CASE("the player must be alive, off a taxi and not charmed", "[npc][service]") {
    auto a = vendor();
    a.playerUnitFlags = 0x100000u;
    CHECK_FALSE(serviceOffered(a));
    a = vendor();
    a.playerCharmed = true;
    CHECK_FALSE(serviceOffered(a));
    a = vendor();
    a.playerAlive = false;
    CHECK_FALSE(serviceOffered(a));
}

TEST_CASE("a form keeps services unless it is flagged 8 or would be left", "[npc][service]") {
    auto a = vendor();
    a.playerInForm = true;
    CHECK_FALSE(serviceOffered(a));
    a.formFlags = 0x8u;
    CHECK(serviceOffered(a));
    a.formFlags = 0;
    a.wouldAutoUnshift = true;
    CHECK(serviceOffered(a));
}

TEST_CASE("the unit: dead, charmed or fighting", "[npc][service]") {
    auto a = vendor();
    a.alive = false;
    CHECK_FALSE(serviceOffered(a));
    a.typeFlags = 0x80u;
    CHECK(serviceOffered(a));
    a = vendor();
    a.charmed = true;
    CHECK_FALSE(serviceOffered(a));
    a.vehicle = true;
    CHECK(serviceOffered(a));
    a = vendor();
    a.inCombat = true;
    CHECK_FALSE(serviceOffered(a));
    a.typeFlags = 0x40000u;
    CHECK(serviceOffered(a));
    a.typeFlags = 0;
    a.playerFlags2 = 0x4000u;
    CHECK(serviceOffered(a));
}

TEST_CASE("ghosts, owners, selection and standing", "[npc][service]") {
    auto a = vendor();
    a.playerGhost = true;
    CHECK_FALSE(serviceOffered(a));
    a.typeFlags = 0x2u;
    CHECK(serviceOffered(a));
    a = vendor();
    a.npcFlags = 0x1u;
    a.typeFlags = 0x800000u;
    a.createdByOther = true;
    CHECK_FALSE(serviceOffered(a));
    a.createdByOther = false;
    CHECK(serviceOffered(a));
    a = vendor();
    a.unitFlags = 0x2000000u;
    CHECK_FALSE(serviceOffered(a));
    a = vendor();
    a.reactionToPlayer = 2;
    CHECK_FALSE(serviceOffered(a));
    a.flags2 = 0x4000u;
    CHECK(serviceOffered(a));
    a = vendor();
    a.npcFlags = 0;
    CHECK_FALSE(serviceOffered(a));
    a = vendor();
    a.npcFlags = 0x1000000u;
    CHECK_FALSE(serviceOffered(a));
    a.spellClickable = true;
    CHECK(serviceOffered(a));
}

TEST_CASE("CONFIRM_XP_LOSS carries five percent of the level's XP (0x006db010)", "[npc][interact]") {
    // fild PLAYER_NEXT_LEVEL_XP, fmul 0.05f (0x00a31bb0), _ftol2.
    CHECK(confirmXpLossArg(0) == 0);
    CHECK(confirmXpLossArg(400) == 20);
    CHECK(confirmXpLossArg(1999) == 99);
    // Level 79's 1,606,700 XP: 80335 (0.05f is a hair above 0.05).
    CHECK(confirmXpLossArg(1606700) == 80335);
    CHECK(confirmXpLossArg(19) == 0);
}

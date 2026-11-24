// What the playtest rounds found, through GameHandler's own dispatch. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/combat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/quest_handler.hpp"

using replay::Session;
using replay::UpdateObject;
using wowee::game::MovementFlags;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr uint64_t kGiver = 0xF130000BEC00150Eull;

void spawnPlayer(Session& s) {
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 9}});
    // Not at the map's origin, from where no movement is sent.
    s.handler().setPosition(-356.4f, -2337.2f, -8.7f);
}

// PlayerMenu::SendQuestGiverQuestDetails, with nothing rewarded.
wowee::network::Packet questDetails(uint32_t questId) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
    p.writeUInt64(kGiver);
    p.writeUInt64(0);
    p.writeUInt32(questId);
    p.writeString("The Hunter's Way");
    p.writeString("Details.");
    p.writeString("Objectives.");
    p.writeUInt8(0);
    for (int i = 0; i < 2; ++i) p.writeUInt32(0);
    p.writeUInt8(0);
    for (int i = 0; i < 40; ++i) p.writeUInt32(0);
    return p;
}

bool has(Session& s, MovementFlags f) { return s.handler().getMovementInfo().hasFlag(f); }

}  // namespace

// The giver was asked for the quest again five seconds after every accept, and
// the server answered that it had been begun.
TEST_CASE("a quest in the log answers its accept", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.receive(questDetails(861));
    s.handler().acceptQuest();
    REQUIRE(s.sent().of(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST).size() == 1);
    auto* quests = s.handler().getQuestHandler();
    REQUIRE(quests);
    CHECK(quests->pendingQuestAcceptTimeoutsRef().count(861) == 1);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 861}}));
    CHECK(quests->pendingQuestAcceptTimeoutsRef().empty());
}

TEST_CASE("an accept that runs out asks its giver nothing", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.receive(questDetails(861));
    s.handler().acceptQuest();
    s.handler().getQuestHandler()->triggerQuestAcceptResync(861, "timeout");
    CHECK(s.sent().of(Opcode::CMSG_QUESTGIVER_QUERY_QUEST).empty());
}

// The vendor a gossip option leads to: the server says the gossip is over, and
// the frame MERCHANT_SHOW puts in its place calls CloseGossip as it hides.
TEST_CASE("a gossip is closed once", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    int closed = 0;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
        if (event == "GOSSIP_CLOSED") ++closed;
    });
    // PlayerMenu::SendGossipMenu with no option and no quest.
    wowee::network::Packet gossip(wowee::game::wireOpcode(Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(kGiver);
    for (int i = 0; i < 4; ++i) gossip.writeUInt32(0);
    s.receive(gossip);
    CHECK(closed == 0);
    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_GOSSIP_COMPLETE)));
    CHECK(closed == 1);
    s.handler().closeGossip();
    CHECK(closed == 1);
    // And again with the next one, closed by the player.
    s.receive(gossip);
    s.handler().closeGossip();
    s.handler().closeGossip();
    CHECK(closed == 2);
    s.handler().setAddonEventCallback({});
}

TEST_CASE("a landing clears FALLING whether or not its packet goes out", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto* mh = s.handler().getMovementHandler();
    REQUIRE(mh);

    SECTION("a step down of a hand's breadth, never told of, lands unsaid") {
        mh->startFall(20);
        CHECK(has(s, MovementFlags::FALLING));
        mh->landFall(0.02f);
        CHECK_FALSE(has(s, MovementFlags::FALLING));
        CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).empty());
    }
    SECTION("a drop is told of") {
        mh->startFall(300);
        mh->landFall(1.5f);
        CHECK_FALSE(has(s, MovementFlags::FALLING));
        CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).size() == 1);
    }
    SECTION("so is any landing of a fall a packet carried") {
        s.handler().sendMovement(Opcode::MSG_MOVE_JUMP);
        CHECK(has(s, MovementFlags::FALLING));
        mh->landFall(0.0f);
        CHECK_FALSE(has(s, MovementFlags::FALLING));
        CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).size() == 1);
    }
    SECTION("held back, it has landed all the same") {
        s.handler().sendMovement(Opcode::MSG_MOVE_JUMP);
        mh->setServerMovementAllowed(false);
        mh->landFall(0.0f);
        CHECK_FALSE(has(s, MovementFlags::FALLING));
        CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).empty());
    }
    SECTION("no fall, no landing") {
        mh->landFall(3.0f);
        CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).empty());
    }
    SECTION("far as its step has it, and over with the landing") {
        mh->startFall(0);
        mh->setFallingFar();
        CHECK(has(s, MovementFlags::FALLINGFAR));
        mh->landFall(9.0f);
        CHECK_FALSE(has(s, MovementFlags::FALLINGFAR));
        mh->setFallingFar();
        CHECK_FALSE(has(s, MovementFlags::FALLINGFAR));
    }
}

TEST_CASE("a swim begun while movement is held back is one", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto* mh = s.handler().getMovementHandler();
    mh->setServerMovementAllowed(false);
    s.handler().sendMovement(Opcode::MSG_MOVE_START_SWIM);
    CHECK(has(s, MovementFlags::SWIMMING));
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_SWIM).empty());
    s.handler().sendMovement(Opcode::MSG_MOVE_STOP_SWIM);
    CHECK_FALSE(has(s, MovementFlags::SWIMMING));
    CHECK(has(s, MovementFlags::FALLING));
}

// CMSG_MOVE_SET_FLY (0x346) was never sent: the take-off went out on the
// ascent, the landing on a heartbeat.
TEST_CASE("taking off and landing are said with CMSG_MOVE_SET_FLY", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto* mh = s.handler().getMovementHandler();
    // Flying allowed: SMSG_MOVE_SET_CAN_FLY as the server sends it.
    wowee::network::Packet allow(wowee::game::wireOpcode(Opcode::SMSG_MOVE_SET_CAN_FLY));
    allow.writePackedGuid(Session::kPlayerGuid);
    allow.writeUInt32(1);
    s.receive(allow);
    REQUIRE(has(s, MovementFlags::CAN_FLY));

    s.handler().sendMovement(Opcode::MSG_MOVE_START_ASCEND);
    CHECK(has(s, MovementFlags::FLYING));
    CHECK(has(s, MovementFlags::ASCENDING));
    CHECK(s.sent().of(Opcode::CMSG_MOVE_SET_FLY).size() == 1);
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_ASCEND).size() == 1);

    mh->setFlightAirborne(false);
    CHECK_FALSE(has(s, MovementFlags::FLYING));
    CHECK(has(s, MovementFlags::FALLING));
    CHECK(s.sent().of(Opcode::CMSG_MOVE_SET_FLY).size() == 2);
    // The fall's first step lands it, and the packet told of the fall.
    mh->landFall(0.0f);
    CHECK_FALSE(has(s, MovementFlags::FALLING));
    CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).size() == 1);

    // A swimmer swims up and takes off from nothing.
    s.handler().sendMovement(Opcode::MSG_MOVE_START_SWIM);
    s.handler().sendMovement(Opcode::MSG_MOVE_START_ASCEND);
    CHECK_FALSE(has(s, MovementFlags::FLYING));
    CHECK(s.sent().of(Opcode::CMSG_MOVE_SET_FLY).size() == 2);
}

// The attack's own rules of distance kept a swing from being asked for beyond
// eight yards and called a hit creature too far away (0x006e2610, 0x006d5eb0).
TEST_CASE("an attack is asked for at any distance and said too far by the server", "[replay][playtest]") {
    Session s;
    // UNIT_FLAG_PLAYER_CONTROLLED, which a player may attack a creature by.
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}, {UF::UNIT_FIELD_FLAGS, 0x8}});
    s.handler().setPosition(-356.4f, -2337.2f, -8.7f);
    constexpr uint64_t kMammoth = 0xF130005D7C000001ull;
    s.receive(UpdateObject().create(kMammoth, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100},
                                     {UF::UNIT_FIELD_MAXHEALTH, 100},
                                     {UF::UNIT_FIELD_FACTIONTEMPLATE, 14}}));
    REQUIRE(s.handler().unitMayAttack(Session::kPlayerGuid, kMammoth));
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "PLAYER_ENTER_COMBAT" || event == "PLAYER_LEAVE_COMBAT") events.push_back(event);
        if (event == "UI_ERROR_MESSAGE" && !args.empty()) events.push_back(args[0]);
    });
    s.handler().startAutoAttack(kMammoth);
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    CHECK(s.handler().isAutoAttacking());
    CHECK(events.empty());
    // Asked for once.
    s.handler().startAutoAttack(kMammoth);
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);

    wowee::network::Packet start(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTART));
    start.writeUInt64(Session::kPlayerGuid);
    start.writeUInt64(kMammoth);
    s.receive(start);
    CHECK(events == std::vector<std::string>{"PLAYER_ENTER_COMBAT"});
    events.clear();

    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSWING_NOTINRANGE)));
    s.handler().getCombatHandler()->updateAutoAttack();
    s.handler().getCombatHandler()->updateAutoAttack();
    CHECK(events.size() == 1);
    CHECK(s.handler().isAutoAttacking());
    events.clear();

    // Stopped when the server has stopped it.
    s.handler().stopAutoAttack();
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSTOP).size() == 1);
    CHECK(events.empty());
    wowee::network::Packet stop(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTOP));
    stop.writePackedGuid(Session::kPlayerGuid);
    stop.writePackedGuid(kMammoth);
    stop.writeUInt32(0);
    s.receive(stop);
    CHECK(events == std::vector<std::string>{"PLAYER_LEAVE_COMBAT"});
    CHECK_FALSE(s.handler().isAutoAttacking());
    s.handler().setAddonEventCallback({});
}

// The ghost flag going raised PLAYER_ALIVE again, and PLAYER_UNGHOST never came.
TEST_CASE("a ghost's return to life is told of as one", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
        if (event == "PLAYER_DEAD" || event == "PLAYER_ALIVE" || event == "PLAYER_UNGHOST") events.push_back(event);
    });
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 1}, {UF::PLAYER_FLAGS, 0x10}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 90}, {UF::PLAYER_FLAGS, 0}}));
    CHECK(events == std::vector<std::string>{"PLAYER_DEAD", "PLAYER_ALIVE", "PLAYER_UNGHOST"});
    s.handler().setAddonEventCallback({});
}

// A loot window stayed open however far the player walked off.
TEST_CASE("starting to move lets go of the loot", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    const auto open = [&](uint64_t from) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        p.writeUInt64(from);
        p.writeUInt8(1);
        p.writeUInt32(25);  // coins
        p.writeUInt8(0);
        s.receive(p);
    };
    open(0xF130005D7C000001ull);
    REQUIRE(s.handler().isLootWindowOpen());
    s.handler().sendMovement(Opcode::MSG_MOVE_STOP);
    CHECK(s.handler().isLootWindowOpen());
    SECTION("walking") { s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD); }
    SECTION("turning") { s.handler().sendMovement(Opcode::MSG_MOVE_START_TURN_LEFT); }
    SECTION("looking round") { s.handler().sendMovement(Opcode::MSG_MOVE_SET_FACING); }
    CHECK_FALSE(s.handler().isLootWindowOpen());
    CHECK(s.sent().of(Opcode::CMSG_LOOT_RELEASE).size() == 1);
    // An item's comes along.
    open(0x4000000000000123ull);
    REQUIRE(s.handler().isLootWindowOpen());
    s.handler().sendMovement(Opcode::MSG_MOVE_START_BACKWARD);
    CHECK(s.handler().isLootWindowOpen());
    s.handler().setAddonEventCallback({});
}

namespace {

constexpr uint32_t kFireball = 133, kProc = 31725;

// Spell::SendSpellStart and SendSpellGo of the player's, with no target.
wowee::network::Packet playerSpell(Opcode opcode, uint8_t count, uint32_t spellId, uint32_t castFlags, uint32_t time) {
    wowee::network::Packet p(wowee::game::wireOpcode(opcode));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(count);
    p.writeUInt32(spellId);
    p.writeUInt32(castFlags);
    p.writeUInt32(time);
    if (opcode == Opcode::SMSG_SPELL_GO) {
        p.writeUInt8(0);
        p.writeUInt8(0);
    }
    p.writeUInt32(0);
    return p;
}

}  // namespace

// A proc's go (cast flags 0x1) raised UNIT_SPELLCAST_STOP and SUCCEEDED for the
// player and ended the cast that was running.
TEST_CASE("a proc going off says nothing and ends no cast", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.handler().spellNameCacheRef()[kFireball].name = "Fireball";
    s.handler().spellNameCacheRef()[kProc].name = "Proc";
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event.rfind("UNIT_SPELLCAST_S", 0) == 0 && !args.empty() && args[0] == "player") events.push_back(event);
    });
    s.receive(playerSpell(Opcode::SMSG_SPELL_START, 1, kFireball, 0, 3000));
    REQUIRE(s.handler().isCasting());
    events.clear();

    s.receive(playerSpell(Opcode::SMSG_SPELL_GO, 0, kProc, 0x1, 0));
    CHECK(events.empty());
    CHECK(s.handler().isCasting());

    // An instant of the player's own meanwhile: it succeeded, the cast goes on.
    s.receive(playerSpell(Opcode::SMSG_SPELL_GO, 2, kProc, 0, 0));
    CHECK(events == std::vector<std::string>{"UNIT_SPELLCAST_SUCCEEDED"});
    CHECK(s.handler().isCasting());
    events.clear();

    s.receive(playerSpell(Opcode::SMSG_SPELL_GO, 1, kFireball, 0, 0));
    CHECK(events == std::vector<std::string>{"UNIT_SPELLCAST_SUCCEEDED", "UNIT_SPELLCAST_STOP"});
    CHECK_FALSE(s.handler().isCasting());
    s.handler().setAddonEventCallback({});
}

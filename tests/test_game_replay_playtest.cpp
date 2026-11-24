// What the playtest rounds found, through GameHandler's own dispatch. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
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
    s.handler().addonEventCallbackRef() = [&](const std::string& event, const std::vector<std::string>&) {
        if (event == "GOSSIP_CLOSED") ++closed;
    };
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

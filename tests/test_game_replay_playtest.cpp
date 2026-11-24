// What the playtest rounds found, through GameHandler's own dispatch. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/combat_handler.hpp"
#include "game/idle_steps.hpp"
#include "game/movement_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/spell_handler.hpp"

#include <array>
#include <utility>
#include <vector>

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

namespace {

// Every UNIT_SPELLCAST_* of the player's, as its event and fourth argument.
struct CastEvents {
    std::vector<std::string> heard;
    Session& session;
    explicit CastEvents(Session& s) : session(s) {
        s.handler().setAddonEventCallback([this](const std::string& event, const std::vector<std::string>& args) {
            const std::string prefix = "UNIT_SPELLCAST_";
            if (event.rfind(prefix, 0) != 0 || args.size() != 4 || args[0] != "player") return;
            heard.push_back(event.substr(prefix.size()) + " " + args[1] + " " + args[3]);
        });
    }
    ~CastEvents() { session.handler().setAddonEventCallback({}); }
    std::vector<std::string> take() { return std::exchange(heard, {}); }
};

wowee::network::Packet castFailed(uint8_t count, uint32_t spellId, uint8_t result) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CAST_FAILED));
    p.writeUInt8(count);
    p.writeUInt32(spellId);
    p.writeUInt8(result);
    return p;
}

wowee::network::Packet spellFailure(Opcode opcode, uint8_t count, uint32_t spellId, uint8_t result) {
    wowee::network::Packet p(wowee::game::wireOpcode(opcode));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(count);
    p.writeUInt32(spellId);
    p.writeUInt8(result);
    return p;
}

constexpr uint8_t kInterrupted = 0x28, kInProgress = 0x69;

}  // namespace

// The fourth argument was the spell's id, not the count the cast bar holds it
// against, and an interrupted cast's bar filled up and flashed.
TEST_CASE("a cast's events carry its count", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto& fireball = s.handler().spellNameCacheRef()[kFireball];
    fireball.name = "Fireball";
    fireball.interruptFlags = 0x1;
    CastEvents events(s);
    using Heard = std::vector<std::string>;

    // An instant's start is no cast shown.
    s.receive(playerSpell(Opcode::SMSG_SPELL_START, 3, kFireball, 0, 0));
    CHECK(events.take().empty());
    s.receive(playerSpell(Opcode::SMSG_SPELL_GO, 3, kFireball, 0, 0));
    CHECK(events.take() == Heard{"SUCCEEDED Fireball 3"});

    s.receive(playerSpell(Opcode::SMSG_SPELL_START, 4, kFireball, 0, 3000));
    CHECK(events.take() == Heard{"START Fireball 4"});

    SECTION("pressed again, it goes on") {
        s.receive(castFailed(5, kFireball, kInProgress));
        CHECK(events.take() == Heard{"FAILED_QUIET Fireball 5"});
        CHECK(s.handler().isCasting());
        s.receive(playerSpell(Opcode::SMSG_SPELL_GO, 4, kFireball, 0, 0));
        CHECK(events.take() == Heard{"SUCCEEDED Fireball 4", "STOP Fireball 4"});
    }
    SECTION("a step interrupts it, once") {
        s.handler().sendMovement(Opcode::MSG_MOVE_START_BACKWARD);
        CHECK(events.take() == Heard{"INTERRUPTED Fireball 4", "STOP Fireball 4"});
        CHECK_FALSE(s.handler().isCasting());
        CHECK(s.sent().of(Opcode::CMSG_CANCEL_CAST).size() == 1);
        // The server's own say ends nothing more.
        s.receive(castFailed(4, kFireball, kInterrupted));
        s.receive(spellFailure(Opcode::SMSG_SPELL_FAILURE, 4, kFireball, kInterrupted));
        CHECK(events.take() == Heard{"INTERRUPTED Fireball 4", "INTERRUPTED Fireball 4"});
    }
    SECTION("the server interrupts it") {
        s.receive(spellFailure(Opcode::SMSG_SPELL_FAILURE, 4, kFireball, kInterrupted));
        CHECK(events.take() == Heard{"INTERRUPTED Fireball 4", "STOP Fireball 4"});
        CHECK_FALSE(s.handler().isCasting());
    }
    SECTION("stopped by hand") {
        s.handler().cancelCast();
        CHECK(events.take() == Heard{"FAILED_QUIET Fireball 4", "STOP Fireball 4"});
    }
}

// A failure ended the channel without a word, and its bar ran on.
TEST_CASE("a channel is ended by its update", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint32_t kRain = 5740;
    s.handler().spellNameCacheRef()[kRain].name = "Rain";
    CastEvents events(s);
    using Heard = std::vector<std::string>;

    wowee::network::Packet start(wowee::game::wireOpcode(Opcode::MSG_CHANNEL_START));
    start.writePackedGuid(Session::kPlayerGuid);
    start.writeUInt32(kRain);
    start.writeUInt32(8000);
    s.receive(start);
    CHECK(events.take() == Heard{"CHANNEL_START Rain 0"});
    s.handler().cancelCast();
    CHECK(events.take().empty());
    s.receive(castFailed(7, kRain, kInterrupted));
    s.receive(spellFailure(Opcode::SMSG_SPELL_FAILURE, 7, kRain, kInterrupted));
    CHECK(events.take() == Heard{"INTERRUPTED Rain 7", "INTERRUPTED Rain 7"});
    CHECK(s.handler().isChanneling());
    wowee::network::Packet end(wowee::game::wireOpcode(Opcode::MSG_CHANNEL_UPDATE));
    end.writePackedGuid(Session::kPlayerGuid);
    end.writeUInt32(0);
    s.receive(end);
    CHECK(events.take() == Heard{"CHANNEL_STOP Rain 0"});
    CHECK_FALSE(s.handler().isChanneling());
}

// A stop and a heartbeat went out after the acknowledgement, for a mover whose
// key was still down: the server had it stand while it ran on.
TEST_CASE("a teleport is answered with its acknowledgement alone", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(has(s, MovementFlags::FORWARD));
    const auto count = [&](Opcode op) { return s.sent().of(op).size(); };
    const size_t stops = count(Opcode::MSG_MOVE_STOP), beats = count(Opcode::MSG_MOVE_HEARTBEAT);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt32(7);   // counter
    p.writeUInt32(0);   // flags
    p.writeUInt16(0);
    p.writeUInt32(1000);
    p.writeFloat(-300.0f);
    p.writeFloat(-2300.0f);
    p.writeFloat(10.0f);
    p.writeFloat(1.0f);
    p.writeUInt32(0);   // fall time
    // And it is an event of the mover's, which asks for the base animation: teleported out of
    // a fall onto the ground, the character went on falling on the spot (playtest T50).
    std::vector<Opcode> events;
    s.handler().setMoverEventCallback([&](Opcode op, uint32_t flags, bool) {
        if (op == Opcode::MSG_MOVE_TELEPORT_ACK) CHECK((flags & static_cast<uint32_t>(MovementFlags::FORWARD)) == 0);
        events.push_back(op);
    });
    s.receive(p);
    CHECK(events == std::vector<Opcode>{Opcode::MSG_MOVE_TELEPORT_ACK});
    s.handler().setMoverEventCallback({});
    CHECK(count(Opcode::MSG_MOVE_TELEPORT_ACK) == 1);
    CHECK(count(Opcode::MSG_MOVE_STOP) == stops);
    CHECK(count(Opcode::MSG_MOVE_HEARTBEAT) == beats);
    // What it did is over (0x0098ba90); the key starts it again.
    CHECK_FALSE(has(s, MovementFlags::FORWARD));
}

// The corpse's marker stood on the map of a character that had died and was
// alive again.
TEST_CASE("only a ghost has a corpse on the map", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    float x = 0.0f, y = 0.0f;
    const auto shown = [&] { return s.handler().getCorpseCanonicalPos(x, y); };
    const auto asked = [&] { return s.sent().of(Opcode::MSG_CORPSE_QUERY).size(); };
    const auto answer = [&](bool found) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_CORPSE_QUERY));
        p.writeUInt8(found ? 1 : 0);
        if (found) {
            p.writeUInt32(s.handler().getCurrentMapId());
            p.writeFloat(-400.0f);
            p.writeFloat(-2400.0f);
            p.writeFloat(20.0f);
            p.writeUInt32(s.handler().getCurrentMapId());
            p.writeUInt32(0);
        }
        s.receive(p);
    };
    const auto values = [&](const UpdateObject::Fields& fields) {
        s.receive(UpdateObject().values(Session::kPlayerGuid, fields));
    };
    const size_t atLogin = asked();
    CHECK_FALSE(shown());
    // Bones of one's own, on the way past, and a corpse not yet bones.
    constexpr uint64_t kBones = 0xF101000000000001ull;
    const auto corpse = [&](uint64_t guid, uint32_t corpseFlags) {
        // A corpse's owner is in the fields a dynamic object's caster is.
        s.receive(UpdateObject().create(guid, wowee::game::ObjectType::CORPSE,
                                        {{UF::DYNAMICOBJECT_CASTER, static_cast<uint32_t>(Session::kPlayerGuid)},
                                         {UF::CORPSE_FIELD_FLAGS, corpseFlags}}));
    };
    corpse(kBones, 0x05);
    CHECK_FALSE(shown());
    CHECK(s.handler().corpseGuidRef() == 0);
    corpse(kBones + 1, 0x04);
    CHECK_FALSE(shown());
    CHECK(s.handler().corpseGuidRef() == kBones + 1);
    // Dead and not released, and raised again from there.
    values({{UF::UNIT_FIELD_HEALTH, 0}});
    CHECK_FALSE(shown());
    answer(true);
    CHECK_FALSE(shown());
    values({{UF::UNIT_FIELD_HEALTH, 100}});
    CHECK_FALSE(shown());
    // Released: the ghost asks, and the answer is shown.
    values({{UF::UNIT_FIELD_HEALTH, 0}});
    values({{UF::UNIT_FIELD_HEALTH, 1}, {UF::PLAYER_FLAGS, 0x10}});
    CHECK(asked() == atLogin + 1);
    CHECK_FALSE(shown());
    answer(true);
    REQUIRE(shown());

    SECTION("alive again, however") {
        values({{UF::PLAYER_FLAGS, 0}});
        CHECK_FALSE(shown());
        answer(true);  // one on its way
        CHECK_FALSE(shown());
        CHECK(asked() == atLogin + 1);
    }
    SECTION("the server has none") {
        answer(false);
        CHECK_FALSE(shown());
    }
    SECTION("another world") {
        s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 1}, {UF::UNIT_FIELD_MAXHEALTH, 181}, {UF::PLAYER_FLAGS, 0x10}});
        CHECK_FALSE(shown());
        CHECK(asked() == atLogin + 2);
        answer(true);
        CHECK(shown());
    }
}

// The bank and the auction house had a class icon for a face: "npc" was
// nobody there.
TEST_CASE("the npc is whoever the open window is of", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kBanker = 0xF130000C2E000010ull, kAuctioneer = 0xF130000C2F000011ull;
    CHECK(s.handler().getInteractionObject() == 0);
    wowee::network::Packet bank(wowee::game::wireOpcode(Opcode::SMSG_SHOW_BANK));
    bank.writeUInt64(kBanker);
    s.receive(bank);
    CHECK(s.handler().getInteractionObject() == kBanker);
    s.handler().closeBank();
    s.handler().settleInteractionObject();
    CHECK(s.handler().getInteractionObject() == 0);
    wowee::network::Packet auction(wowee::game::wireOpcode(Opcode::MSG_AUCTION_HELLO));
    auction.writeUInt64(kAuctioneer);
    auction.writeUInt32(1);
    auction.writeUInt8(1);
    s.receive(auction);
    CHECK(s.handler().getInteractionObject() == kAuctioneer);
    s.handler().closeAuctionHouse();
    s.handler().settleInteractionObject();
    CHECK(s.handler().getInteractionObject() == 0);
}

// A portal run into led nowhere: the server was told of it without being told
// where the player was.
TEST_CASE("a trigger is told of with the place", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto& h = s.handler();
    h.areaTriggerDbcLoadedRef() = true;
    wowee::game::GameHandler::AreaTriggerEntry portal, other;
    portal.id = 2230;
    portal.mapId = other.mapId = h.getCurrentMapId();
    portal.x = -300.0f;
    portal.y = -2300.0f;
    portal.z = 10.0f;
    portal.radius = 5.0f;
    other = portal;
    other.id = 2231;
    h.areaTriggersRef() = {portal, other};
    auto* movement = h.getMovementHandler();
    REQUIRE(movement);
    const auto told = [&] { return s.sent().of(Opcode::CMSG_AREATRIGGER).size(); };

    movement->checkAreaTriggers();
    CHECK(told() == 0);
    h.setPosition(-301.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    REQUIRE(told() == 1);
    const auto& all = s.sent().all();
    REQUIRE(all.size() >= 2);
    CHECK(all[all.size() - 2].getOpcode() == wowee::game::wireOpcode(Opcode::MSG_MOVE_HEARTBEAT));
    CHECK(all.back().getOpcode() == wowee::game::wireOpcode(Opcode::CMSG_AREATRIGGER));
    // Once, and the one over it not while it stands in this one.
    movement->checkAreaTriggers();
    CHECK(told() == 1);
    h.setPosition(-340.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    CHECK(told() == 1);
    h.setPosition(-301.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    CHECK(told() == 2);
    // Not the dead.
    h.setPosition(-340.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    h.setPosition(-301.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    CHECK(told() == 2);
}

TEST_CASE("a trigger's box is turned by its yaw", "[replay][playtest]") {
    wowee::game::GameHandler::AreaTriggerEntry at;
    at.boxLength = 10.0f;  // along the wire's x, canonical y
    at.boxWidth = 2.0f;
    at.boxHeight = 4.0f;
    CHECK(wowee::game::insideAreaTrigger(at, 0.0f, 4.0f, 0.0f));
    CHECK_FALSE(wowee::game::insideAreaTrigger(at, 4.0f, 0.0f, 0.0f));
    CHECK_FALSE(wowee::game::insideAreaTrigger(at, 0.0f, 0.0f, 3.0f));
    at.boxYaw = 1.5707963f;
    CHECK(wowee::game::insideAreaTrigger(at, 4.0f, 0.0f, 0.0f));
    CHECK_FALSE(wowee::game::insideAreaTrigger(at, 0.0f, 4.0f, 0.0f));
}

// A stack split was never split: its count went as one byte of the four.
TEST_CASE("a split's count is four bytes", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.handler().splitItemTo(0xFF, 27, 0xFF, 23, 300);
    const auto sent = s.sent().of(Opcode::CMSG_SPLIT_ITEM);
    REQUIRE(sent.size() == 1);
    const auto& data = sent[0].getData();
    REQUIRE(data.size() == 8);
    CHECK(data[0] == 0xFF);
    CHECK(data[1] == 27);
    CHECK(data[3] == 23);
    CHECK((data[4] | data[5] << 8 | data[6] << 16 | data[7] << 24) == 300);
}

// A worldport in a fight left the interface in it: the player that comes back has
// no flags before its own to see the fight end by.
TEST_CASE("a fight is over as the world is left", "[replay][playtest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}, {UF::UNIT_FIELD_FLAGS, 0x80008}});
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
        if (event.rfind("PLAYER_REGEN", 0) == 0 || event == "PLAYER_LEAVING_WORLD" || event == "INSTANCE_LOCK_STOP")
            events.push_back(event);
    });
    using Heard = std::vector<std::string>;
    s.handler().onLeaveWorld(/*worldport=*/true);
    CHECK(events == Heard{"PLAYER_REGEN_ENABLED", "INSTANCE_LOCK_STOP", "PLAYER_LEAVING_WORLD"});
    events.clear();
    s.handler().sayFightOnEntry();
    CHECK(events == Heard{"PLAYER_REGEN_DISABLED"});
    events.clear();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x8}}));
    CHECK(events == Heard{"PLAYER_REGEN_ENABLED"});
    events.clear();
    s.handler().onLeaveWorld(/*worldport=*/true);
    s.handler().sayFightOnEntry();
    CHECK(events == Heard{"INSTANCE_LOCK_STOP", "PLAYER_LEAVING_WORLD"});
    s.handler().setAddonEventCallback({});
}

// The movement said so with its next packet only.
TEST_CASE("the movement is on the transport the player is on", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    constexpr uint64_t kTram = 0xF11002AFD0000001ull;
    // UPDATEFLAG_TRANSPORT with a stationary position.
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
    p.writeUInt32(1);
    p.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::CREATE_OBJECT2));
    p.writePackedGuid(kTram);
    p.writeUInt8(static_cast<uint8_t>(wowee::game::ObjectType::GAMEOBJECT));
    p.writeUInt16(0x0042);
    for (int i = 0; i < 4; ++i) p.writeFloat(0.0f);
    p.writeUInt32(0);
    p.writeUInt8(0);
    s.receive(p);
    REQUIRE(s.handler().isTransportGuid(kTram));
    s.handler().setPlayerOnTransport(kTram, glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(has(s, MovementFlags::ONTRANSPORT));
    CHECK(s.handler().getMovementInfo().transportGuid == kTram);
    CHECK(s.handler().getMovementInfo().transportZ == 3.0f);
    s.handler().clearPlayerTransport();
    CHECK_FALSE(has(s, MovementFlags::ONTRANSPORT));
    CHECK(has(s, MovementFlags::FORWARD));
}

namespace {

constexpr uint64_t kWolf = 0xF130000BEC001510ull, kPet = 0xF140000BEC001511ull;

void spawnFoes(Session& s) {
    spawnPlayer(s);
    s.receive(UpdateObject().create(kWolf, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100},
                                     {UF::UNIT_FIELD_MAXHEALTH, 100},
                                     {UF::UNIT_FIELD_CREATEDBY, static_cast<uint32_t>(Session::kPlayerGuid)}}));
}

// Unit::SendAttackStateUpdate.
wowee::network::Packet swing(uint64_t attacker, uint64_t target, uint32_t hitInfo, uint32_t damage, uint8_t state,
                             uint32_t absorbed = 0, uint32_t blocked = 0) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ATTACKERSTATEUPDATE));
    p.writeUInt32(hitInfo);
    p.writePackedGuid(attacker);
    p.writePackedGuid(target);
    p.writeUInt32(damage);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt32(1);
    p.writeFloat(static_cast<float>(damage));
    p.writeUInt32(damage);
    if ((hitInfo & 0x60u) != 0) p.writeUInt32(absorbed);
    p.writeUInt8(state);
    p.writeUInt32(0);
    p.writeUInt32(0);
    if ((hitInfo & 0x2000u) != 0) p.writeUInt32(blocked);
    return p;
}

// Unit::SendSpellNonMeleeDamageLog.
wowee::network::Packet spellHit(uint64_t caster, uint64_t target, uint32_t spellId, uint32_t damage, uint32_t absorbed,
                                uint32_t resisted, uint32_t blocked, uint32_t flags) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SPELLNONMELEEDAMAGELOG));
    p.writePackedGuid(target);
    p.writePackedGuid(caster);
    p.writeUInt32(spellId);
    p.writeUInt32(damage);
    p.writeUInt32(0);
    p.writeUInt8(4);
    p.writeUInt32(absorbed);
    p.writeUInt32(resisted);
    p.writeUInt8(0);
    p.writeUInt8(0);
    p.writeUInt32(blocked);
    p.writeUInt32(flags);
    p.writeUInt8(0);
    return p;
}

std::vector<wowee::game::CombatTextEntry::Type> floaters(Session& s) {
    std::vector<wowee::game::CombatTextEntry::Type> types;
    for (const auto& entry : s.handler().getCombatText()) types.push_back(entry.type);
    s.handler().clearCombatText();
    return types;
}

}  // namespace

// The numbers, the portrait's word and the blood came as the packet did, up to half
// a second before the blow, with a word of their own for what was blocked or absorbed
// of it; a pet's numbers never.
TEST_CASE("a blow shows what it did as it is settled", "[replay][playtest]") {
    using T = wowee::game::CombatTextEntry;
    using Types = std::vector<T::Type>;
    Session s;
    spawnFoes(s);
    s.handler().setTarget(kWolf);
    std::vector<wowee::game::AttackerStateUpdateData> kept;
    int words = 0, spurts = 0;
    s.handler().setMeleeHitCallback([&](const wowee::game::AttackerStateUpdateData& d) { kept.push_back(d); });
    s.handler().setBloodSpurtCallback([&](uint64_t victim, uint64_t, bool) { spurts += victim == kWolf; });
    s.handler().setAddonEventValuesCallback(
        [&](const std::string& event, const std::vector<wowee::game::combat_log::Arg>&) { words += event == "UNIT_COMBAT"; });
    s.receive(swing(Session::kPlayerGuid, kWolf, 0x2042, 30, 1, 12, 5));
    REQUIRE(kept.size() == 1);
    CHECK(floaters(s).empty());
    CHECK(words == 0);
    CHECK(spurts == 0);
    auto* combat = s.handler().getCombatHandler();
    combat->settleMeleeBlow(kept[0].blow());
    CHECK(floaters(s) == Types{T::MELEE_DAMAGE});
    CHECK(words == 1);
    combat->bleedFromMeleeBlow(kept[0].blow());
    CHECK(spurts == 1);
    // The pet's.
    s.receive(swing(kPet, kWolf, 0x202, 7, 1));
    combat->settleMeleeBlow(kept[1].blow());
    CHECK(floaters(s) == Types{T::CRIT_DAMAGE});
    CHECK(words == 2);
    // Another's is said and shows none.
    s.receive(swing(0xF130000BEC001599ull, kWolf, 0x2, 7, 1));
    combat->settleMeleeBlow(kept[2].blow());
    CHECK(floaters(s).empty());
    CHECK(words == 3);
    s.handler().setMeleeHitCallback({});
    s.handler().setBloodSpurtCallback({});
    s.handler().setAddonEventValuesCallback({});
}

// A spell's number came with a word for what was absorbed or resisted of it, a share
// of split damage and a hidden spell's as well, and a pet's never.
TEST_CASE("a spell's damage is one text", "[replay][playtest]") {
    using T = wowee::game::CombatTextEntry;
    using Types = std::vector<T::Type>;
    Session s;
    spawnFoes(s);
    s.handler().spellNameCacheRef()[kFireball].name = "Fireball";
    s.handler().spellNameCacheRef()[kProc].name = "Proc";
    s.handler().spellNameCacheRef()[kProc].attr = 0x100;
    const auto hit = [&](uint64_t caster, uint32_t spell, uint32_t damage, uint32_t absorbed, uint32_t resisted,
                         uint32_t blocked, uint32_t flags) {
        s.receive(spellHit(caster, kWolf, spell, damage, absorbed, resisted, blocked, flags));
        return floaters(s);
    };
    CHECK(hit(Session::kPlayerGuid, kFireball, 100, 20, 30, 0, 0) == Types{T::SPELL_DAMAGE});
    CHECK(hit(Session::kPlayerGuid, kFireball, 100, 0, 0, 0, 2) == Types{T::CRIT_DAMAGE});
    CHECK(hit(Session::kPlayerGuid, kFireball, 0, 20, 30, 0, 0) == Types{T::ABSORB});
    CHECK(hit(Session::kPlayerGuid, kFireball, 0, 0, 30, 10, 0) == Types{T::BLOCK});
    CHECK(hit(Session::kPlayerGuid, kFireball, 0, 0, 30, 0, 0) == Types{T::RESIST});
    CHECK(hit(Session::kPlayerGuid, kFireball, 100, 0, 0, 0, 8).empty());
    CHECK(hit(Session::kPlayerGuid, kProc, 100, 0, 0, 0, 0).empty());
    CHECK(hit(kPet, kFireball, 100, 0, 0, 0, 0) == Types{T::SPELL_DAMAGE});
    CHECK(hit(0xF130000BEC001599ull, kFireball, 100, 0, 0, 0, 0).empty());
}

// The acknowledgement went as SMSG_NEW_WORLD came, and the player stood in the new
// world for as long as its client loaded it.
TEST_CASE("a worldport is acknowledged once the world is in", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    const auto acks = [&] { return s.sent().of(Opcode::MSG_MOVE_WORLDPORT_ACK).size(); };
    wowee::network::Packet world(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    world.writeUInt32(1);
    world.writeFloat(-2328.0f);
    world.writeFloat(-358.0f);
    world.writeFloat(-8.7f);
    world.writeFloat(0.4f);
    auto* mh = s.handler().getMovementHandler();
    SECTION("loaded") {
        s.handler().setWorldEntryCallback([&](uint32_t, float, float, float, bool) { mh->holdWorldportAck(); });
        s.receive(world);
        CHECK(acks() == 0);
        CHECK(mh->releaseWorldportAck());
        CHECK(acks() == 1);
        CHECK_FALSE(mh->releaseWorldportAck());
    }
    SECTION("with nothing to load") {
        s.handler().setWorldEntryCallback([](uint32_t, float, float, float, bool) {});
        s.receive(world);
        CHECK(acks() == 1);
        CHECK_FALSE(mh->releaseWorldportAck());
    }
    s.handler().setWorldEntryCallback({});
}

// SMSG_LOGOUT_COMPLETE takes the character out of the world and leaves the
// connection standing (0x006b2180 -> 0x00406510(1, 1, 0) -> "charselect"),
// where the client dropped it and asked for the password again.
TEST_CASE("a logout keeps the connection for the character selection", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.receive(UpdateObject().create(kGiver, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 10}}));
    REQUIRE(s.handler().getEntityManager().getEntity(kGiver));
    int completes = 0;
    bool left = false;
    s.handler().setLogoutCompleteCallback([&](bool exiting) {
        ++completes;
        CHECK_FALSE(exiting);
        left = s.handler().leaveWorldForCharacterList();
    });
    s.setConnected(true);
    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_LOGOUT_COMPLETE)));
    REQUIRE(completes == 1);
    CHECK(left);
    // The socket is the same one, and what the character selection asks for
    // went out on it.
    CHECK(s.handler().isConnected());
    CHECK(s.sent().of(Opcode::CMSG_READY_FOR_ACCOUNT_DATA_TIMES).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_CHAR_ENUM).size() == 1);
    CHECK(s.handler().getState() == wowee::game::WorldState::CHAR_LIST_REQUESTED);
    // The world is gone with the character.
    CHECK_FALSE(s.handler().getEntityManager().getEntity(kGiver));
    CHECK_FALSE(s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
}

// With no connection to keep there is only the login screen (0x004da9d0).
TEST_CASE("a logout without a connection leaves none", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    CHECK_FALSE(s.handler().leaveWorldForCharacterList());
    CHECK(s.handler().getState() == wowee::game::WorldState::IN_WORLD);
    s.setConnected(true);
    s.handler().disconnect();
    CHECK_FALSE(s.handler().leaveWorldForCharacterList());
    CHECK(s.handler().getState() == wowee::game::WorldState::DISCONNECTED);
}

// The equipment manager draws its list on EQUIPMENT_SETS_CHANGED alone, which
// the client raises as it saves, renames and deletes (0x005ade50, 0x005af400,
// 0x005ae260): a deleted set stayed in the window, and a renamed or saved one
// was the old one until the next login.
TEST_CASE("the equipment manager hears of every change at once", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_EQUIPMENT_SET_LIST));
    list.writeUInt32(2);
    for (uint32_t i = 0; i < 2; ++i) {
        list.writePackedGuid(0x70 + i);
        list.writeUInt32(i);
        list.writeString(i == 0 ? "Tank" : "Heal");
        list.writeString("INV_Shield_06");
        for (int slot = 0; slot < 19; ++slot) list.writePackedGuid(slot == 4 ? 1 : 0);
    }
    s.receive(list);
    auto& gh = s.handler();
    REQUIRE(gh.getEquipmentSets().size() == 2);
    // The slot left alone is a 1, as GetEquipmentSetLocations answers it.
    REQUIRE(gh.getEquipmentSetItems(0));
    CHECK((*gh.getEquipmentSetItems(0))[4] == 1);
    CHECK(gh.getEquipmentSetIgnoreMask(0) == 1u << 4);
    gh.setActionBarSlot(5, wowee::game::ActionBarSlot::EQUIPMENT_SET, 0);

    int changed = 0;
    gh.setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
        if (event == "EQUIPMENT_SETS_CHANGED") ++changed;
    });
    gh.renameEquipmentSet("Heal", "Mend", 0);
    CHECK(changed >= 1);
    CHECK(gh.getEquipmentSets()[1].name == "Mend");
    changed = 0;
    gh.saveEquipmentSet("New", "INV_Helmet_01", 0);
    CHECK(changed == 1);
    REQUIRE(gh.getEquipmentSets().size() == 3);
    CHECK(gh.getEquipmentSets()[2].setId == 2);
    changed = 0;
    gh.deleteEquipmentSet("Tank");
    CHECK(changed == 1);
    CHECK(gh.getEquipmentSets().size() == 2);
    CHECK(s.sent().of(Opcode::CMSG_DELETEEQUIPMENT_SET).size() == 1);
    // And off the action bar (0x005aae20).
    CHECK(gh.getActionBar()[5].isEmpty());
    // Wearing a set leaves its ignored slot alone: the guid of one.
    CHECK(gh.useEquipmentSet("Mend", false));
    const auto used = s.sent().of(Opcode::CMSG_EQUIPMENT_SET_USE);
    REQUIRE(used.size() == 1);
    wowee::network::Packet p = used[0];
    CHECK_FALSE(gh.useEquipmentSet("Mend", false));   // the first is not answered
}

// The vendors on the player's own mount stayed, for the game, where they got on.
TEST_CASE("a vehicle's passenger goes where it goes", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kVendor = 0xF130007FF4001520ull;
    s.receive(UpdateObject().create(kVendor, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    // SMSG_MONSTER_MOVE_TRANSPORT, a stop in seat 1: two yards behind, three up.
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT));
    p.writePackedGuid(kVendor);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(1);
    p.writeUInt8(0);
    p.writeFloat(-2.0f);
    p.writeFloat(0.0f);
    p.writeFloat(3.0f);
    p.writeUInt32(1);
    p.writeUInt8(1);
    s.receive(p);
    auto* mh = s.handler().getMovementHandler();
    const auto vendor = s.handler().getEntityManager().getEntity(kVendor);
    REQUIRE(vendor);
    // Facing north (server +x, canonical +y), then west.
    auto& move = s.handler().movementInfoRef();
    move.orientation = wowee::core::coords::serverToCanonicalYaw(0.0f);
    mh->updateAttachedTransportChildren(0.016f);
    CHECK(vendor->getX() == Catch::Approx(move.x).margin(0.01f));
    CHECK(vendor->getY() == Catch::Approx(move.y - 2.0f).margin(0.01f));
    CHECK(vendor->getZ() == Catch::Approx(move.z + 3.0f).margin(0.01f));
    move.x += 60.0f;
    move.y -= 70.0f;
    move.orientation = wowee::core::coords::serverToCanonicalYaw(1.57079633f);
    mh->updateAttachedTransportChildren(0.016f);
    CHECK(vendor->getX() == Catch::Approx(move.x - 2.0f).margin(0.01f));
    CHECK(vendor->getY() == Catch::Approx(move.y).margin(0.01f));
}

// The server drops what comes while it moves the player from map to map, and what had been
// asked for then was never asked for again: an entry of the combat log waited for its name,
// and everything after it behind that.
TEST_CASE("what a worldport lost is asked for again", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kStranger = 0x777;
    s.receive(UpdateObject().create(kStranger, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    auto& gh = s.handler();
    const auto names = [&] {
        size_t n = 0;
        for (wowee::network::Packet p : s.sent().of(Opcode::CMSG_NAME_QUERY)) n += p.readUInt64() == kStranger;
        return n;
    };
    const auto sent = [&](Opcode op) { return s.sent().of(op).size(); };
    gh.queryPlayerName(kStranger);
    REQUIRE(names() == 1);
    gh.queryCreatureInfo(1725, kGiver);
    gh.queryGameObjectInfo(31, 0xF11000001F000001ull);
    gh.queryItemInfo(25, 0);
    gh.getSpellHandler()->requestPetName(0xF140000001000005ull);
    const std::array asked = {Opcode::CMSG_CREATURE_QUERY, Opcode::CMSG_GAMEOBJECT_QUERY,
                              Opcode::CMSG_ITEM_QUERY_SINGLE, Opcode::CMSG_PET_NAME_QUERY};
    std::array<size_t, 4> before{};
    for (size_t i = 0; i < asked.size(); ++i) before[i] = sent(asked[i]);
    // Asked for once, whoever else wants it.
    gh.queryPlayerName(kStranger);
    gh.getSpellHandler()->requestPetName(0xF140000001000005ull);
    CHECK(names() == 1);
    CHECK(sent(Opcode::CMSG_PET_NAME_QUERY) == before[3]);

    gh.setWorldEntryCallback([](uint32_t, float, float, float, bool) {});
    wowee::network::Packet world(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    world.writeUInt32(1);
    for (float v : {-2328.0f, -358.0f, -8.7f, 0.4f}) world.writeFloat(v);
    s.receive(world);
    REQUIRE(sent(Opcode::MSG_MOVE_WORLDPORT_ACK) == 1);
    CHECK(names() == 2);
    for (size_t i = 0; i < asked.size(); ++i) CHECK(sent(asked[i]) > before[i]);

    // What is answered is not.
    wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
    answer.writePackedGuid(kStranger);
    answer.writeUInt8(0);
    answer.writeString("Midai");
    answer.writeString("");
    for (int i = 0; i < 3; ++i) answer.writeUInt8(1);
    answer.writeUInt8(0);
    s.receive(answer);
    s.receive(world);
    CHECK(names() == 2);
    CHECK(gh.getCachedPlayerName(kStranger) == "Midai");
    gh.setWorldEntryCallback({});
}

// 0x0052af40: nothing of it was there.
TEST_CASE("five idle minutes sit down and go away, thirty log out", "[replay][playtest]") {
    using wowee::game::IdlePlayer;
    using wowee::game::idleSteps;
    IdlePlayer p{.idleMs = 299999, .standing = true};
    CHECK_FALSE(idleSteps(p).sit);
    CHECK_FALSE(idleSteps(p).away);
    p.idleMs = 300000;
    CHECK(idleSteps(p).sit);
    CHECK(idleSteps(p).away);
    CHECK_FALSE(idleSteps(p).logout);
    for (bool IdlePlayer::*no : {&IdlePlayer::fighting, &IdlePlayer::mounted, &IdlePlayer::walking}) {
        IdlePlayer q = p;
        q.*no = true;
        CHECK_FALSE(idleSteps(q).sit);
        CHECK(idleSteps(q).away);
    }
    IdlePlayer seated = p;
    seated.standing = false;
    CHECK_FALSE(idleSteps(seated).sit);
    for (bool IdlePlayer::*no : {&IdlePlayer::onTaxi, &IdlePlayer::away}) {
        IdlePlayer q = p;
        q.*no = true;
        CHECK(idleSteps(q).sit);
        CHECK_FALSE(idleSteps(q).away);
    }
    p.idleMs = 1800000;
    CHECK(idleSteps(p).logout);
    CHECK_FALSE(idleSteps(p).sit);
    p.loggingOut = true;
    CHECK_FALSE(idleSteps(p).logout);
}

TEST_CASE("away ends with a step, and is the server's", "[replay][playtest]") {
    Session s;
    s.spawnPlayer({{UF::PLAYER_FLAGS, 0}});
    auto& gh = s.handler();
    const auto said = [&] { return s.sent().of(Opcode::CMSG_MESSAGECHAT).size(); };
    gh.toggleAfk();
    REQUIRE(gh.isAfk());
    const size_t before = said();
    s.handler().sendMovement(Opcode::MSG_MOVE_JUMP);
    CHECK(gh.isAfk());
    s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    CHECK_FALSE(gh.isAfk());
    CHECK(said() == before + 1);
    s.handler().sendMovement(Opcode::MSG_MOVE_START_STRAFE_LEFT);
    CHECK(said() == before + 1);

    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0x2}}));
    CHECK(gh.isAfk());
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0x4}}));
    CHECK_FALSE(gh.isAfk());
    CHECK(gh.isDnd());

    gh.logoutIdle();
    gh.logoutIdle();
    CHECK(s.sent().of(Opcode::CMSG_LOGOUT_REQUEST).size() == 1);
}

// A line came with a gossip alone, a race library's with a vendor, and none with the rest.
TEST_CASE("a unit says hello as a window opens with it, and farewell", "[replay][playtest]") {
    using NpcLine = wowee::game::GameHandler::NpcLine;
    Session s;
    spawnPlayer(s);
    auto& gh = s.handler();
    s.receive(UpdateObject().create(kGiver, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 10}}));
    const auto giver = gh.getEntityManager().getEntity(kGiver);
    REQUIRE(giver);
    (void)giver->takeBaseAnimationRequest();
    std::vector<NpcLine> said;
    gh.setNpcLineCallback([&](uint64_t guid, NpcLine line, const glm::vec3&) {
        CHECK(guid == kGiver);
        said.push_back(line);
    });
    gh.setInteractionObject(kGiver);
    CHECK(said == std::vector<NpcLine>{NpcLine::Hello});
    CHECK(giver->takeBaseAnimationRequest());
    // The next window with the same one is not another hello.
    gh.setInteractionObject(kGiver);
    CHECK(said.size() == 1);
    gh.setInteractionObject(0);
    CHECK(said == std::vector<NpcLine>{NpcLine::Hello, NpcLine::Farewell});
    CHECK(giver->takeBaseAnimationRequest());
    // The dead say nothing.
    s.receive(UpdateObject().values(kGiver, {{UF::UNIT_FIELD_HEALTH, 0}}));
    gh.setInteractionObject(kGiver);
    CHECK(said.size() == 2);
    gh.setNpcLineCallback({});
}

// Revived by the server between two maps, the player stayed dead for all that asked: no update
// of the old object ever said so (playtest T59).
TEST_CASE("a death is over where the player's new object lives", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto& gh = s.handler();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    REQUIRE(gh.isPlayerDead());
    const auto born = [&](uint32_t health, uint32_t flags) {
        s.receive(UpdateObject().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                        {{UF::UNIT_FIELD_HEALTH, health},
                                         {UF::UNIT_FIELD_MAXHEALTH, 100},
                                         {UF::PLAYER_FLAGS, flags}}));
    };
    SECTION("alive") {
        born(50, 0);
        CHECK_FALSE(gh.isPlayerDead());
        CHECK_FALSE(gh.isPlayerGhost());
    }
    SECTION("a ghost") {
        born(1, 0x10);
        CHECK(gh.isPlayerDead());
        CHECK(gh.isPlayerGhost());
        born(50, 0);
        CHECK_FALSE(gh.isPlayerGhost());
    }
    SECTION("still dead") {
        born(0, 0);
        CHECK(gh.isPlayerDead());
    }
}

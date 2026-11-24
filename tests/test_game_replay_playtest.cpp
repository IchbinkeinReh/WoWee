// What the playtest rounds found, through GameHandler's own dispatch. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/combat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/spell_handler.hpp"

#include <utility>

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
    s.receive(p);
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
    CHECK(s.handler().getInteractNpcGuid() == 0);
    wowee::network::Packet bank(wowee::game::wireOpcode(Opcode::SMSG_SHOW_BANK));
    bank.writeUInt64(kBanker);
    s.receive(bank);
    CHECK(s.handler().getInteractNpcGuid() == kBanker);
    s.handler().closeBank();
    CHECK(s.handler().getInteractNpcGuid() == 0);
    wowee::network::Packet auction(wowee::game::wireOpcode(Opcode::MSG_AUCTION_HELLO));
    auction.writeUInt64(kAuctioneer);
    auction.writeUInt32(1);
    auction.writeUInt8(1);
    s.receive(auction);
    CHECK(s.handler().getInteractNpcGuid() == kAuctioneer);
    s.handler().closeAuctionHouse();
    CHECK(s.handler().getInteractNpcGuid() == 0);
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

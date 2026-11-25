// What the playtest rounds found, through GameHandler's own dispatch. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include <optional>

#include "core/coordinates.hpp"
#include "game/game_object_type.hpp"
#include "game_replay.hpp"
#include "game/combat_handler.hpp"
#include "game/idle_steps.hpp"
#include "game/movement_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/spell_handler.hpp"

#include <array>
#include <algorithm>
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

// 0x00808200, 0x00809c70 -> 0x004cff30
TEST_CASE("a failed cast fizzles where the caster is", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    s.handler().spellNameCacheRef()[kFireball].schoolMask = 0x4;
    s.handler().getSpellHandler()->setResistanceSounds({0u, 1428u, 1424u, 1426u, 1425u, 1427u, 1428u});
    std::vector<std::pair<uint32_t, uint64_t>> heard;
    s.handler().setSoundAtUnitCallback([&](uint32_t sound, uint64_t unit) { heard.emplace_back(sound, unit); });
    const std::vector<std::pair<uint32_t, uint64_t>> own{{1424u, Session::kPlayerGuid}};
    const auto take = [&] { return std::exchange(heard, {}); };
    s.receive(castFailed(5, kFireball, kInProgress));
    CHECK(take() == own);
    s.receive(castFailed(6, kFireball, kInProgress));  // said once, but fizzled again
    CHECK(take() == own);
    s.receive(castFailed(7, kFireball, 0x1b));
    CHECK(take().empty());
    s.receive(spellFailure(Opcode::SMSG_SPELL_FAILURE, 4, kFireball, kInterrupted));
    CHECK(take() == own);
    s.receive(spellFailure(Opcode::SMSG_SPELL_FAILED_OTHER, 4, kFireball, kInterrupted));
    CHECK(take().empty());
    s.handler().getSpellHandler()->reportLocalCastFailure(kFireball, 0x0c);
    CHECK(take() == own);
}

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

// T161: the two hours (0x1c20) to hand a thing back run on.
TEST_CASE("how long a thing can be handed back", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kItem = 0x4000000000000123ull;
    CHECK(s.handler().itemRefundLeft(kItem) == 0);
    const auto answer = [&](uint32_t since) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ITEM_REFUND_INFO_RESPONSE));
        p.writeUInt64(kItem);
        for (int i = 0; i < 3 + 10; ++i) p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(since);
        s.receive(p);
    };
    answer(100);
    CHECK(s.handler().itemRefundLeft(kItem) == 7100);
    answer(7200);
    CHECK(s.handler().itemRefundLeft(kItem) == 0);
}

// T292: the state of a way out of a seat, written as the world was left, stayed.
TEST_CASE("a seat's state goes with the world", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    auto& roster = s.handler().vehicleRoster();
    roster.setSeatState(Session::kPlayerGuid, 4);
    CHECK(s.handler().heldBySeat(Session::kPlayerGuid));
    roster.forgetSeatStates();
    CHECK(roster.seatState(Session::kPlayerGuid) == 0);
    CHECK_FALSE(s.handler().heldBySeat(Session::kPlayerGuid));
}

// T293: 0x0051fde0: every window closes as its object is walked away from or goes, not four of them.
TEST_CASE("a window closes as its object is left", "[replay][playtest]") {
    constexpr uint64_t kBanker = 0xF130000CED000077ull;
    Session s;
    s.setConnected(true);
    s.handler().setFactionTemplates({{35u, {.id = 35, .faction = 31, .factionGroup = 1, .friendGroup = 1}}});
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100},
                   {UF::UNIT_FIELD_MAXHEALTH, 100},
                   {UF::UNIT_FIELD_FACTIONTEMPLATE, 35}});
    s.receive(replay::UpdateObject().create(kBanker, wowee::game::ObjectType::UNIT,
                                            {{UF::UNIT_FIELD_HEALTH, 100},
                                             {UF::UNIT_NPC_FLAGS, 0x20001u},
                                             {UF::UNIT_FIELD_FACTIONTEMPLATE, 35}}));
    const auto banker = s.handler().getEntityManager().getEntity(kBanker);
    REQUIRE(banker);
    const auto& me = s.handler().playerMovementInfo();
    const auto open = [&] {
        banker->setPosition(me.x + 3.0f, me.y, me.z, 0.0f);
        wowee::network::Packet bank(wowee::game::wireOpcode(Opcode::SMSG_SHOW_BANK));
        bank.writeUInt64(kBanker);
        s.receive(bank);
        REQUIRE(s.handler().isBankOpen());
    };
    open();
    s.handler().update(0.016f);
    CHECK(s.handler().isBankOpen());
    // Its bounding radius and four yards.
    banker->setPosition(me.x + 9.0f, me.y, me.z, 0.0f);
    s.handler().update(0.016f);
    CHECK_FALSE(s.handler().isBankOpen());
    // No longer one that is spoken to.
    open();
    s.receive(replay::UpdateObject().values(kBanker, {{UF::UNIT_FIELD_FLAGS, 0x2000000u}}));
    s.handler().update(0.016f);
    CHECK_FALSE(s.handler().isBankOpen());
    s.receive(replay::UpdateObject().values(kBanker, {{UF::UNIT_FIELD_FLAGS, 0u}}));
    // Gone.
    open();
    wowee::network::Packet gone(wowee::game::wireOpcode(Opcode::SMSG_DESTROY_OBJECT));
    gone.writeUInt64(kBanker);
    gone.writeUInt8(0);
    s.receive(gone);
    s.handler().update(0.016f);
    CHECK_FALSE(s.handler().isBankOpen());
}

// T294: 0x00524010: a ghost by a battleground's spirit guide asks it and is let go of by it.
TEST_CASE("a ghost takes the spirit guide it comes to", "[replay][playtest]") {
    constexpr uint64_t kGuide = 0xF1300033B7000078ull;
    Session s;
    s.handler().setFactionTemplates({{35u, {.id = 35, .faction = 31, .factionGroup = 1, .friendGroup = 1}}});
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 1},
                   {UF::UNIT_FIELD_MAXHEALTH, 100},
                   {UF::UNIT_FIELD_FACTIONTEMPLATE, 35},
                   {UF::PLAYER_FLAGS, 0x10u}});
    s.receive(replay::UpdateObject().create(kGuide, wowee::game::ObjectType::UNIT,
                                            {{UF::UNIT_FIELD_HEALTH, 100},
                                             {UF::UNIT_NPC_FLAGS, 0x8001u},
                                             {UF::UNIT_FIELD_FACTIONTEMPLATE, 35}}));
    auto& gh = s.handler();
    const auto guide = gh.getEntityManager().getEntity(kGuide);
    REQUIRE(guide);
    const auto& me = gh.playerMovementInfo();
    const auto asked = [&] { return s.sent().of(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY).size(); };
    const auto cancelled = [&] { return s.sent().of(Opcode::CMSG_CANCEL_AURA).size(); };
    const auto at = [&](float yards) {
        guide->setPosition(me.x + yards, me.y, me.z, 0.0f);
        gh.stepSpiritGuide();
    };
    REQUIRE(gh.isPlayerGhost());
    REQUIRE(gh.unitMayAssist(Session::kPlayerGuid, kGuide));
    at(21.0f);
    CHECK(asked() == 0);
    at(19.0f);
    CHECK(asked() == 1);
    CHECK(gh.getAreaSpiritHealerGuid() == kGuide);
    at(21.5f);
    CHECK(asked() == 1);
    // Its answer.
    int inRange = 0;
    wowee::network::Packet time(wowee::game::wireOpcode(Opcode::SMSG_AREA_SPIRIT_HEALER_TIME));
    time.writeUInt64(kGuide);
    time.writeUInt32(20000);
    gh.setAddonEventCallback([&](const std::string& e, const auto&) { inRange += e == "AREA_SPIRIT_HEALER_IN_RANGE"; });
    s.receive(time);
    CHECK(inRange == 1);
    CHECK(cancelled() == 0);
    at(22.5f);
    CHECK(gh.getAreaSpiritHealerGuid() == 0);
    CHECK(cancelled() == 1);
    // An answer of one the ghost is not at is nothing.
    s.receive(time);
    CHECK(inRange == 1);
    // Alive again.
    at(5.0f);
    CHECK(asked() == 2);
    s.receive(replay::UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0u}}));
    gh.stepSpiritGuide();
    CHECK(gh.getAreaSpiritHealerGuid() == 0);
    gh.setAddonEventCallback({});
}

// T291: the teleport named the player and put the eye it moved there.
TEST_CASE("a teleport is the named unit's", "[replay][playtest]") {
    constexpr uint64_t kEye = 0xABull;
    Session s;
    spawnPlayer(s);
    s.receive(replay::UpdateObject().create(kEye, wowee::game::ObjectType::UNIT,
                                            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_FLAGS, 0x1000000u}}));
    auto& gh = s.handler();
    const auto eye = gh.getEntityManager().getEntity(kEye);
    REQUIRE(eye);
    eye->setPosition(500.0f, 600.0f, 70.0f, 0.0f);
    s.receive(replay::UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, static_cast<uint32_t>(kEye)}}));
    wowee::network::Packet control(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
    control.writePackedGuid(kEye);
    control.writeUInt8(1);
    s.receive(control);
    REQUIRE(gh.activeMover() == kEye);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt32(3);
    p.writeUInt32(0);
    p.writeUInt16(0);
    p.writeUInt32(1000);
    for (float f : {-100.0f, 200.0f, 30.0f, 1.0f}) p.writeFloat(f);
    p.writeUInt32(0);
    s.receive(p);
    CHECK(gh.activeMover() == Session::kPlayerGuid);
    const glm::vec3 there = wowee::core::coords::serverToCanonical(glm::vec3(-100.0f, 200.0f, 30.0f));
    CHECK(gh.playerMovementInfo().x == Catch::Approx(there.x));
    CHECK(gh.playerMovementInfo().y == Catch::Approx(there.y));
    CHECK(eye->getX() == Catch::Approx(500.0f));
    CHECK(eye->getY() == Catch::Approx(600.0f));
}

// What is read of a packet is counted: too little or too much of it.
TEST_CASE("packets read short or past their end are counted", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    const auto wire = wowee::game::wireOpcode(Opcode::SMSG_TIME_SYNC_REQ);
    const auto row = [&] {
        for (const auto& r : s.handler().packetReads().odd())
            if (r.opcode == wire) return r;
        return wowee::network::PacketReads::Row{};
    };
    const auto send = [&](int bytes) {
        wowee::network::Packet p(wire);
        for (int i = 0; i < bytes; ++i) p.writeUInt8(0);
        s.receive(p);
    };
    send(4);
    CHECK(row().packets == 0);
    send(7);
    CHECK(row().leftOver == 1);
    CHECK(row().mostLeftOver == 3);
    CHECK(row().past == 0);
    CHECK(row().packets == 2);

    wowee::network::PacketReads reads;
    reads.note(1, 0, false);
    reads.note(2, 9, false);
    reads.note(2, 4, false);
    reads.note(3, 0, true);
    const auto odd = reads.odd();
    REQUIRE(odd.size() == 2);
    CHECK(odd[0].opcode == 3);
    CHECK(odd[1].opcode == 2);
    CHECK(odd[1].mostLeftOver == 9);
    wowee::network::Packet shortOne(5);
    shortOne.writeUInt16(1);
    (void)shortOne.readUInt16();
    CHECK_FALSE(shortOne.readPastEnd());
    (void)shortOne.readUInt8();
    CHECK(shortOne.readPastEnd());
}

// T161: ITEM_FIELD_GIFTCREATOR, for a wrapped item's tooltip.
TEST_CASE("who wrapped a gift is known", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kGift = 0x4000000000000321ull;
    s.receive(UpdateObject().create(
        kGift, wowee::game::ObjectType::ITEM, {{UF::OBJECT_FIELD_ENTRY, 5043}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    CHECK(s.handler().getItemGiftCreatorByGuid(kGift) == 0);
    s.receive(UpdateObject().create(kGift, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 5043},
                                     {UF::ITEM_FIELD_STACK_COUNT, 1},
                                     {UF::ITEM_FIELD_GIFTCREATOR, 0x42}}));
    CHECK(s.handler().getItemGiftCreatorByGuid(kGift) == 0x42);
}

// A petition's answer is told by its number (0x005cea60); the guid in it is its owner's.
// Taken for the charter's, it was nobody's and the window did not open.
TEST_CASE("a charter's window opens with its petition", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kCharter = 0x4000000000000444ull;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    const auto shown = [&] { return std::count(events.begin(), events.end(), "PETITION_SHOW"); };
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_PETITION_SHOW_SIGNATURES));
    list.writeUInt64(kCharter);
    list.writeUInt64(Session::kPlayerGuid);
    list.writeUInt32(77);
    list.writeUInt8(0);
    s.receive(list);
    CHECK(s.sent().of(Opcode::CMSG_PETITION_QUERY).size() == 1);
    CHECK(shown() == 0);
    const auto answer = [&](uint32_t id, const char* name, uint32_t arena) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_PETITION_QUERY_RESPONSE));
        p.writeUInt32(id);
        p.writeUInt64(Session::kPlayerGuid);
        p.writeString(name);
        p.writeUInt8(0);
        p.writeUInt32(9);
        p.writeUInt32(9);
        p.writeUInt32(arena);
        s.receive(p);
    };
    answer(78, "Another", 0);
    CHECK(shown() == 0);
    answer(77, "The Seven", 0);
    CHECK(shown() == 1);
    CHECK(s.handler().getPetitionInfo().guildName == "The Seven");
    CHECK(s.handler().getPetitionInfo().ownerGuid == Session::kPlayerGuid);

    // And its tooltip (T161): asked for once, then what it is.
    wowee::network::Packet item(wowee::game::wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
    item.writeUInt32(5863);
    for (int i = 0; i < 3; ++i) item.writeUInt32(0);
    item.writeString("Guild Charter");
    for (int i = 0; i < 3; ++i) item.writeString("");
    for (int i = 0; i < 160; ++i) item.writeUInt32(i == 2 ? 0x2000u : 0u);
    s.receive(item);
    auto& held = s.handler().onlineItemsRef()[kCharter];
    held.entry = 5863;
    held.permanentEnchantId = 79;
    held.signatures = 3;
    const size_t asked = s.sent().of(Opcode::CMSG_PETITION_QUERY).size();
    CHECK_FALSE(s.handler().charterFacts(kCharter, 5863));
    CHECK_FALSE(s.handler().charterFacts(kCharter, 5863));
    CHECK(s.sent().of(Opcode::CMSG_PETITION_QUERY).size() == asked + 1);
    answer(79, "Two of us", 2);
    const auto facts = s.handler().charterFacts(kCharter, 5863);
    REQUIRE(facts);
    CHECK(facts->title == "Two of us");
    CHECK(facts->arena);
    CHECK(facts->signatures == 3);
    CHECK(facts->ownerGuid == Session::kPlayerGuid);
    CHECK_FALSE(s.handler().charterFacts(kCharter, 5043));
    s.handler().setAddonEventCallback({});
}

// T279: 0x00741b60 -> 0x00740d30, case 0xc5: no counter before the movement.
TEST_CASE("another unit is where it is teleported to", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    constexpr uint64_t kBoss = 0xF1300010B3000036ull;
    s.receive(UpdateObject().create(kBoss, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    float movedTo[3] = {};
    uint32_t took = 99;
    s.handler().setCreatureMoveCallback([&](uint64_t guid, float x, float y, float z, uint32_t ms) {
        if (guid != kBoss) return;
        movedTo[0] = x;
        movedTo[1] = y;
        movedTo[2] = z;
        took = ms;
    });
    for (const uint32_t flags : {0u, static_cast<uint32_t>(MovementFlags::FORWARD)}) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT));
        p.writePackedGuid(kBoss);
        p.writeUInt32(flags);
        p.writeUInt16(0);
        p.writeUInt32(1000);
        p.writeFloat(-76.75f);
        p.writeFloat(2152.5f);
        p.writeFloat(155.75f);
        p.writeFloat(1.5f);
        p.writeUInt32(0);   // fall time
        s.receive(p);
        const auto boss = s.handler().getEntityManager().getEntity(kBoss);
        REQUIRE(boss != nullptr);
        const glm::vec3 at = wowee::core::coords::serverToCanonical(glm::vec3(-76.75f, 2152.5f, 155.75f));
        CHECK(boss->getX() == at.x);
        CHECK(boss->getY() == at.y);
        CHECK(boss->getZ() == at.z);
        CHECK(movedTo[0] == at.x);
        CHECK(movedTo[1] == at.y);
        CHECK(took == 0);
        // From somewhere else once more.
        boss->setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    }
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
    // Not while a teleport waits for its answer (T280).
    h.setPosition(-340.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    h.setWorldEntryCallback([&](uint32_t, float, float, float, bool) { movement->holdTeleportAck(); });
    wowee::network::Packet teleport(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    teleport.writePackedGuid(Session::kPlayerGuid);
    teleport.writeUInt32(7);
    teleport.writeUInt32(0);
    teleport.writeUInt16(0);
    teleport.writeUInt32(1000);
    const glm::vec3 wire = wowee::core::coords::canonicalToServer(glm::vec3(-301.0f, -2300.0f, 10.0f));
    teleport.writeFloat(wire.x);
    teleport.writeFloat(wire.y);
    teleport.writeFloat(wire.z);
    teleport.writeFloat(1.0f);
    teleport.writeUInt32(0);
    s.receive(teleport);
    h.setWorldEntryCallback({});
    movement->checkAreaTriggers();
    CHECK(told() == 2);
    CHECK(movement->releaseTeleportAck());
    movement->checkAreaTriggers();
    CHECK(told() == 3);
    // Not the dead.
    h.setPosition(-340.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    h.setPosition(-301.0f, -2300.0f, 10.0f);
    movement->checkAreaTriggers();
    CHECK(told() == 3);
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

// V304: 0x005e7370 keeps every area with its map; they went as the map changed, and no
// quest was asked for again.
TEST_CASE("a quest's places are kept over a change of map", "[replay][playtest]") {
    Session s;
    spawnPlayer(s);
    const auto places = [&] { return s.handler().getGossipPois().size(); };
    const auto answer = [&] {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUEST_POI_QUERY_RESPONSE));
        p.writeUInt32(1);
        p.writeUInt32(743);
        p.writeUInt32(2);
        for (const uint32_t map : {1u, 530u}) {
            p.writeUInt32(map);   // the point's number
            p.writeUInt32(0);
            p.writeUInt32(map);
            p.writeUInt32(9);
            p.writeUInt32(0);
            p.writeUInt32(0);
            p.writeUInt32(0);
            p.writeUInt32(3);
            for (const int32_t v : {0, 0, 100, 0, 100, 100}) p.writeUInt32(static_cast<uint32_t>(v));
        }
        s.receive(p);
    };
    answer();
    REQUIRE(places() == 2);
    CHECK(s.handler().getGossipPois()[0].mapId == 1);
    CHECK(s.handler().getGossipPois()[1].mapId == 530);
    // A gossip's point, of the same number as it happens.
    wowee::network::Packet point(wowee::game::wireOpcode(Opcode::SMSG_GOSSIP_POI));
    point.writeUInt32(0);
    point.writeFloat(1.0f);
    point.writeFloat(2.0f);
    point.writeUInt32(7);
    point.writeUInt32(743);
    point.writeString("The bank");
    s.receive(point);
    REQUIRE(s.handler().gossipPoint());
    answer();
    CHECK(places() == 3);
    CHECK(s.handler().gossipPoint());

    s.handler().setWorldEntryCallback([](uint32_t, float, float, float, bool) {});
    wowee::network::Packet world(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    world.writeUInt32(530);
    for (const float v : {-1863.0f, 5430.0f, -8.7f, 0.4f}) world.writeFloat(v);
    s.receive(world);
    s.handler().setWorldEntryCallback({});
    CHECK(places() == 2);
    CHECK_FALSE(s.handler().gossipPoint());
    // With the character they go.
    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_LOGOUT_COMPLETE)));
    CHECK(places() == 0);
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

// A forced change answered while the player's own unit is not there yet went out
// with the movement of none: 0/0/0 facing 1.5707964, where the server then had
// the character until it moved. 0x00732450 looks the unit up for every one of
// them and leaves the packet be where there is none.
TEST_CASE("a forced movement change is answered by a unit that is there", "[replay][playtest]") {
    struct Forced {
        Opcode op;
        Opcode ack;
        std::vector<float> floats;
        bool extraByte = false;
    };
    const Forced forced = GENERATE(
        Forced{Opcode::SMSG_FORCE_RUN_SPEED_CHANGE, Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, {8.05f}, true},
        Forced{Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE, Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, {4.7f}},
        Forced{Opcode::SMSG_FORCE_MOVE_ROOT, Opcode::CMSG_FORCE_MOVE_ROOT_ACK, {}},
        Forced{Opcode::SMSG_FORCE_MOVE_UNROOT, Opcode::CMSG_FORCE_MOVE_UNROOT_ACK, {}},
        Forced{Opcode::SMSG_MOVE_SET_CAN_FLY, Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, {}},
        Forced{Opcode::SMSG_MOVE_WATER_WALK, Opcode::CMSG_MOVE_WATER_WALK_ACK, {}},
        Forced{Opcode::SMSG_MOVE_SET_COLLISION_HGT, Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK, {2.0f}},
        Forced{Opcode::SMSG_MOVE_KNOCK_BACK, Opcode::CMSG_MOVE_KNOCK_BACK_ACK, {1.0f, 0.0f, 5.0f, -5.0f}});
    const auto packet = [&] {
        wowee::network::Packet p(wowee::game::wireOpcode(forced.op));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt32(7);
        if (forced.extraByte) p.writeUInt8(1);
        for (float f : forced.floats) p.writeFloat(f);
        return p;
    };
    Session s;
    s.receive(packet());
    CHECK(s.sent().of(forced.ack).empty());

    spawnPlayer(s);
    s.receive(packet());
    REQUIRE(s.sent().of(forced.ack).size() == 1);
    auto ack = s.sent().of(forced.ack).front();
    CHECK(ack.readPackedGuid() == Session::kPlayerGuid);
    CHECK(ack.readUInt32() == 7);
    ack.readUInt32();
    ack.readUInt16();
    ack.readUInt32();
    CHECK(ack.readFloat() == -2337.2f);
    CHECK(ack.readFloat() == -356.4f);
    CHECK(ack.readFloat() == -8.7f);

    // Between two worlds there is none either (0x004d6750 took it away).
    wowee::network::Packet world(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    world.writeUInt32(1);
    for (float f : {-2328.0f, -358.0f, -8.7f, 0.4f}) world.writeFloat(f);
    s.receive(world);
    s.receive(packet());
    CHECK(s.sent().of(forced.ack).size() == 1);
}

// P49: a creature with a path of over 256 points took the rest of the packet
// with it.
TEST_CASE("a long walk in a create block loses nothing after it", "[replay][playtest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    const auto guid = [](uint32_t i) { return 0xF130004B00000000ull + i; };
    const glm::vec3 at(-1834.0f, 5299.0f, -12.4f);
    UpdateObject packet;
    uint32_t count = 0;
    const auto stands = [&] {
        ++count;
        packet.create(guid(count), wowee::game::ObjectType::UNIT,
                      {{UF::UNIT_FIELD_HEALTH, 10 + count}, {UF::UNIT_FIELD_MAXHEALTH, 500}});
    };
    const auto walks = [&](const std::vector<glm::vec3>& way, uint32_t flags, const glm::vec3& end) {
        ++count;
        packet.walking(guid(count), {{UF::UNIT_FIELD_HEALTH, 10 + count}, {UF::UNIT_FIELD_MAXHEALTH, 500}}, at, way,
                       flags, 1, end);
    };
    for (const uint32_t flags : {0x0u, 0x8000u, 0x10000u, 0x20000u}) {
        std::vector<glm::vec3> way;
        for (int i = 0; i < 300; ++i) way.push_back(at + glm::vec3(static_cast<float>(i), 0.0f, 0.0f));
        for (int i = 0; i < 20; ++i) stands();
        walks(way, flags, way.back());
    }
    // A cyclic one's end point is the origin.
    walks({at, at + glm::vec3(1.0f)}, 0x180000u, glm::vec3(0.0f));
    for (int i = 0; i < 20; ++i) stands();
    s.receive(packet);
    uint32_t there = 0;
    for (uint32_t i = 1; i <= count; ++i) {
        const auto unit = std::dynamic_pointer_cast<wowee::game::Unit>(s.handler().getEntityManager().getEntity(guid(i)));
        if (unit && unit->getHealth() == 10 + i) ++there;
    }
    CHECK(there == count);
}

// P47: the character window, the portraits and the dressing room dress their
// model from here, and 0x005975e0 duplicates the one in the world.
TEST_CASE("a helm or cloak not shown is not on the player's model anywhere", "[replay][playtest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FLAGS, 0}});
    const auto wear = [&](wowee::game::EquipSlot slot, uint32_t display, uint8_t type) {
        wowee::game::ItemDef item;
        item.itemId = display;
        item.displayInfoId = display;
        item.inventoryType = type;
        s.handler().getInventory().setEquipSlot(slot, item);
    };
    wear(wowee::game::EquipSlot::HEAD, 501, 1);
    wear(wowee::game::EquipSlot::BACK, 502, 16);
    wear(wowee::game::EquipSlot::CHEST, 503, 5);
    const auto shown = [&](int slot) {
        std::array<uint32_t, 19> ids{};
        std::array<uint8_t, 19> types{};
        REQUIRE(s.handler().getOtherPlayerEquipment(Session::kPlayerGuid, ids, types));
        return ids[static_cast<size_t>(slot)];
    };
    CHECK(shown(0) == 501u);
    CHECK(shown(14) == 502u);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0x400}}));
    CHECK(shown(0) == 0u);
    CHECK(shown(14) == 502u);
    CHECK(shown(4) == 503u);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0xc00}}));
    CHECK(shown(14) == 0u);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0}}));
    CHECK(shown(0) == 501u);
    CHECK(shown(14) == 502u);
}

// P57: the character window asked the flag, the model in the world was not
// told: ShowCloak's 0x006e0ef0 and 0x006dd1b0 dress and undress it at once.
TEST_CASE("showing or hiding the helm or the cloak dresses the model anew", "[replay][playtest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FLAGS, 0}});
    auto& gh = s.handler();
    for (int i = 0; i < 2; ++i) {
        (void)gh.consumeOnlineEquipmentDirty();
        gh.toggleCloak();
        CHECK(gh.consumeOnlineEquipmentDirty());
        gh.toggleHelm();
        CHECK(gh.consumeOnlineEquipmentDirty());
    }
    CHECK(s.sent().of(Opcode::CMSG_SHOWING_CLOAK).size() == 2);
    CHECK(s.sent().of(Opcode::CMSG_SHOWING_HELM).size() == 2);
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

// Each was raised at once for the first token alone, and with an empty unit for one without any
// (playtest T60); the list was sorted and its first taken for the victim.
TEST_CASE("threat events come once a frame, for every token a unit has", "[replay][playtest]") {
    constexpr uint64_t kMob = 0xF130000001000009ull, kStranger = 0xF130000001000010ull;
    Session s;
    spawnPlayer(s);
    auto& gh = s.handler();
    for (const uint64_t guid : {kMob, kStranger})
        s.receive(UpdateObject().create(guid, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 10}}));
    gh.setTarget(kMob);
    gh.setFocus(kMob);
    std::vector<std::pair<std::string, std::string>> raised;
    gh.setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event.starts_with("UNIT_THREAT")) raised.emplace_back(event, args.empty() ? "-" : args[0]);
    });
    const auto threat = [&](uint64_t mob, uint64_t victim, std::vector<std::pair<uint64_t, uint32_t>> list) {
        wowee::network::Packet p(wowee::game::wireOpcode(victim ? Opcode::SMSG_HIGHEST_THREAT_UPDATE
                                                                : Opcode::SMSG_THREAT_UPDATE));
        p.writePackedGuid(mob);
        if (victim) p.writePackedGuid(victim);
        p.writeUInt32(static_cast<uint32_t>(list.size()));
        for (const auto& [guid, value] : list) {
            p.writePackedGuid(guid);
            p.writeUInt32(value);
        }
        s.receive(p);
    };
    // Another is the victim, the player has more threat than it: high, not tanking.
    threat(kMob, kGiver, {{kGiver, 100}, {Session::kPlayerGuid, 105}});
    threat(kMob, 0, {{Session::kPlayerGuid, 106}});
    threat(kStranger, kGiver, {{kGiver, 100}});
    CHECK(raised.empty());
    gh.updateCombatText(0.0f);
    const auto times = [&](const char* event, const char* unit) {
        return std::count(raised.begin(), raised.end(), std::pair<std::string, std::string>(event, unit));
    };
    CHECK(times("UNIT_THREAT_LIST_UPDATE", "target") == 1);
    CHECK(times("UNIT_THREAT_LIST_UPDATE", "focus") == 1);
    CHECK(times("UNIT_THREAT_SITUATION_UPDATE", "player") == 1);
    CHECK(times("UNIT_THREAT_LIST_UPDATE", "") == 0);
    CHECK(raised.size() == 3);
    const auto mine = gh.threatSituation(Session::kPlayerGuid, kMob);
    CHECK(mine.standing.status == 2);
    CHECK_FALSE(mine.standing.victim);
    CHECK(gh.threatSituation(kGiver, kMob).standing.status == 3);
    CHECK(gh.threatStatus(Session::kPlayerGuid) == 2);

    // Out of sight the list goes, and the player is told.
    raised.clear();
    wowee::network::Packet gone(wowee::game::wireOpcode(Opcode::SMSG_DESTROY_OBJECT));
    gone.writeUInt64(kMob);
    gone.writeUInt8(0);
    s.receive(gone);
    gh.updateCombatText(0.0f);
    CHECK(times("UNIT_THREAT_SITUATION_UPDATE", "player") == 1);
    CHECK(gh.threatStatus(Session::kPlayerGuid) == 0);
    gh.setAddonEventCallback({});
}

// 0x00740450: none was shown.
TEST_CASE("getting onto a mount puffs, as one in sight already does not", "[replay][playtest]") {
    using Effect = wowee::game::GameHandler::UnitEffect;
    Session s;
    spawnPlayer(s);
    auto& gh = s.handler();
    std::vector<uint64_t> puffed;
    gh.setUnitEffectCallback([&](uint64_t guid, Effect effect) {
        if (effect == Effect::MountPoof) puffed.push_back(guid);
    });
    s.receive(UpdateObject().create(kGiver, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 229}}));
    CHECK(puffed.empty());
    const auto rides = [&](uint64_t guid, uint32_t display) {
        s.receive(UpdateObject().values(guid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, display}}));
    };
    rides(kGiver, 229);
    CHECK(puffed.empty());
    rides(kGiver, 0);
    CHECK(puffed.empty());
    rides(kGiver, 230);
    rides(kGiver, 231);
    rides(Session::kPlayerGuid, 229);
    CHECK(puffed == std::vector<uint64_t>{kGiver, kGiver, Session::kPlayerGuid});
    gh.setUnitEffectCallback({});
}

namespace {
// SMSG_AURA_UPDATE of one slot on the player, its own; spell 0 clears it.
wowee::network::Packet playerAura(uint8_t slot, uint32_t spell) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(slot);
    p.writeUInt32(spell);
    if (spell != 0) {
        p.writeUInt8(0x01 | 0x08 | 0x10);
        p.writeUInt8(80);
        p.writeUInt8(0);
    }
    return p;
}
}  // namespace

// 0x00727760 and 0x00722090 -> 0x0057ea30.
TEST_CASE("the minimap hears of a tracking spell's aura", "[replay][tracking]") {
    constexpr uint32_t kHerbs = 2383, kMinerals = 2580, kOther = 1243;
    int updates = 0;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    for (const uint32_t spell : {kHerbs, kMinerals, kOther}) {
        auto& e = s.handler().spellNameCacheRef()[spell];
        e.name = "Spell";
        e.effectAuraIds[0] = spell == kOther ? 22 : 45;
        if (spell != kMinerals) s.handler().getSpellHandler()->addKnownSpell(spell);
    }
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        if (name == "MINIMAP_UPDATE_TRACKING") ++updates;
    });
    s.handler().setMinimapOtherTracking(2);
    s.receive(playerAura(0, kOther));
    CHECK(updates == 0);
    CHECK(s.handler().minimapOtherTracking() == 2);
    // One the player does not have is none of its tracking.
    s.receive(playerAura(1, kMinerals));
    CHECK(updates == 0);
    s.receive(playerAura(2, kHerbs));
    CHECK(updates == 1);
    CHECK(s.handler().minimapOtherTracking() == -1);
    // Sent again as it is, nothing is new.
    s.receive(playerAura(2, kHerbs));
    CHECK(updates == 1);
    // Gone, with the menu's entry chosen since left alone.
    s.handler().setMinimapOtherTracking(1);
    s.receive(playerAura(2, 0));
    CHECK(updates == 2);
    CHECK(s.handler().minimapOtherTracking() == 1);
    s.handler().setAddonEventCallback(nullptr);
}

// 0x00512ab0: what a unit's tooltip tells of it.
TEST_CASE("a unit's tooltip hears of its name and of its skin", "[replay][tooltip]") {
    constexpr uint64_t kWolf = 0xF130000ABC000001ull;
    std::vector<uint64_t> told;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().create(kWolf, wowee::game::ObjectType::UNIT,
                                    {{UF::OBJECT_FIELD_ENTRY, 2748},
                                     {UF::UNIT_FIELD_HEALTH, 50}, {UF::UNIT_FIELD_MAXHEALTH, 50},
                                     {UF::UNIT_FIELD_FACTIONTEMPLATE, 32}, {UF::UNIT_FIELD_FLAGS, 0}}));
    s.handler().setUnitTooltipCallback([&](uint64_t guid) { told.push_back(guid); });
    s.receive(UpdateObject().values(kWolf, {{UF::UNIT_FIELD_HEALTH, 20}}));
    s.receive(UpdateObject().values(kWolf, {{UF::UNIT_FIELD_FACTIONTEMPLATE, 35}}));
    s.receive(UpdateObject().values(kWolf, {{UF::UNIT_FIELD_FLAGS, 0x80000}}));
    CHECK(told.empty());
    s.receive(UpdateObject().values(kWolf, {{UF::UNIT_FIELD_FLAGS, 0x4080000}}));
    CHECK(told == std::vector<uint64_t>{kWolf});
    // 0x0072cde0, 0x00728ca0: named.
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(2748);
    p.writeString("Archaedas");
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (uint32_t v : {0u, 5u, 0u, 1u}) p.writeUInt32(v);
    for (uint32_t v : {0u, 0u, 5988u, 0u, 0u, 0u}) p.writeUInt32(v);
    p.writeFloat(1.0f);
    p.writeFloat(1.0f);
    p.writeUInt8(0);
    for (int i = 0; i < 7; ++i) p.writeUInt32(0);
    s.receive(p);
    CHECK(told == std::vector<uint64_t>{kWolf, kWolf});
    s.handler().setUnitTooltipCallback(nullptr);
}

// 0x0053fad0.
TEST_CASE("a spell removed unlearns no companion", "[replay][companion]") {
    Session s;
    spawnPlayer(s);
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_REMOVED_SPELL));
    p.writeUInt32(133);
    s.receive(p);
    s.handler().setAddonEventCallback({});
    CHECK(std::count(events.begin(), events.end(), "SPELLS_CHANGED") == 1);
    CHECK(std::count(events.begin(), events.end(), "COMPANION_UNLEARNED") == 0);
    CHECK(std::count(events.begin(), events.end(), "COMPANION_UPDATE") == 0);
}

// The creature cache's row: +0x34, +0x38.
TEST_CASE("a creature's template says what it makes of health and power", "[replay][creature]") {
    Session s;
    spawnPlayer(s);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(416);
    p.writeString("Wichtel");
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (uint32_t v : {0u, 3u, 23u, 0u}) p.writeUInt32(v);   // flags, type, family, rank
    for (uint32_t v : {0u, 0u, 4449u, 0u, 0u, 0u}) p.writeUInt32(v);
    p.writeFloat(0.85f);
    p.writeFloat(1.25f);
    p.writeUInt8(0);
    for (int i = 0; i < 6; ++i) p.writeUInt32(0);
    p.writeUInt32(121);
    s.receive(p);
    const auto& creatures = s.handler().getCreatureInfoCache();
    REQUIRE(creatures.count(416) == 1);
    CHECK(creatures.at(416).healthModifier == 0.85f);
    CHECK(creatures.at(416).powerModifier == 1.25f);
    CHECK(creatures.at(416).movementId == 121);
}

TEST_CASE("a unit's stats are its own fields", "[replay][stats]") {
    constexpr uint64_t kImp = 0xF1400001A0000001ull;
    Session s;
    spawnPlayer(s);
    s.receive(UpdateObject().create(kImp, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_BYTES_0, 0x00000800}, {UF::UNIT_FIELD_LEVEL, 80},
                                     {UF::UNIT_FIELD_STAT0, 1}, {UF::UNIT_FIELD_STAT1, 79},
                                     {UF::UNIT_FIELD_STAT2, 2}, {UF::UNIT_FIELD_STAT3, 479},
                                     {UF::UNIT_FIELD_STAT4, 367}}));
    const auto* imp = s.handler().getUnitByGuid(kImp);
    REQUIRE(imp);
    const auto stats = wowee::game::GameHandler::statsOf(*imp);
    CHECK(stats.classId == 8);
    CHECK(stats.level == 80);
    CHECK(stats.agility == 79);
    CHECK(stats.intellect == 479);
    CHECK(stats.spirit == 367);
}

// 0x00614190, 0x00740570, 0x00724d90: each token of the unit hears of it.
TEST_CASE("a unit's tokens hear of its fields", "[replay][fieldevents]") {
    constexpr uint64_t kWolf = 0xF130000ABC000001ull;
    Session s;
    spawnPlayer(s);
    s.receive(UpdateObject().create(kWolf, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 50}, {UF::UNIT_FIELD_MAXHEALTH, 50},
                                     {UF::UNIT_FIELD_FACTIONTEMPLATE, 32}, {UF::UNIT_FIELD_FLAGS, 0}}));
    s.handler().setTarget(kWolf);
    s.handler().setFocus(kWolf);
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name.rfind("UNIT_", 0) == 0) events.push_back(name + (args.empty() ? "" : " " + args[0]));
    });
    const auto heard = [&](UF field, uint32_t value) {
        events.clear();
        s.receive(UpdateObject().values(kWolf, {{field, value}}));
        std::sort(events.begin(), events.end());
        std::string out;
        for (const auto& e : events) out += e + ";";
        return out;
    };
    CHECK(heard(UF::UNIT_FIELD_HEALTH, 20) == "UNIT_HEALTH focus;UNIT_HEALTH target;");
    CHECK(heard(UF::UNIT_FIELD_HEALTH, 20).empty());
    CHECK(heard(UF::UNIT_FIELD_FACTIONTEMPLATE, 35) == "UNIT_FACTION focus;UNIT_FACTION target;");
    CHECK(heard(UF::UNIT_FIELD_FLAGS, 0x80000) == "UNIT_FLAGS focus;UNIT_FLAGS target;");
    CHECK(heard(UF::UNIT_DYNAMIC_FLAGS, 4) ==
          "UNIT_DYNAMIC_FLAGS focus;UNIT_DYNAMIC_FLAGS target;UNIT_FACTION focus;UNIT_FACTION target;");
    // 0x006143f0
    events.clear();
    wowee::network::Packet aura(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
    aura.writePackedGuid(kWolf);
    aura.writeUInt8(0);
    aura.writeUInt32(0);
    s.receive(aura);
    CHECK(std::count(events.begin(), events.end(), "UNIT_AURA target") == 1);
    CHECK(std::count(events.begin(), events.end(), "UNIT_AURA focus") == 1);
    s.handler().setAddonEventCallback({});
}

// 0x005c4ef0
TEST_CASE("the socketing window hears of the gems that went in", "[replay][fieldevents]") {
    constexpr uint64_t kHelm = 0x4000000000000777ull;
    Session s;
    spawnPlayer(s);
    s.receive(UpdateObject().create(kHelm, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 39496}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    REQUIRE(s.handler().onlineItemsRef().count(kHelm) == 1);
    int updates = 0;
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        if (name == "SOCKET_INFO_UPDATE") ++updates;
    });
    // ITEM_FIELD_ENCHANTMENT_1_1, eight on from the stack's count; the first socket's
    // is the third enchantment.
    const auto socket = static_cast<uint16_t>(wowee::game::fieldIndex(UF::ITEM_FIELD_STACK_COUNT) + 8 + 6);
    s.receive(UpdateObject().valuesAt(kHelm, {{socket, 3621}}));
    CHECK(updates == 0);
    s.handler().openSocketing(kHelm);
    updates = 0;
    s.receive(UpdateObject().valuesAt(kHelm, {{socket, 3448}}));
    CHECK(updates == 1);
    s.receive(UpdateObject().valuesAt(kHelm, {{socket, 3448}}));
    CHECK(updates == 1);
    s.handler().setAddonEventCallback({});
}

// 0x006334f0: all that is there, each frame - and behind a new world, once it has been read in.
TEST_CASE("a frame handles every packet there is", "[replay][playtest]") {
    using Access = wowee::game::GameHandlerTestAccess;
    Session s;
    spawnPlayer(s);
    for (uint32_t i = 0; i < 300; ++i)
        Access::queue(s.handler(), UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 100 + i}}).packet());
    Access::frame(s.handler());
    CHECK(Access::queued(s.handler()) == 0);
    const auto me = std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
    REQUIRE(me);
    CHECK(me->getHealth() == 399);

    bool pending = true;
    s.handler().setWorldEntryPendingQuery([&pending] { return pending; });
    Access::queue(s.handler(), UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 50}}).packet());
    Access::frame(s.handler());
    CHECK(Access::queued(s.handler()) == 1);
    CHECK(me->getHealth() == 399);
    pending = false;
    Access::frame(s.handler());
    CHECK(Access::queued(s.handler()) == 0);
    CHECK(me->getHealth() == 50);
}

// P60: a chair that became the player's kept its old name on the target frame.
TEST_CASE("a pet's name is told by every token it has", "[replay][playtest]") {
    constexpr uint64_t kPet = 0xF15000880700042Aull;
    Session s;
    spawnPlayer(s);
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    auto& gh = s.handler();
    gh.setTarget(kPet);
    gh.setFocus(kPet);
    gh.getSpellHandler()->requestPetName(kPet);
    REQUIRE_FALSE(s.sent().of(Opcode::CMSG_PET_NAME_QUERY).empty());
    const uint32_t key = s.sent().of(Opcode::CMSG_PET_NAME_QUERY).back().readUInt32();
    std::vector<std::string> told;
    gh.setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "UNIT_NAME_UPDATE") told.push_back(args.empty() ? "-" : args[0]);
    });
    wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::SMSG_PET_NAME_QUERY_RESPONSE));
    answer.writeUInt32(key);
    answer.writeString("The Cranberry Chair");
    answer.writeUInt32(0);
    answer.writeUInt8(0);
    s.receive(answer);
    CHECK(std::count(told.begin(), told.end(), "target") == 1);
    CHECK(std::count(told.begin(), told.end(), "focus") == 1);
    CHECK(told.size() == 2);
    gh.setAddonEventCallback({});
}

// T271: 0x0072a000 asks the pet's number first; 0x0072cde0 overwrites nothing.
TEST_CASE("a pet keeps its own name when its creature's comes after", "[replay][playtest]") {
    constexpr uint64_t kPet = 0xF14000000600008Cull;
    Session s;
    spawnPlayer(s);
    const auto create = [&] {
        s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                        {{UF::OBJECT_FIELD_ENTRY, 416}, {UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    };
    create();
    auto& gh = s.handler();
    const auto name = [&] {
        const auto unit = std::dynamic_pointer_cast<wowee::game::Unit>(gh.getEntityManager().getEntity(kPet));
        return unit ? unit->getName() : std::string("?");
    };
    gh.getSpellHandler()->requestPetName(kPet);
    REQUIRE_FALSE(s.sent().of(Opcode::CMSG_PET_NAME_QUERY).empty());
    wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::SMSG_PET_NAME_QUERY_RESPONSE));
    answer.writeUInt32(s.sent().of(Opcode::CMSG_PET_NAME_QUERY).back().readUInt32());
    answer.writeString("Karuri");
    answer.writeUInt32(0);
    answer.writeUInt8(0);
    s.receive(answer);
    CHECK(name() == "Karuri");
    wowee::network::Packet creature(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    creature.writeUInt32(416);
    creature.writeString("Wichtel");
    for (int i = 0; i < 5; ++i) creature.writeString("");
    for (uint32_t v : {0u, 3u, 23u, 0u}) creature.writeUInt32(v);
    for (uint32_t v : {0u, 0u, 4449u, 0u, 0u, 0u}) creature.writeUInt32(v);
    for (int i = 0; i < 12; ++i) creature.writeUInt32(0);
    s.receive(creature);
    CHECK(name() == "Karuri");
    // Seen again: its own at once.
    create();
    CHECK(name() == "Karuri");
}

// P59: the player stayed, for the game, where it got on, and stood there when it was let go;
// jumped onto it, it fell on from there.
TEST_CASE("the player in a vehicle's seat is its place there, and is let go at it", "[replay][playtest]") {
    constexpr uint64_t kChair = 0xF15000880700042Aull;
    Session s;
    spawnPlayer(s);
    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    s.receive(UpdateObject().create(kChair, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    const auto chair = gh.getEntityManager().getEntity(kChair);
    REQUIRE(chair);
    chair->setPosition(gh.movementInfoRef().x + 5.0f, gh.movementInfoRef().y + 2.0f, gh.movementInfoRef().z,
                       wowee::core::coords::serverToCanonicalYaw(0.0f));
    auto& move = gh.movementInfoRef();
    move.flags |= static_cast<uint32_t>(wowee::game::MovementFlags::FALLING);
    move.z += 1.5f;
    // SMSG_MONSTER_MOVE_TRANSPORT to seat 0: a yard ahead on the chair, half a yard up.
    wowee::network::Packet in(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT));
    in.writePackedGuid(Session::kPlayerGuid);
    in.writePackedGuid(kChair);
    in.writeUInt8(0);
    in.writeUInt8(0);
    for (float v : {1.0f, 0.0f, 0.5f}) in.writeFloat(v);
    in.writeUInt32(1);
    in.writeUInt8(1);
    s.receive(in);
    CHECK((gh.movementInfoRef().flags & static_cast<uint32_t>(wowee::game::MovementFlags::FALLING)) == 0);
    const auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    const auto isAt = [&](const glm::vec3& place) {
        CHECK(gh.movementInfoRef().x == Catch::Approx(place.x).margin(0.01f));
        CHECK(gh.movementInfoRef().y == Catch::Approx(place.y).margin(0.01f));
        CHECK(gh.movementInfoRef().z == Catch::Approx(place.z).margin(0.01f));
        CHECK(self->getX() == Catch::Approx(place.x).margin(0.01f));
        CHECK(self->getY() == Catch::Approx(place.y).margin(0.01f));
    };
    const auto seatNow = [&] { return glm::vec3(chair->getX(), chair->getY() + 1.0f, chair->getZ() + 0.5f); };
    mh->updateAttachedTransportChildren(0.016f);
    isAt(seatNow());
    chair->setPosition(chair->getX() + 30.0f, chair->getY(), chair->getZ() + 4.0f, chair->getOrientation());
    mh->updateAttachedTransportChildren(0.016f);
    isAt(seatNow());
    const auto seat = mh->seatPlace(Session::kPlayerGuid);
    REQUIRE(seat);
    CHECK(seat->x == Catch::Approx(seatNow().x));

    // Off it (0x006ec400): where it sat, in the world.
    const glm::vec3 last = seatNow();
    gh.vehicleRoster().leave(Session::kPlayerGuid);
    chair->setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    mh->updateAttachedTransportChildren(0.016f);
    isAt(last);
    CHECK_FALSE(mh->seatPlace(Session::kPlayerGuid));
}

// Logged in on one, the create block says where.
TEST_CASE("a player made in a vehicle's seat is there", "[replay][playtest]") {
    constexpr uint64_t kChair = 0xF15000880700042Aull;
    Session s;
    s.receive(UpdateObject().create(kChair, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    UpdateObject::Spline self;
    self.objectType = wowee::game::ObjectType::PLAYER;
    self.moveFlags = 0x200u;
    self.transport = kChair;
    self.onTransport = {1.0f, 0.0f, 0.5f};
    self.seat = 0;
    s.receive(UpdateObject().onSpline(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 10}}, self));
    const auto chair = s.handler().getEntityManager().getEntity(kChair);
    REQUIRE(chair);
    chair->setPosition(100.0f, 200.0f, 30.0f, wowee::core::coords::serverToCanonicalYaw(0.0f));
    s.handler().getMovementHandler()->updateAttachedTransportChildren(0.016f);
    CHECK(s.handler().movementInfoRef().x == Catch::Approx(100.0f).margin(0.01f));
    CHECK(s.handler().movementInfoRef().y == Catch::Approx(201.0f).margin(0.01f));
    CHECK(s.handler().movementInfoRef().z == Catch::Approx(30.5f).margin(0.01f));
}

// P79: the open doors of an instance were solid for the whole first visit: whether one is a door was
// asked of its template, which is not there yet. 0x00714250 takes the field's type.
TEST_CASE("a game object's type is its field's, with its template there or not", "[replay][gameobject]") {
    namespace game = wowee::game;
    Session s;
    game::GameObject door(0xF1100002C633ull);
    const uint16_t bytes1 = game::fieldIndex(UF::GAMEOBJECT_BYTES_1);
    REQUIRE(bytes1 != 0xFFFF);
    const auto collides = [&](uint32_t bytes, const game::GameObjectQueryResponseData* info) {
        door.setField(bytes1, bytes);
        const auto state = static_cast<uint8_t>(bytes & 0xFF);
        return game::stateCollides(game::gameObjectTypeOf(door, info), state,
                                   game::go_interaction::stateForChange(state, state));
    };
    // Animation progress, art kit, type, state.
    CHECK(collides(0xFF000000u, nullptr) == false);  // an open door
    CHECK(collides(0xFF000001u, nullptr) == true);   // a shut one
    CHECK(collides(0xFF000002u, nullptr) == false);  // a broken one
    CHECK(collides(0xFF002300u, nullptr) == false);  // an open trapdoor
    CHECK(collides(0xFF002301u, nullptr) == true);
    CHECK(collides(0xFF000300u, nullptr) == std::nullopt);  // an open chest is one to walk into
    // The template says nothing about it, whatever it holds.
    game::GameObjectQueryResponseData chest;
    chest.type = 3;
    CHECK(collides(0xFF000000u, &chest) == false);
    CHECK(game::gameObjectTypeOf(door, &chest) == 0);
}

// P79: a door that closes was in the way from the first frame of it. 0x0070d8d0 has it so in Closed alone,
// and Close (Rebuild, for a broken one) goes there as it has played (0x0070d7e0).
TEST_CASE("a closing door is in the way once it is shut", "[replay][gameobject]") {
    namespace game = wowee::game;
    namespace gi = wowee::game::go_interaction;
    for (const uint8_t from : {uint8_t{0}, uint8_t{2}}) {
        CAPTURE(from);
        const uint8_t closing = gi::stateForChange(from, 1);
        CHECK(game::stateCollides(0, 1, closing) == false);
        const auto shut = gi::stateAfter(closing);
        REQUIRE(shut);
        CHECK(game::stateCollides(0, 1, *shut) == true);
        CHECK(gi::collidesAfterEntering(0, *shut) == true);
    }
    // One that opens or breaks lets through at once.
    CHECK(game::stateCollides(0, 0, gi::stateForChange(1, 0)) == false);
    CHECK(game::stateCollides(0, 2, gi::stateForChange(1, 2)) == false);
    // Where it came into view shut, it is.
    CHECK(game::stateCollides(0, 1, gi::stateForChange(1, 1)) == true);
}

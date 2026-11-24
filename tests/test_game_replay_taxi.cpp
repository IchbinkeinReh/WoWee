// Flights through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/movement_handler.hpp"

#include <chrono>
#include <thread>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

void spawn(Session& s, uint32_t unitFlags) {
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}, {UF::UNIT_FIELD_FLAGS, unitFlags}});
    s.handler().setPosition(-4500.0f, 3500.0f, 150.0f);
}

void fly(Session& s) {
    auto* m = s.handler().getMovementHandler();
    m->onTaxiFlightRef() = true;
    m->taxiClientActiveRef() = true;
}

}  // namespace

// Put at another place (MSG_MOVE_TELEPORT_ACK), the character glided on along
// the way it had been flying.
TEST_CASE("a teleport ends the flight", "[replay][taxi]") {
    Session s;
    spawn(s, 0);
    fly(s);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt32(41);
    p.writeUInt32(0);
    p.writeUInt16(0);
    p.writeUInt32(1000);
    p.writeFloat(717.282f);
    p.writeFloat(6979.87f);
    p.writeFloat(-73.028f);
    p.writeFloat(0.0f);
    p.writeUInt32(0);
    s.receive(p);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK_FALSE(s.handler().getMovementHandler()->taxiClientActiveRef());
}

TEST_CASE("leaving the world ends the flight", "[replay][taxi]") {
    Session s;
    spawn(s, 0);
    fly(s);
    s.handler().disconnect();
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK_FALSE(s.handler().getMovementHandler()->taxiClientActiveRef());
}

namespace {

// SMSG_MONSTER_MOVE of the player's own along a curve in the air, every point
// of it whole (flags 0x42000).
wowee::network::Packet flightSpline(uint32_t splineId, uint32_t durationMs) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(0);
    p.writeFloat(3500.0f);
    p.writeFloat(-4500.0f);
    p.writeFloat(150.0f);
    p.writeUInt32(splineId);
    p.writeUInt8(0);
    p.writeUInt32(0x42000);
    p.writeUInt32(durationMs);
    p.writeUInt32(3);
    for (int i = 1; i <= 3; ++i) {
        p.writeFloat(3500.0f + 100.0f * static_cast<float>(i));
        p.writeFloat(-4500.0f);
        p.writeFloat(150.0f);
    }
    return p;
}

}  // namespace

// At the border of a map the server waits for the end of its spline to be told
// before it takes the flight to the other one; it was held back as movement.
TEST_CASE("a spline's end is told in flight too", "[replay][taxi]") {
    Session s;
    spawn(s, 0);
    fly(s);
    s.receive(flightSpline(77, 1));
    auto* m = s.handler().getMovementHandler();
    std::vector<wowee::network::Packet> sent;
    for (int i = 0; i < 200 && sent.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        m->updateMoverSplineDone();
        sent = s.sent().of(Opcode::CMSG_MOVE_SPLINE_DONE);
    }
    REQUIRE(sent.size() == 1);
    const auto& data = sent[0].getData();
    REQUIRE(data.size() >= 4);
    CHECK((data[data.size() - 4] | data[data.size() - 3] << 8) == 77);
}

// On the other map, and into the world in flight, the player flies the spline
// the server sends (UNIT_FLAG_TAXI_FLIGHT, 0x100000).
TEST_CASE("a spline sent in the air is flown", "[replay][taxi]") {
    Session s;
    spawn(s, 0x100000);
    auto* m = s.handler().getMovementHandler();
    REQUIRE_FALSE(m->taxiClientActiveRef());
    s.receive(flightSpline(78, 20000));
    m->updateClientTaxi(0.0f);
    CHECK(m->taxiClientActiveRef());
    CHECK(s.handler().isOnTaxiFlight());
    const auto& at = s.handler().getMovementInfo();
    CHECK(at.x == Catch::Approx(-4500.0f));
    CHECK(at.y == Catch::Approx(3500.0f));
    m->updateClientTaxi(1.0f);
    CHECK(at.y > 3510.0f);

    // One of the player's on the ground is not a flight.
    Session ground;
    spawn(ground, 0);
    ground.receive(flightSpline(79, 20000));
    ground.handler().getMovementHandler()->updateClientTaxi(0.0f);
    CHECK_FALSE(ground.handler().isOnTaxiFlight());
}

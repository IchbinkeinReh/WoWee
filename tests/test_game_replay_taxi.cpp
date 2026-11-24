// Flights through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/movement_handler.hpp"

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

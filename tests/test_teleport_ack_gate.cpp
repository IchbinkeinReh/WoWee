// When MSG_MOVE_TELEPORT_ACK goes out (TeleportAckGate): after the area
// about the new place is in, as 0x006ed0f0 sends it once 0x006ecf80 has
// returned, and at once for a teleport that loads nothing.
#include <catch_amalgamated.hpp>

#include "game/teleport_ack_gate.hpp"

using wowee::game::TeleportAckGate;

TEST_CASE("A teleport that loads nothing is answered when its handler is done", "[teleport_ack]") {
    TeleportAckGate gate;
    CHECK_FALSE(gate.arrive(7, 1234).has_value());
    CHECK(gate.pending());
    const auto ack = gate.settle();
    REQUIRE(ack.has_value());
    CHECK(ack->counter == 7);
    CHECK(ack->moveTime == 1234);
    CHECK_FALSE(gate.pending());
    // Nothing is left for the loader to let go.
    CHECK_FALSE(gate.release().has_value());
}

TEST_CASE("A teleport behind the loading screen is answered after the load", "[teleport_ack]") {
    TeleportAckGate gate;
    (void)gate.arrive(3, 99);
    gate.hold();  // the world-entry callback queued the area load
    CHECK_FALSE(gate.settle().has_value());
    CHECK(gate.held());
    const auto ack = gate.release();  // WorldLoader: the area is in
    REQUIRE(ack.has_value());
    CHECK(ack->counter == 3);
    CHECK(ack->moveTime == 99);
    CHECK_FALSE(gate.held());
    CHECK_FALSE(gate.release().has_value());
}

TEST_CASE("A hold left from a world transfer does not hold the next teleport", "[teleport_ack]") {
    TeleportAckGate gate;
    gate.hold();  // SMSG_NEW_WORLD's entry went through the same callback
    CHECK_FALSE(gate.held());
    (void)gate.arrive(1, 5);
    const auto ack = gate.settle();
    REQUIRE(ack.has_value());
    CHECK(ack->counter == 1);
}

TEST_CASE("An unanswered teleport goes before the next one", "[teleport_ack]") {
    TeleportAckGate gate;
    (void)gate.arrive(4, 10);
    gate.hold();
    const auto earlier = gate.arrive(5, 20);
    REQUIRE(earlier.has_value());
    CHECK(earlier->counter == 4);
    const auto ack = gate.settle();
    REQUIRE(ack.has_value());
    CHECK(ack->counter == 5);
}

TEST_CASE("A new session drops the last one's teleport", "[teleport_ack]") {
    TeleportAckGate gate;
    (void)gate.arrive(9, 1);
    gate.hold();
    gate.drop();
    CHECK_FALSE(gate.pending());
    CHECK_FALSE(gate.release().has_value());
}

// A passenger's facing on a transport, as the wire carries it.
//
// The movement block's transport orientation is the passenger's facing on the
// transport: the client keeps it as it comes (0x009872c0), sends it back as
// it is (0x00987140), and faces the passenger in the world the transport's way
// plus it (0x004f42a0). transport_local does the same both ways; these are
// movement blocks as the server writes them and a heartbeat as it goes out.
#include <catch_amalgamated.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/transport_local.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

using namespace wowee;
using game::transport_local::localFacing;
using game::transport_local::worldYaw;

namespace {

using Float4 = std::array<uint8_t, 4>;

// A living movement block on a transport (UPDATEFLAG_LIVING, ONTRANSPORT),
// with the world facing and the facing on the transport given as bytes.
std::vector<uint8_t> livingOnTransport(const std::vector<uint8_t>& packedTransport,
                                       const Float4& worldO, const Float4& transportO) {
    std::vector<uint8_t> b = {
        0x20, 0x00,                          // UPDATEFLAG_LIVING
        0x00, 0x02, 0x00, 0x00,              // ONTRANSPORT
        0x00, 0x00,                          // flags2
        0x40, 0xE2, 0x01, 0x00,              // time
        0x00, 0x00, 0xC8, 0x42,              // x 100
        0x00, 0x00, 0x48, 0x43,              // y 200
        0x00, 0x00, 0xA0, 0x40,              // z 5
    };
    b.insert(b.end(), worldO.begin(), worldO.end());
    b.insert(b.end(), packedTransport.begin(), packedTransport.end());
    const std::vector<uint8_t> offset = {
        0x00, 0x00, 0xC0, 0x3F,              // 1.5
        0x00, 0x00, 0x00, 0xC0,              // -2
        0x00, 0x00, 0xC8, 0x40,              // 6.25
    };
    b.insert(b.end(), offset.begin(), offset.end());
    b.insert(b.end(), transportO.begin(), transportO.end());
    const std::vector<uint8_t> tail = {
        0x10, 0x27, 0x00, 0x00,              // transport time
        0xFF,                                // no seat
        0x00, 0x00, 0x00, 0x00,              // fall time
    };
    b.insert(b.end(), tail.begin(), tail.end());
    for (int i = 0; i < 9; ++i) {            // speeds, 1.0 each
        b.insert(b.end(), {0x00, 0x00, 0x80, 0x3F});
    }
    return b;
}

game::UpdateBlock parse(const std::vector<uint8_t>& bytes) {
    network::Packet p(0, bytes);
    game::UpdateBlock block;
    REQUIRE(game::UpdateObjectParser::parseMovementBlock(p, block));
    REQUIRE(p.getRemainingSize() == 0u);
    REQUIRE(block.onTransport);
    return block;
}

// The facing a heartbeat sends on the transport, read back off its bytes.
float sentTransportO(uint64_t transportGuid, float transportFacing, float canonicalYaw) {
    game::MovementInfo info;
    info.flags = static_cast<uint32_t>(game::MovementFlags::ONTRANSPORT);
    info.transportGuid = transportGuid;
    info.transportO = localFacing(transportFacing, canonicalYaw);
    info.orientation = core::coords::canonicalToServerYaw(canonicalYaw);
    network::Packet p;
    game::MovementPacket::writeMovementPayload(p, info);
    // flags, flags2, time, x y z o, the packed guid, x y z, then o.
    network::Packet r(0, p.getData());
    r.readUInt32();
    r.readUInt16();
    r.readUInt32();
    for (int i = 0; i < 4; ++i) r.readFloat();
    CHECK(r.readPackedGuid() == transportGuid);
    for (int i = 0; i < 3; ++i) r.readFloat();
    return r.readFloat();
}

bool sameAngle(float a, float b) {
    return std::abs(core::coords::normalizeAngleRad(a - b)) < 1e-4f;
}

}  // namespace

TEST_CASE("a player on a ship faces the bow, the stern and port with it", "[transport_facing]") {
    // An MO_TRANSPORT ship (0x1FC0...14, packed C1 14 C0 1F) whose own facing
    // is pi/2. A hull is drawn bow at its model's -X and the client faces a
    // ship back down its route (0x007f82b0), so its facing points at the
    // stern: 0 on it is the stern, pi the bow, 3pi/2 port.
    const std::vector<uint8_t> ship = {0xC1, 0x14, 0xC0, 0x1F};
    const uint64_t shipGuid = 0x1FC0000000000014ull;
    const float shipFacing = core::coords::PI / 2.0f;

    struct Case {
        const char* name;
        Float4 worldO;      // what the server writes as the world facing
        Float4 transportO;  // and on the ship
        float local;
    };
    const Case cases[] = {
        {"stern", {0xDB, 0x0F, 0xC9, 0x3F}, {0x00, 0x00, 0x00, 0x00}, 0.0f},
        {"bow",   {0xE4, 0xCB, 0x96, 0x40}, {0xDB, 0x0F, 0x49, 0x40}, core::coords::PI},
        {"port",  {0x00, 0x00, 0x00, 0x00}, {0xE4, 0xCB, 0x96, 0x40}, 1.5f * core::coords::PI},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const game::UpdateBlock block = parse(livingOnTransport(ship, c.worldO, c.transportO));
        CHECK(block.transportGuid == shipGuid);
        CHECK(block.transportO == Catch::Approx(c.local));

        // In the world, the ship's facing plus the player's on it - the
        // world facing the server wrote beside it, not its negation nor a
        // quarter turn off it.
        const float yaw = worldYaw(shipFacing, block.transportO);
        CHECK(sameAngle(yaw, core::coords::serverToCanonicalYaw(block.orientation)));

        // And back out as it came.
        const float sent = sentTransportO(shipGuid, shipFacing, yaw);
        CHECK(sent >= 0.0f);
        CHECK(sent < core::coords::TWO_PI);
        CHECK(sameAngle(sent, block.transportO));
    }
}

TEST_CASE("a passenger turns with its ship", "[transport_facing]") {
    // The facing on the ship stays; the world one goes round with the ship.
    const float onDeck = core::coords::PI;  // the bow
    const float before = worldYaw(0.25f, onDeck);
    const float after = worldYaw(1.0f, onDeck);
    CHECK(sameAngle(after - before, 0.75f));
    CHECK(sameAngle(localFacing(1.0f, after), onDeck));
}

TEST_CASE("an NPC on an elevator faces the elevator's way plus its own", "[transport_facing]") {
    // A creature on a lift (a type 11 game object, 0xF110...0ABC, packed
    // C3 BC 0A 10 F1) placed at 1.2. The lift goes up and down its
    // TransportAnimation.dbc path without turning (0x007139e0), so its
    // facing is where it was placed. The creature is at 2.5 on it, 3.7 in the
    // world.
    const std::vector<uint8_t> lift = {0xC3, 0xBC, 0x0A, 0x10, 0xF1};
    const game::UpdateBlock block = parse(livingOnTransport(
        lift, {0xCD, 0xCC, 0x6C, 0x40}, {0x00, 0x00, 0x20, 0x40}));
    CHECK(block.transportGuid == 0xF110000000000ABCull);
    CHECK(block.transportO == Catch::Approx(2.5f));
    CHECK(block.transportX == Catch::Approx(1.5f));

    const float yaw = worldYaw(1.2f, block.transportO);
    CHECK(sameAngle(yaw, core::coords::serverToCanonicalYaw(3.7f)));
    // Taken negated, as it was, it faced 2.5 the other way on the lift.
    CHECK_FALSE(sameAngle(yaw, worldYaw(1.2f, -block.transportO)));
}

TEST_CASE("a transport the client cannot find adds nothing", "[transport_facing]") {
    // 0x0074b590 answers 0 for a transport it cannot dereference, so the
    // facing on it is the world one.
    CHECK(sameAngle(worldYaw(0.0f, 2.0f), core::coords::serverToCanonicalYaw(2.0f)));
    CHECK(sameAngle(localFacing(0.0f, core::coords::serverToCanonicalYaw(2.0f)), 2.0f));
}

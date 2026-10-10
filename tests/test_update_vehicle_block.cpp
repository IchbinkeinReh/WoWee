// SMSG_UPDATE_OBJECT's movement block as WotLK writes it for a vehicle and a
// passenger: the living block's transport with its seat byte (0x007489c0's
// seat), and UPDATEFLAG_VEHICLE's Vehicle.dbc row after the position
// (0x0074c750).
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

using wowee::game::UpdateBlock;
using wowee::game::UpdateObjectParser;
using wowee::network::Packet;

namespace {
void writeLiving(Packet& p, uint32_t moveFlags) {
    p.writeUInt32(moveFlags);
    p.writeUInt16(0);      // flags2
    p.writeUInt32(1234);   // time
    p.writeFloat(1.0f);
    p.writeFloat(2.0f);
    p.writeFloat(3.0f);
    p.writeFloat(0.25f);   // orientation
}
void writeTail(Packet& p) {
    p.writeUInt32(0);  // fall time
    for (int i = 0; i < 9; ++i) p.writeFloat(1.0f);  // speeds
}
}  // namespace

TEST_CASE("a passenger's living block carries its vehicle and seat", "[vehicle]") {
    Packet w;
    w.writeUInt16(0x0020);  // UPDATEFLAG_LIVING
    writeLiving(w, 0x00000200);  // ONTRANSPORT
    w.writePackedGuid(0xF150007000000042ull);
    w.writeFloat(0.5f);
    w.writeFloat(0.0f);
    w.writeFloat(1.5f);
    w.writeFloat(3.0f);   // transport orientation
    w.writeUInt32(99);    // transport time
    w.writeUInt8(2);      // seat
    writeTail(w);

    Packet r(0, w.getData());
    UpdateBlock block;
    REQUIRE(UpdateObjectParser::parseMovementBlock(r, block));
    CHECK(block.onTransport);
    CHECK(block.transportGuid == 0xF150007000000042ull);
    CHECK(block.transportSeat == 2);
    CHECK(block.transportO == Catch::Approx(3.0f));
    CHECK(block.vehicleId == 0u);
    CHECK(r.getRemainingSize() == 0u);
}

TEST_CASE("a vehicle's block names its Vehicle.dbc row", "[vehicle]") {
    Packet w;
    w.writeUInt16(0x0020 | 0x0080);  // LIVING | VEHICLE
    writeLiving(w, 0);
    writeTail(w);
    w.writeUInt32(36);     // vehicle id
    w.writeFloat(0.1f);    // aim adjustment

    Packet r(0, w.getData());
    UpdateBlock block;
    REQUIRE(UpdateObjectParser::parseMovementBlock(r, block));
    CHECK_FALSE(block.onTransport);
    CHECK(block.transportSeat == -1);
    CHECK(block.vehicleId == 36u);
    CHECK(r.getRemainingSize() == 0u);
}

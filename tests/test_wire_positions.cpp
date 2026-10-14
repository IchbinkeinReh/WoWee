// Places the server sends and the client sends back, as the wire has them.
//
// The wire's x and y are the canonical y and x (core/coordinates.hpp). Each
// of these took one side of that the wrong way: a gossip point, a minimap
// ping and a battleground's flag carriers were kept as they came and drawn as
// canonical; a GM ticket sent the canonical place as it was; and the minimap
// ping was read with a packed guid where the client reads a full one, so its
// two floats were never the ones the server wrote.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/battleground_flag.hpp"
#include "game/transport_local.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;

TEST_CASE("a gossip point is kept canonical", "[wire_positions]") {
    // SMSG_GOSSIP_POI (0x0058a870): flags, x, y, icon, importance, name -
    // a city guard's "the bank" in Stormwind, wire (-8932.69, 610.23).
    const std::vector<uint8_t> bytes = {
        0x63, 0x00, 0x00, 0x00,                          // flags
        0xC3, 0x92, 0x0B, 0xC6, 0xB8, 0x8E, 0x18, 0x44,  // x, y
        0x07, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,  // icon, importance
        0x53, 0x74, 0x6F, 0x72, 0x6D, 0x77, 0x69, 0x6E, 0x64, 0x20, 0x42, 0x61, 0x6E, 0x6B, 0x00};
    wowee::network::Packet p(0, bytes);
    GossipPoiData poi;
    REQUIRE(GossipPoiParser::parse(p, poi));
    CHECK(poi.flags == 0x63);
    CHECK(poi.canonicalX == Catch::Approx(610.23f));
    CHECK(poi.canonicalY == Catch::Approx(-8932.69f));
    CHECK(poi.icon == 7);
    CHECK(poi.importance == 6);
    CHECK(poi.name == "Stormwind Bank");
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("a minimap ping is a full guid and a place, kept canonical", "[wire_positions]") {
    // MSG_MINIMAP_PING (0x006e2e90 case 0x1d5): u64 sender, x, y.
    const std::vector<uint8_t> bytes = {
        0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x23, 0xCB, 0x44, 0x66, 0xE6, 0xC0, 0xC4};
    wowee::network::Packet p(0, bytes);
    MinimapPingData ping;
    REQUIRE(MinimapPingParser::parse(p, ping));
    CHECK(ping.senderGuid == 0x42);
    CHECK(ping.canonicalX == Catch::Approx(-1543.2f));
    CHECK(ping.canonicalY == Catch::Approx(1625.1f));
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("a battleground's positions are kept canonical, list by list", "[wire_positions]") {
    // MSG_BATTLEGROUND_PLAYER_POSITIONS (0x0054b3f0): no team positions, one
    // flag carrier at wire (1029.3, 1544.8).
    const std::vector<uint8_t> bytes = {
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9A, 0xA9, 0x80, 0x44, 0x9A, 0x19, 0xC1, 0x44};
    wowee::network::Packet p(0, bytes);
    const auto positions = BattlegroundPlayerPositionsParser::parse(p);
    REQUIRE(positions.size() == 1);
    CHECK(positions[0].guid == 7);
    CHECK(positions[0].group == 1);
    CHECK(positions[0].canonicalX == Catch::Approx(1544.8f));
    CHECK(positions[0].canonicalY == Catch::Approx(1029.3f));
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("one flag carrier is drawn with the flag its team holds", "[wire_positions]") {
    // The packet above: one carrier, guid 7. The client draws it as it draws
    // two (0x0054d010): by its own team where it is in sight - a Horde one
    // (an orc) holds the Alliance flag - else by the raid it is or is not in.
    namespace bf = wowee::game::bg_flag;
    constexpr uint32_t kWarsong = 489;
    CHECK(std::string(bf::texture(bf::carrierTeam(uint8_t{2}, 1, false), kWarsong)) == "AllianceFlag");
    CHECK(std::string(bf::texture(bf::carrierTeam(uint8_t{1}, 2, false), kWarsong)) == "HordeFlag");
    // Out of sight: a human's raid mate holds the Horde flag, anyone else
    // the Alliance one.
    CHECK(std::string(bf::texture(bf::carrierTeam(std::nullopt, 1, true), kWarsong)) == "HordeFlag");
    CHECK(std::string(bf::texture(bf::carrierTeam(std::nullopt, 1, false), kWarsong)) == "AllianceFlag");
    // Eye of the Storm names them the other way round.
    CHECK(std::string(bf::texture(bf::carrierTeam(uint8_t{2}, 1, false), 566)) == "HordeFlag");
    // ChrRaces: the goblin's faction template is an Alliance one.
    CHECK(bf::raceTeam(9) == 1);
    CHECK(bf::raceTeam(10) == 0);
    CHECK(bf::raceTeam(0) == -1);
    CHECK(bf::texture(-1, kWarsong) == nullptr);
}

TEST_CASE("a battleground's flag carriers are kept last first", "[wire_positions]") {
    // 0x0054b3f0 reads the carriers into its slots counting down: of two,
    // the one written first is the second. One team position, kept in order.
    const std::vector<uint8_t> bytes = {
        0x01, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40,    // (1, 2)
        0x02, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x80, 0x40,    // (3, 4)
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0xC0, 0x40};   // (5, 6)
    wowee::network::Packet p(0, bytes);
    const auto positions = BattlegroundPlayerPositionsParser::parse(p);
    REQUIRE(positions.size() == 3);
    CHECK(positions[0].guid == 5);
    CHECK(positions[0].group == 0);
    CHECK(positions[1].guid == 8);
    CHECK(positions[1].group == 1);
    CHECK(positions[1].canonicalX == Catch::Approx(6.0f));
    CHECK(positions[1].canonicalY == Catch::Approx(5.0f));
    CHECK(positions[2].guid == 7);
    CHECK(positions[2].group == 1);
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("a GM ticket sends the player's place as the wire has it", "[wire_positions]") {
    // 0x005ac8f0: the map, then the player's x, y, z.
    const auto pkt = GmTicketCreatePacket::build(1, -4325.5f, 15.25f, 5.5f, "help");
    const auto& d = pkt.getData();
    const std::vector<uint8_t> head(d.begin(), d.begin() + 16);
    CHECK(head == std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x74, 0x41,    // 15.25: the wire's x
                                       0x00, 0x2C, 0x87, 0xC5,    // -4325.5: its y
                                       0x00, 0x00, 0xB0, 0x40});  // 5.5
}

TEST_CASE("a passenger's offset goes back out as it came in", "[wire_positions]") {
    const glm::vec3 wire(3.0f, -7.5f, 2.25f);
    for (const bool wmo : {true, false}) {
        const glm::vec3 local = transport_local::fromWire(wmo, wire);
        const glm::vec3 back = transport_local::toWire(wmo, local);
        CHECK(back.x == wire.x);
        CHECK(back.y == wire.y);
        CHECK(back.z == wire.z);
    }
    // A WMO transport keeps the wire's axes; an M2 one a canonical delta.
    CHECK(transport_local::fromWire(true, wire).x == 3.0f);
    CHECK(transport_local::fromWire(false, wire).x == -7.5f);
}

TEST_CASE("a passenger on a turned lift is sent its place in the lift's frame", "[wire_positions]") {
    // The client's offset is the place put through the inverse of the lift's
    // matrix (0x004f47b0 -> 0x004c2300); a lift placed at pi/2 has its own +x
    // along the wire's +y. Standing two yards north of its middle - canonical
    // +x, the wire's +y - is two yards along the lift's +x.
    const float liftFacing = 1.5707964f;
    const glm::vec3 wire = transport_local::toWire(false, glm::vec3(2.0f, 0.0f, 0.5f), liftFacing);
    CHECK(wire.x == Catch::Approx(2.0f));
    CHECK(wire.y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(wire.z == 0.5f);

    // As a heartbeat writes it: flags (ONTRANSPORT), flags2, time, x y z o,
    // the lift's packed guid (0xF110000000000ABC), then the offset.
    MovementInfo info;
    info.flags = static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
    info.transportGuid = 0xF110000000000ABCull;
    info.transportX = wire.x;
    info.transportY = wire.y;
    info.transportZ = wire.z;
    wowee::network::Packet out;
    MovementPacket::writeMovementPayload(out, info);
    wowee::network::Packet r(0, out.getData());
    r.readUInt32();
    r.readUInt16();
    r.readUInt32();
    for (int i = 0; i < 4; ++i) r.readFloat();
    const std::vector<uint8_t> guid = {r.readUInt8(), r.readUInt8(), r.readUInt8(),
                                       r.readUInt8(), r.readUInt8()};
    CHECK(guid == std::vector<uint8_t>{0xC3, 0xBC, 0x0A, 0x10, 0xF1});
    CHECK(r.readFloat() == Catch::Approx(2.0f));
    CHECK(r.readFloat() == Catch::Approx(0.0f).margin(1e-5));
    CHECK(r.readFloat() == 0.5f);

    // An offset the server sends on the lift comes back out as it came.
    const glm::vec3 incoming(1.5f, -2.0f, 6.25f);
    const glm::vec3 local = transport_local::fromWire(false, incoming, 1.2f);
    const glm::vec3 back = transport_local::toWire(false, local, 1.2f);
    CHECK(back.x == Catch::Approx(incoming.x));
    CHECK(back.y == Catch::Approx(incoming.y));
    CHECK(back.z == incoming.z);
    // And it is the lift's turn of it in the world, not the wire's axes.
    CHECK(glm::length(local) == Catch::Approx(glm::length(incoming)));
    CHECK_FALSE(local.x == Catch::Approx(incoming.y));
}

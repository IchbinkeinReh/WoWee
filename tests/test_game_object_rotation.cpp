// A game object's rotation, as the update block carries it.
//
// UPDATEFLAG_ROTATION's 64 bits are the object's rotation, kept packed by the
// movement block's reader (0x004d3890) and taken as the object's own by
// 0x00714250. Its facing is that rotation's yaw (0x0070c310 -> 0x004f4630)
// and it is drawn by it (0x00712ee0). These are create blocks as a server
// writes them for a placed game object.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/game_object_rotation.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"
#include "rendering/placement_transform.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

using namespace wowee;
namespace gor = game::game_object_rotation;

namespace {

// LOWGUID | HIGHGUID | STATIONARY_POSITION | ROTATION, as a placed object's
// create block has it: x 100, y 200, z 5, orientation 1.5, the guid's two
// halves, then the packed rotation - read as the update reader reads it.
game::UpdateBlock placedObject(const std::vector<uint8_t>& packedRotation) {
    std::vector<uint8_t> bytes = {
        0x58, 0x02,                          // update flags
        0x00, 0x00, 0xC8, 0x42,              // x 100
        0x00, 0x00, 0x48, 0x43,              // y 200
        0x00, 0x00, 0xA0, 0x40,              // z 5
        0x00, 0x00, 0xC0, 0x3F,              // orientation 1.5
        0x2A, 0x00, 0x00, 0x00,              // low guid
        0x00, 0x00, 0x10, 0xF1,              // high guid
    };
    bytes.insert(bytes.end(), packedRotation.begin(), packedRotation.end());
    network::Packet packet(0, bytes);
    game::UpdateBlock block;
    REQUIRE(game::UpdateObjectParser::parseMovementBlock(packet, block));
    REQUIRE_FALSE(packet.hasData());
    return block;
}

bool sameAngle(float a, float b) {
    return std::abs(core::coords::normalizeAngleRad(a - b)) < 1e-4f;
}

}  // namespace

TEST_CASE("a placed object's rotation is read and unpacked as the client does", "[go_rotation]") {
    // Turned 1.5 about z: z = sin(0.75) in the low 21 bits.
    const game::UpdateBlock block = placedObject({0xFE, 0xE7, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00});
    REQUIRE(block.hasRotation);
    CHECK(block.packedRotation == 0xAE7FEull);
    CHECK(block.orientation == 1.5f);

    const glm::quat q = gor::unpack(block.packedRotation);
    CHECK(q.x == 0.0f);
    CHECK(q.y == 0.0f);
    CHECK(q.z == Catch::Approx(std::sin(0.75f)).margin(1e-6));
    CHECK(q.w == Catch::Approx(std::cos(0.75f)).margin(1e-5));
    CHECK(sameAngle(gor::facing(q), 1.5f));
    const glm::vec3 euler = gor::placementEuler(q);
    CHECK(euler.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(euler.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(sameAngle(euler.z, 1.5f));
}

TEST_CASE("a rotation past a half turn comes back with w kept positive", "[go_rotation]") {
    // Turned 4.0: w = cos(2) is negative, so the server negates the lot and
    // z comes as a negative 21-bit number.
    const game::UpdateBlock block = placedObject({0x85, 0x73, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00});
    const glm::quat q = gor::unpack(block.packedRotation);
    CHECK(q.z == Catch::Approx(-std::sin(2.0f)).margin(1e-6));
    CHECK(q.w > 0.0f);
    CHECK(sameAngle(gor::facing(q), 4.0f));
}

TEST_CASE("a tilted object is drawn tilted, not by its facing alone", "[go_rotation]") {
    // Turned 0.5 about z, then rolled 0.3 about its own x.
    const game::UpdateBlock block = placedObject({0xFC, 0xE9, 0xE3, 0xED, 0x12, 0x8C, 0x88, 0x12});
    const glm::quat q = gor::unpack(block.packedRotation);
    CHECK(q.x == Catch::Approx(0.144792f).margin(1e-5));
    CHECK(q.y == Catch::Approx(0.036972f).margin(1e-5));
    // Its facing is the turn about z; the roll does not move its +x.
    CHECK(sameAngle(gor::facing(q), 0.5f));

    // Drawn by the whole rotation: the placement angles rebuild it.
    const glm::vec3 euler = gor::placementEuler(q);
    CHECK(euler.x == Catch::Approx(0.3f).margin(1e-4));
    CHECK(euler.y == Catch::Approx(0.0f).margin(1e-4));
    CHECK(euler.z == Catch::Approx(0.5f).margin(1e-4));
    const glm::mat4 drawn = rendering::placementModelMatrix(glm::vec3(0.0f), euler, 1.0f);
    const glm::mat4 client = glm::mat4_cast(q);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) CHECK(drawn[c][r] == Catch::Approx(client[c][r]).margin(1e-4));
}

TEST_CASE("no rotation bits is no turn at all", "[go_rotation]") {
    // 0x00982340 on 0: x, y and z are 0, so w is 1.
    const glm::quat q = gor::unpack(0);
    CHECK(q.w == 1.0f);
    CHECK(gor::facing(q) == 0.0f);
}

TEST_CASE("a facing straight down an axis is the client's exact angle", "[go_rotation]") {
    // 0x004f4630's own answers where one side of the atan is under 2^-22.
    CHECK(gor::facing(gor::fromFacing(core::coords::PI)) == Catch::Approx(core::coords::PI));
    CHECK(gor::facing(gor::fromFacing(1.5f * core::coords::PI)) == Catch::Approx(1.5f * core::coords::PI));
    CHECK(gor::facing(gor::fromFacing(0.5f * core::coords::PI)) == Catch::Approx(0.5f * core::coords::PI));
}

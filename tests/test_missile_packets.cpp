// The siege missile's two reports, as the client writes them:
// CMSG_UPDATE_MISSILE_TRAJECTORY (0x006fd6b0) - the guid whole, the spell,
// pitch, speed, launch and impact, then the movement as MSG_MOVE_STOP
// (0x0071f060) - and CMSG_UPDATE_PROJECTILE_POSITION (0x00703640) - the
// caster's guid, the spell, the cast count and the point.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/packet_parsers.hpp"
#include "core/application.hpp"

#include <cstring>

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;
using Bytes = std::vector<uint8_t>;

namespace {
float floatAt(const Bytes& b, size_t at) {
    float f = 0.0f;
    std::memcpy(&f, b.data() + at, 4);
    return f;
}
uint32_t u32At(const Bytes& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
}  // namespace

TEST_CASE("the trajectory update names the caster whole, then the spell and the shot", "[missile]") {
    MovementInfo wire;
    wire.x = 1.0f;
    const Bytes b = UpdateMissileTrajectoryPacket::build(0xF150001234000056ull, 0xBEEF, 0.75f, 42.0f,
                                                         glm::vec3(1, 2, 3), glm::vec3(4, 5, 6),
                                                         0xF150001234000056ull, wire)
                        .getData();
    REQUIRE(b.size() > 48u);
    const Bytes guid{0x56, 0x00, 0x00, 0x34, 0x12, 0x00, 0x50, 0xF1};
    CHECK(Bytes(b.begin(), b.begin() + 8) == guid);
    CHECK(u32At(b, 8) == 0xBEEFu);
    CHECK(floatAt(b, 12) == 0.75f);
    CHECK(floatAt(b, 16) == 42.0f);
    CHECK(floatAt(b, 20) == 1.0f);
    CHECK(floatAt(b, 24) == 2.0f);
    CHECK(floatAt(b, 28) == 3.0f);
    CHECK(floatAt(b, 32) == 4.0f);
    CHECK(floatAt(b, 36) == 5.0f);
    CHECK(floatAt(b, 40) == 6.0f);
    // There is movement: the flag, the opcode, the mover packed, then the
    // movement block itself.
    CHECK(b[44] == 1);
    wowee::network::Packet movement(0);
    MovementPacket::writeMovementPayload(movement, wire);
    // Mask 0b11011001: five bytes of guid after it.
    const size_t movementAt = 44 + 1 + 4 + 6;
    REQUIRE(b.size() == movementAt + movement.getData().size());
    CHECK(Bytes(b.begin() + 49, b.begin() + 55) == Bytes{0xD9, 0x56, 0x34, 0x12, 0x50, 0xF1});
    CHECK(Bytes(b.begin() + movementAt, b.end()) == movement.getData());
}

TEST_CASE("the projectile position is the caster, spell, cast count and point", "[missile]") {
    const Bytes b =
        UpdateProjectilePositionPacket::build(0x0000000000ABCDEFull, 61234, 7, glm::vec3(10.5f, -3.0f, 99.25f))
            .getData();
    REQUIRE(b.size() == 8u + 4u + 1u + 12u);
    CHECK(Bytes(b.begin(), b.begin() + 8) == Bytes{0xEF, 0xCD, 0xAB, 0, 0, 0, 0, 0});
    CHECK(u32At(b, 8) == 61234u);
    CHECK(b[12] == 7);
    CHECK(floatAt(b, 13) == 10.5f);
    CHECK(floatAt(b, 17) == -3.0f);
    CHECK(floatAt(b, 21) == 99.25f);
}

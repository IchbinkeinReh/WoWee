// The tail of a 3.3.5a movement block - pitch, fall time, jump block, spline
// elevation - as another unit's movement packet carries it (FUN_004f4d40).
//
// The handler of other units' movement used to stop reading after the
// transport block, so it never knew how a unit was falling: a jump drew no
// arc, and a knockback was no jump at all. These pin what the reader takes
// and when, against the payload MovementPacket writes.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

// The packet layer reaches the Application singleton; nothing under test does,
// so it stays null. Same stub the other packet-layout tests carry.
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using wowee::game::MovementFlags;
using wowee::game::MovementInfo;
using wowee::game::MovementPacket;
using wowee::network::Packet;

namespace {

// flags (4), flags2 (2), time (4), position and facing (16).
constexpr size_t kHeadSize = 26;

constexpr uint32_t bit(MovementFlags f) { return static_cast<uint32_t>(f); }

// The tail read back from a payload written for info, with the head's flags.
bool readBack(const MovementInfo& written, MovementInfo& read) {
    Packet packet(0);
    MovementPacket::writeMovementPayload(packet, written);
    packet.setReadPos(kHeadSize);
    read.flags = written.flags;
    read.flags2 = written.flags2;
    const bool ok = MovementPacket::readPitchAndFall(packet, read);
    return ok && !packet.hasData();
}

}  // namespace

TEST_CASE("A falling unit's jump block is read", "[movement][fall]") {
    MovementInfo info;
    info.flags = bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING);
    info.fallTime = 250;
    info.jumpVelocity = -7.955547f;
    info.jumpSinAngle = 0.6f;
    info.jumpCosAngle = 0.8f;
    info.jumpXYSpeed = 7.0f;
    MovementInfo read;
    REQUIRE(readBack(info, read));
    CHECK(read.fallTime == 250);
    CHECK(read.jumpVelocity == -7.955547f);
    CHECK(read.jumpSinAngle == 0.6f);
    CHECK(read.jumpCosAngle == 0.8f);
    CHECK(read.jumpXYSpeed == 7.0f);
}

TEST_CASE("Not falling, there is a fall time and no jump block", "[movement][fall]") {
    MovementInfo info;
    info.flags = bit(MovementFlags::FORWARD);
    info.fallTime = 40;
    MovementInfo read;
    REQUIRE(readBack(info, read));
    CHECK(read.fallTime == 40);
    CHECK(read.jumpVelocity == 0.0f);
}

TEST_CASE("The pitch comes first while swimming", "[movement][fall]") {
    MovementInfo info;
    info.flags = bit(MovementFlags::SWIMMING) | bit(MovementFlags::FALLING);
    info.pitch = -0.5f;
    info.fallTime = 7;
    info.jumpVelocity = -9.096748f;
    MovementInfo read;
    REQUIRE(readBack(info, read));
    CHECK(read.pitch == -0.5f);
    CHECK(read.fallTime == 7);
    CHECK(read.jumpVelocity == -9.096748f);
}

TEST_CASE("Always allowed to pitch, the pitch is there on the ground", "[movement][fall]") {
    // MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING (0x20), which the writer does not
    // send but the client's reader takes.
    Packet packet(0);
    packet.writeFloat(0.25f);
    packet.writeUInt32(99);
    MovementInfo read;
    read.flags2 = 0x0020;
    REQUIRE(MovementPacket::readPitchAndFall(packet, read));
    CHECK(read.pitch == 0.25f);
    CHECK(read.fallTime == 99);
    CHECK_FALSE(packet.hasData());
}

TEST_CASE("A spline elevation after the jump block is read past", "[movement][fall]") {
    Packet packet(0);
    packet.writeUInt32(10);
    for (float f : {-3.0f, 1.0f, 0.0f, 2.5f}) packet.writeFloat(f);
    packet.writeFloat(1.5f);  // Spline elevation
    MovementInfo read;
    read.flags = bit(MovementFlags::FALLING) | 0x04000000;
    REQUIRE(MovementPacket::readPitchAndFall(packet, read));
    CHECK(read.jumpVelocity == -3.0f);
    CHECK(read.jumpXYSpeed == 2.5f);
    CHECK_FALSE(packet.hasData());
}

TEST_CASE("A packet that ends in the jump block is refused", "[movement][fall]") {
    Packet packet(0);
    packet.writeUInt32(10);
    packet.writeFloat(-7.955547f);
    MovementInfo read;
    read.flags = bit(MovementFlags::FALLING);
    CHECK_FALSE(MovementPacket::readPitchAndFall(packet, read));
}

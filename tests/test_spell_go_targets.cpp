// SMSG_SPELL_GO's SpellCastTargets, as the WotLK reader keeps them.
//
// The client aims a spell's missile with these: at each target of a unit
// cast, at the destination of a ground one (FUN_007fffb0). The reader used to
// step over the two points; a ground-targeted missile then had nowhere to go.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "game/spell_target_point.hpp"

#include <optional>
#include <vector>

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;

namespace {

constexpr uint64_t kCaster = 0x0000000000000042ull;
constexpr uint64_t kTarget = 0xF130000ABC000123ull;

/// The head of a 3.3.5a SMSG_SPELL_GO with one hit and no misses.
wowee::network::Packet spellGoHead(uint32_t spellId) {
    wowee::network::Packet p(0);
    p.writePackedGuid(kCaster);
    p.writePackedGuid(kCaster);
    p.writeUInt8(1);         // castCount
    p.writeUInt32(spellId);
    p.writeUInt32(0x100);    // castFlags
    p.writeUInt32(123456);   // timestamp
    p.writeUInt8(1);         // hitCount
    p.writeUInt64(kTarget);
    p.writeUInt8(0);         // missCount
    return p;
}

}  // namespace

TEST_CASE("a unit cast keeps its target and flags", "[spell_go]") {
    auto p = spellGoHead(116);  // Frostbolt
    p.writeUInt32(0x2);         // TARGET_FLAG_UNIT
    p.writePackedGuid(kTarget);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    CHECK(data.targetFlags == 0x2);
    CHECK(data.targetGuid == kTarget);
    // The cast's time, which its dynamic object carries (0x00804cc0).
    CHECK(data.castTime == 123456);
    CHECK_FALSE(data.hasSourceLocation);
    CHECK_FALSE(data.hasDestLocation);
    REQUIRE(data.hitTargets.size() == 1);
    CHECK(data.hitTargets[0] == kTarget);
}

TEST_CASE("a ground cast keeps its source and destination points", "[spell_go]") {
    auto p = spellGoHead(42208);  // Blizzard
    p.writeUInt32(0x20 | 0x40);   // SOURCE_LOCATION | DEST_LOCATION
    p.writePackedGuid(0);         // source transport
    p.writeFloat(1.0f); p.writeFloat(2.0f); p.writeFloat(3.0f);
    p.writePackedGuid(0);         // destination transport
    p.writeFloat(-100.5f); p.writeFloat(250.25f); p.writeFloat(12.0f);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    CHECK(data.targetFlags == 0x60);
    REQUIRE(data.hasSourceLocation);
    CHECK(data.sourceX == 1.0f);
    CHECK(data.sourceY == 2.0f);
    CHECK(data.sourceZ == 3.0f);
    REQUIRE(data.hasDestLocation);
    CHECK(data.destX == -100.5f);
    CHECK(data.destY == 250.25f);
    CHECK(data.destZ == 12.0f);
}

TEST_CASE("the targets' guid flags are the client's", "[spell_go]") {
    // 0x009ab9c0: an object GUID for 0x18a02, an item GUID for 0x1010, a
    // string for 0x2000. UNIT_RAID (0x4) carries nothing of its own.
    auto p = spellGoHead(1);
    p.writeUInt32(0x8000 | 0x4 | 0x1000 | 0x2000);  // CORPSE_ALLY, UNIT_RAID, TRADE_ITEM, STRING
    p.writePackedGuid(kTarget);
    p.writePackedGuid(0x77);
    p.writeUInt8('a'); p.writeUInt8(0);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    CHECK(data.targetGuid == kTarget);
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("an ADJUST_MISSILE cast keeps its elevation and delay", "[spell_go]") {
    // castFlags 0x800 | 0x200000 | 0x20000: power, runes (two spent, each
    // with its cooldown byte), then the trajectory (0x0080e1b0).
    wowee::network::Packet p(0);
    p.writePackedGuid(kCaster);
    p.writePackedGuid(kCaster);
    p.writeUInt8(0);
    p.writeUInt32(57610);  // a cannon shot
    p.writeUInt32(0x100 | 0x800 | 0x200000 | 0x20000);
    p.writeUInt32(1000);
    p.writeUInt8(0);       // hits
    p.writeUInt8(0);       // misses
    p.writeUInt32(0x40);   // DEST_LOCATION
    p.writePackedGuid(0);
    p.writeFloat(1.0f); p.writeFloat(2.0f); p.writeFloat(3.0f);
    p.writeUInt32(500);    // power
    p.writeUInt8(0x3F);    // runes before
    p.writeUInt8(0x3C);    // runes after: 0 and 1 spent
    p.writeUInt8(10); p.writeUInt8(20);
    p.writeFloat(0.75f);   // elevation
    p.writeUInt32(1800);   // delay
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    REQUIRE(data.hasDestLocation);
    CHECK(data.destZ == 3.0f);
    REQUIRE(data.hasMissileTrajectory);
    CHECK(data.missileElevation == 0.75f);
    CHECK(data.missileDelayMs == 1800);
}

TEST_CASE("target flag 0x80000's places are read, and the packet to its end", "[spell_go]") {
    // 0x0080e1b0 after the targets: power (0x800), the trajectory (0x20000),
    // the ammo (0x20), cast flag 0x80000's two u32s, the destination's byte
    // (target flag 0x40), then target flag 0x80000's count and places, each
    // three floats and a u64.
    wowee::network::Packet p(0);
    p.writePackedGuid(kCaster);
    p.writePackedGuid(kCaster);
    p.writeUInt8(0);
    p.writeUInt32(42208);
    p.writeUInt32(0x100 | 0x800 | 0x20000 | 0x20 | 0x80000);
    p.writeUInt32(1000);
    p.writeUInt8(0);         // hits
    p.writeUInt8(0);         // misses
    p.writeUInt32(0x40 | 0x80000);
    p.writePackedGuid(0);
    p.writeFloat(1.0f); p.writeFloat(2.0f); p.writeFloat(3.0f);
    p.writeUInt32(500);      // power
    p.writeFloat(0.5f);      // elevation
    p.writeUInt32(900);      // delay
    p.writeUInt32(0x1111);   // ammo display
    p.writeUInt32(0x2222);   // ammo inventory type
    p.writeUInt32(0x3333);   // cast flag 0x80000
    p.writeUInt32(0x4444);
    p.writeUInt8(7);         // the destination's byte
    p.writeUInt32(2);        // places
    p.writeFloat(10.0f); p.writeFloat(20.0f); p.writeFloat(30.0f);
    p.writeUInt64(0);
    p.writeFloat(-1.5f); p.writeFloat(2.5f); p.writeFloat(-3.5f);
    p.writeUInt64(0x1FC0000000000ABCull);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    CHECK(data.missileElevation == 0.5f);
    CHECK(data.missileDelayMs == 900);
    REQUIRE(data.extraTargets.size() == 2);
    CHECK(data.extraTargets[0].x == 10.0f);
    CHECK(data.extraTargets[0].y == 20.0f);
    CHECK(data.extraTargets[0].z == 30.0f);
    CHECK(data.extraTargets[0].transportGuid == 0);
    CHECK(data.extraTargets[1].x == -1.5f);
    CHECK(data.extraTargets[1].y == 2.5f);
    CHECK(data.extraTargets[1].z == -3.5f);
    CHECK(data.extraTargets[1].transportGuid == 0x1FC0000000000ABCull);
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("no places without target flag 0x80000, whatever the cast flags", "[spell_go]") {
    // Cast flag 0x80000 is two u32s of its own; it does not stand for the
    // targets' places.
    wowee::network::Packet q(0);
    q.writePackedGuid(kCaster);
    q.writePackedGuid(kCaster);
    q.writeUInt8(0);
    q.writeUInt32(1);
    q.writeUInt32(0x100 | 0x80000);
    q.writeUInt32(1000);
    q.writeUInt8(0);
    q.writeUInt8(0);
    q.writeUInt32(0x2);
    q.writePackedGuid(kTarget);
    q.writeUInt32(5);
    q.writeUInt32(6);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(q, data));
    CHECK(data.targetGuid == kTarget);
    CHECK(data.extraTargets.empty());
    CHECK(q.getRemainingSize() == 0);
}

// ---------------------------------------------------------------------------
// Where the targets' points are. The wire carries each with the packed guid
// of the object whose frame it is in (0x009ab9c0 keeps both); the client
// puts the destination in the world through that object's matrix, an
// identity one when it does not know it (0x009ab810 -> 0x0074b430). The
// points were taken as canonical as they came, X and Y the wrong way round.

namespace {

namespace stp = wowee::game::spell_target_point;

/// A ship in the world at canonical (100, 200, 10), facing canonical yaw 0
/// - the wire's quarter turn (pi/2).
constexpr uint64_t kShip = 0x1FC00000000007F2ull;
std::optional<glm::vec3> shipFrame(uint64_t guid, const glm::vec3& wire) {
    if (guid != kShip) return std::nullopt;
    return stp::inObjectFrame(glm::vec3(100.0f, 200.0f, 10.0f), 0.0f, wire);
}

}  // namespace

TEST_CASE("a ground cast's destination is the wire's point, X and Y swapped", "[spell_go]") {
    // Blizzard (10) on the ground: no hits, target flag 0x40, its point in
    // the world's frame (packed guid 0), then 0x40's byte.
    const std::vector<uint8_t> bytes = {
        0x01, 0x42, 0x01, 0x42,                          // caster, caster unit
        0x01, 0x0A, 0x00, 0x00, 0x00,                    // cast count, spell 10
        0x00, 0x01, 0x00, 0x00, 0x40, 0xE2, 0x01, 0x00,  // cast flags 0x100, time
        0x00, 0x00,                                      // hits, misses
        0x40, 0x00, 0x00, 0x00, 0x00,                    // target flags, world
        0xB8, 0x86, 0xD1, 0x44, 0x52, 0xDA, 0x86, 0xC5, 0x7B, 0x14, 0x76, 0x42,
        0x00};
    wowee::network::Packet p(0, bytes);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    REQUIRE(data.hasDestLocation);
    CHECK(data.destTransportGuid == 0);
    const glm::vec3 world = stp::worldPoint(data.destTransportGuid, glm::vec3(data.destX, data.destY, data.destZ),
                                            shipFrame);
    CHECK(world.x == Catch::Approx(-4315.29f));
    CHECK(world.y == Catch::Approx(1676.21f));
    CHECK(world.z == Catch::Approx(61.52f));
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("a destination on a ship is in the ship's frame", "[spell_go]") {
    const std::vector<uint8_t> bytes = {
        0x01, 0x42, 0x01, 0x42,
        0x01, 0x0A, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x40, 0xE2, 0x01, 0x00,
        0x00, 0x00,
        0x40, 0x00, 0x00, 0x00,
        0xC3, 0xF2, 0x07, 0xC0, 0x1F,                    // the ship's packed guid
        0x00, 0x00, 0x20, 0xC0, 0x00, 0x00, 0xC8, 0x40, 0x00, 0x00, 0x80, 0x40,
        0x00};
    wowee::network::Packet p(0, bytes);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    REQUIRE(data.hasDestLocation);
    CHECK(data.destTransportGuid == kShip);
    const glm::vec3 local(data.destX, data.destY, data.destZ);
    // (-2.5, 6.25) turned a quarter turn is (-6.25, -2.5) on the wire, from
    // the ship's wire (200, 100): canonical (97.5, 193.75), 4 above it.
    const glm::vec3 world = stp::worldPoint(data.destTransportGuid, local, shipFrame);
    CHECK(world.x == Catch::Approx(97.5f));
    CHECK(world.y == Catch::Approx(193.75f));
    CHECK(world.z == Catch::Approx(14.0f));
    // A ship the client does not know: the point as it came.
    const glm::vec3 unknown = stp::worldPoint(data.destTransportGuid, local,
                                              [](uint64_t, const glm::vec3&) { return std::optional<glm::vec3>{}; });
    CHECK(unknown.x == Catch::Approx(6.25f));
    CHECK(unknown.y == Catch::Approx(-2.5f));
    CHECK(unknown.z == Catch::Approx(4.0f));
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("each of the two points keeps its own frame", "[spell_go]") {
    const std::vector<uint8_t> bytes = {
        0x01, 0x42, 0x01, 0x42,
        0x01, 0x0A, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x40, 0xE2, 0x01, 0x00,
        0x00, 0x00,
        0x60, 0x00, 0x00, 0x00,
        0x00,                                            // source: the world's
        0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x40, 0xC0, 0x00, 0x00, 0xC0, 0x3F,
        0xC3, 0xF2, 0x07, 0xC0, 0x1F,                    // destination: the ship's
        0x00, 0x00, 0x20, 0xC0, 0x00, 0x00, 0xC8, 0x40, 0x00, 0x00, 0x80, 0x40,
        0x00};
    wowee::network::Packet p(0, bytes);
    SpellGoData data;
    REQUIRE(SpellGoParser::parse(p, data));
    REQUIRE(data.hasSourceLocation);
    REQUIRE(data.hasDestLocation);
    CHECK(data.sourceTransportGuid == 0);
    CHECK(data.destTransportGuid == kShip);
    const glm::vec3 source = stp::worldPoint(data.sourceTransportGuid,
                                             glm::vec3(data.sourceX, data.sourceY, data.sourceZ), shipFrame);
    CHECK(source.x == Catch::Approx(-3.0f));
    CHECK(source.y == Catch::Approx(5.0f));
    CHECK(source.z == Catch::Approx(1.5f));
    const glm::vec3 dest = stp::worldPoint(data.destTransportGuid,
                                           glm::vec3(data.destX, data.destY, data.destZ), shipFrame);
    CHECK(dest.x == Catch::Approx(97.5f));
    CHECK(dest.y == Catch::Approx(193.75f));
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("an object's frame turns with its facing", "[spell_go]") {
    // The matrix turns the wire's axes: facing the wire's +Y (canonical yaw
    // 0) ahead is the wire's +Y, canonical +X; the object's left (local +Y)
    // is then the wire's -X, canonical -Y.
    const glm::vec3 ahead = stp::inObjectFrame(glm::vec3(0.0f), 0.0f, glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(ahead.x == Catch::Approx(1.0f));
    CHECK(ahead.y == Catch::Approx(0.0f).margin(1e-5));
    const glm::vec3 left = stp::inObjectFrame(glm::vec3(0.0f), 0.0f, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(left.x == Catch::Approx(0.0f).margin(1e-5));
    CHECK(left.y == Catch::Approx(-1.0f));
    // Facing the wire's +X (canonical -pi/2): ahead is canonical +Y.
    const glm::vec3 side = stp::inObjectFrame(glm::vec3(5.0f, 5.0f, 0.0f), -wowee::core::coords::PI * 0.5f,
                                              glm::vec3(2.0f, 0.0f, 0.0f));
    CHECK(side.x == Catch::Approx(5.0f));
    CHECK(side.y == Catch::Approx(7.0f));
}

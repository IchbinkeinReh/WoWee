// SMSG_SPELL_GO's SpellCastTargets, as the WotLK reader keeps them.
//
// The client aims a spell's missile with these: at each target of a unit
// cast, at the destination of a ground one (FUN_007fffb0). The reader used to
// step over the two points; a ground-targeted missile then had nowhere to go.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "core/application.hpp"
#include "game/world_packets.hpp"

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

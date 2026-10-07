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

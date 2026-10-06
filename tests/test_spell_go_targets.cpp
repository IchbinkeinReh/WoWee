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

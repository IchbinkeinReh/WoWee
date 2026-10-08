// SMSG_SPELL_UPDATE_CHAIN_TARGETS as the client reads it (0x00800470): the
// caster's guid, the spell, the count and each target's guid, none packed.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "core/application.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee;

TEST_CASE("SMSG_SPELL_UPDATE_CHAIN_TARGETS reads whole guids", "[spell_chain]") {
    network::Packet packet(0x330);
    packet.writeUInt64(0xF130001234000042ull);
    packet.writeUInt32(421);
    packet.writeUInt32(2);
    packet.writeUInt64(0x0000000000000007ull);
    packet.writeUInt64(0xF130005678000099ull);
    game::SpellUpdateChainTargetsData data;
    REQUIRE(game::SpellUpdateChainTargetsParser::parse(packet, data));
    CHECK(data.casterGuid == 0xF130001234000042ull);
    CHECK(data.spellId == 421);
    REQUIRE(data.targets.size() == 2);
    CHECK(data.targets[0] == 7);
    CHECK(data.targets[1] == 0xF130005678000099ull);
}

TEST_CASE("SMSG_SPELL_UPDATE_CHAIN_TARGETS short of its targets is refused", "[spell_chain]") {
    network::Packet packet(0x330);
    packet.writeUInt64(1);
    packet.writeUInt32(421);
    packet.writeUInt32(3);
    packet.writeUInt64(2);
    game::SpellUpdateChainTargetsData data;
    CHECK_FALSE(game::SpellUpdateChainTargetsParser::parse(packet, data));
}

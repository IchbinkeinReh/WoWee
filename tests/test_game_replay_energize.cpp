// SMSG_SPELLENERGIZELOG through GameHandler's own dispatch, built as
// AzerothCore's Unit::SendEnergizeSpellLog (WotLK) and cmangos's (classic and
// TBC) write it: both guids packed in every expansion. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

using replay::Session;

TEST_CASE("an energize log has packed guids in every expansion", "[replay][energize]") {
    using wowee::game::CombatTextEntry;
    constexpr uint64_t kShaman = 0xF130000000001234ull;
    for (const char* expansion : {"wotlk", "tbc", "classic"}) {
        CAPTURE(expansion);
        Session s(expansion);
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELLENERGIZELOG));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writePackedGuid(kShaman);
        p.writeUInt32(2687);
        p.writeUInt32(1);  // rage
        p.writeUInt32(20);
        s.receive(p);
        // A power gain is no floater over the world (only damage, misses and
        // the player's heals are), so the log entry is what shows the read.
        CHECK(s.handler().getCombatText().empty());
        const auto& log = s.handler().getCombatLog();
        REQUIRE_FALSE(log.empty());
        CHECK(log.back().type == CombatTextEntry::ENERGIZE);
        CHECK(log.back().amount == 20);
        CHECK(log.back().powerType == 1);
        CHECK_FALSE(log.back().isPlayerSource);
    }
}

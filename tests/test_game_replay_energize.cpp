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
        const auto& texts = s.handler().getCombatText();
        REQUIRE(texts.size() == 1);
        CHECK(texts.front().type == CombatTextEntry::ENERGIZE);
        CHECK(texts.front().amount == 20);
        CHECK(texts.front().powerType == 1);
        CHECK(texts.front().srcGuid == kShaman);
        CHECK(texts.front().dstGuid == Session::kPlayerGuid);
        CHECK_FALSE(texts.front().isPlayerSource);
    }
}

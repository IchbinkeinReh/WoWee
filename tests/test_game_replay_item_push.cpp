// SMSG_ITEM_PUSH_RESULT through GameHandler's own dispatch, built as
// cmangos's Player::SendNewItem and vmangos's ItemPushResult write it: TBC and
// WotLK end with the count now carried, classic does not. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <string>

using replay::Session;

TEST_CASE("an item push is read in every expansion", "[replay][itempush]") {
    for (const char* expansion : {"wotlk", "tbc", "classic", "turtle"}) {
        CAPTURE(expansion);
        Session s(expansion);
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ITEM_PUSH_RESULT));
        p.writeUInt64(Session::kPlayerGuid);
        p.writeUInt32(0);    // received
        p.writeUInt32(0);    // created
        p.writeUInt32(1);    // show in chat
        p.writeUInt8(255);   // backpack
        p.writeUInt32(23);   // slot
        p.writeUInt32(2589); // Linen Cloth
        p.writeUInt32(0);    // suffix factor
        p.writeUInt32(0);    // random property
        p.writeUInt32(2);    // count
        const std::string e = expansion;
        if (e == "wotlk" || e == "tbc") p.writeUInt32(5);  // count carried
        s.receive(p);
        // The item's name is not cached in a replay session, so the
        // notification waits for it.
        const auto& pending = s.handler().pendingItemPushNotifsRef();
        CHECK(pending.size() == 1);
        if (pending.empty()) continue;
        CHECK(pending.front().itemId == 2589);
        CHECK(pending.front().count == 2);
    }
}

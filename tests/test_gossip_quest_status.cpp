// The yellow "?" against the grey one in the gossip and greeting lists
// (game/gossip_quest_status.hpp). A 3.3.5 server sends icon 4 for a taken
// quest whether it is finished or not, so the quest log decides - and the
// list must not undo what the log knows.
#include <catch_amalgamated.hpp>

#include "game/gossip_icon.hpp"
#include "game/gossip_quest_status.hpp"

#include <string>

using namespace wowee::game::gossip_quest;

// The table at 0x00acef20, by the words GossipFrame_Update puts in front of
// "GossipIcon": an innkeeper's option is the binder's, a spirit healer's the
// healer's.
TEST_CASE("an option's icon is the client's word for it", "[gossip_quest]") {
    const char* const kWords[] = {"gossip", "vendor",   "taxi",   "trainer",      "healer",    "binder",
                                  "banker", "petition", "tabard", "battlemaster", "auctioneer"};
    for (uint32_t i = 0; i < 11; ++i) CHECK(std::string(wowee::game::gossipIconName(i)) == kWords[i]);
    for (uint32_t i : {11u, 16u, 20u, 99u}) CHECK(std::string(wowee::game::gossipIconName(i)) == "gossip");
}

TEST_CASE("the server's icons sort quests into offered and taken", "[gossip_quest]") {
    CHECK(iconIsAvailable(2));
    CHECK_FALSE(iconIsActive(2));
    CHECK(iconIsActive(4));
    CHECK_FALSE(iconIsAvailable(4));
    CHECK(iconIsActive(3));
    CHECK(iconIsActive(5));
    CHECK_FALSE(iconIsActive(0));
}

// The interface's lists: 3 and 4 active, all else available (0x0058a5d0).
TEST_CASE("the window's lists take 3 and 4 as active", "[gossip_quest]") {
    for (uint32_t icon : {3u, 4u}) CHECK((listIsActive(icon) && !listIsAvailable(icon)));
    for (uint32_t icon : {0u, 1u, 2u, 5u, 6u, 7u, 8u, 10u}) CHECK((listIsAvailable(icon) && !listIsActive(icon)));
}

TEST_CASE("icon 4 leaves the log's complete flag as it was", "[gossip_quest]") {
    // Reading 4 as "not complete" cleared the slot's bit on every talk.
    CHECK(logCompleteAfterList(4, true));
    CHECK_FALSE(logCompleteAfterList(4, false));
    // A completable icon is news.
    CHECK(logCompleteAfterList(5, false));
}

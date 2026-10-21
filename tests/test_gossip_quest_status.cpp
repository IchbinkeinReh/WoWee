// The yellow "?" against the grey one in the gossip and greeting lists
// (game/gossip_quest_status.hpp). A 3.3.5 server sends icon 4 for a taken
// quest whether it is finished or not, so the quest log decides - and the
// list must not undo what the log knows.
#include <catch_amalgamated.hpp>

#include "game/gossip_quest_status.hpp"

using namespace wowee::game::gossip_quest;

TEST_CASE("the server's icons sort quests into offered and taken", "[gossip_quest]") {
    CHECK(iconIsAvailable(2));
    CHECK_FALSE(iconIsActive(2));
    CHECK(iconIsActive(4));
    CHECK_FALSE(iconIsAvailable(4));
    CHECK(iconIsActive(3));
    CHECK(iconIsActive(5));
    CHECK_FALSE(iconIsActive(0));
}

TEST_CASE("a finished quest sent as icon 4 is complete by the quest log", "[gossip_quest]") {
    // What AzerothCore / TrinityCore send for QUEST_STATUS_COMPLETE.
    CHECK(activeQuestIsComplete(4, /*inLog=*/true, /*logComplete=*/true));
    // The same icon for one still under way.
    CHECK_FALSE(activeQuestIsComplete(4, true, false));
    // Not in the log at all: nothing says it is done.
    CHECK_FALSE(activeQuestIsComplete(4, false, false));
}

TEST_CASE("an icon that says completable is complete whatever the log", "[gossip_quest]") {
    CHECK(activeQuestIsComplete(5, false, false));
    CHECK(activeQuestIsComplete(10, true, false));
}

TEST_CASE("an offered quest is never drawn as complete", "[gossip_quest]") {
    CHECK_FALSE(activeQuestIsComplete(2, true, true));
}

TEST_CASE("icon 4 leaves the log's complete flag as it was", "[gossip_quest]") {
    // Reading 4 as "not complete" cleared the slot's bit on every talk.
    CHECK(logCompleteAfterList(4, true));
    CHECK_FALSE(logCompleteAfterList(4, false));
    // A completable icon is news.
    CHECK(logCompleteAfterList(5, false));
}

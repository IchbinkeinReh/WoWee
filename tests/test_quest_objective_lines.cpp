// The quest log's objective lines: what the list opens with.
//
// GetNumQuestLeaderBoards and GetQuestLogLeaderBoard answer one line for the
// record's 512-byte text at +0x1a24 when it is not empty (0x005e1300,
// 0x005e1460). That text is the quest's EndText - an exploration or event
// objective's wording, empty for most quests - and not the objectives
// paragraph, which put the whole paragraph at the top of every quest's list.
#include <catch_amalgamated.hpp>

#include "game/quest_objectives.hpp"

using wowee::game::GameHandler;
using wowee::game::questObjectiveCount;

TEST_CASE("the objectives paragraph is not an objective line", "[quest][objective]") {
    GameHandler::QuestLogEntry q;
    q.objectives = "Bring 10 Scorpid Stingers to Rhinag in Razor Hill.";
    q.killObjectives[0].npcOrGoId = 3127;
    q.killObjectives[0].required = 10;
    CHECK(questObjectiveCount(q) == 1);
}

TEST_CASE("an exploration quest's EndText is the first line", "[quest][objective]") {
    GameHandler::QuestLogEntry q;
    q.objectives = "Explore the kraal.";
    q.endText = "Kral erkundet";
    CHECK(questObjectiveCount(q) == 1);
    q.killObjectives[0].npcOrGoId = 3127;
    q.killObjectives[0].required = 10;
    CHECK(questObjectiveCount(q) == 2);
}

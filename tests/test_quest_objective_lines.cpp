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

TEST_CASE("a quest link's requirement lines are worded without progress", "[quest][objective][tooltip]") {
    // 0x005e1300 counts, 0x005e1460 words each line with the _NOPROGRESS
    // strings; any place past the list answers the players-slain line.
    using wowee::game::QuestTooltipLookup;
    using wowee::game::questTooltipRequirementCount;
    using wowee::game::questTooltipRequirementLine;
    GameHandler::QuestLogEntry q;
    q.endText = "Kraal explored";
    q.killObjectives[0] = {3127, 10};
    q.killObjectives[1] = {-1234, 0};   // an id alone is a line here
    q.objectiveTexts[1] = "Lever pulled";
    q.itemObjectives[0] = {500, 1};     // the quest's own item, once: not listed
    q.itemObjectives[1] = {600, 4};
    q.sourceItemId = 500;
    q.objectiveSourceItems[0] = 700;
    q.playersSlain = 5;

    QuestTooltipLookup look;
    look.targetName = [](int32_t id) { return id == 3127 ? std::string("Scorpid") : std::string(); };
    look.itemName = [](uint32_t id) { return id == 600 ? std::string("Stinger") : std::string("Page"); };
    look.carried = [](uint32_t) { return 2u; };
    look.text = [](const char*, const char* enUS) { return std::string(enUS); };

    CHECK(questTooltipRequirementCount(q, false) == 5);
    CHECK(questTooltipRequirementLine(q, 0, false, look) == "Kraal explored");
    CHECK(questTooltipRequirementLine(q, 1, false, look) == "Scorpid x 10");
    CHECK(questTooltipRequirementLine(q, 2, false, look) == "Lever pulled x 0");
    CHECK(questTooltipRequirementLine(q, 3, false, look) == "Stinger x 4");
    CHECK(questTooltipRequirementLine(q, 4, false, look) == "Page: (2)");
    // Met in the log: the handed-over items drop out and the players line
    // takes their place.
    CHECK(questTooltipRequirementLine(q, 4, true, look) == "Players x 5");

    GameHandler::QuestLogEntry bare;
    bare.objectives = "Talk to Rhinag.";
    CHECK(questTooltipRequirementCount(bare, false) == 0);
    CHECK(questTooltipRequirementCount(bare, true) == 1);
    CHECK(questTooltipRequirementLine(bare, 0, true, look) == "Talk to Rhinag.");
}

TEST_CASE("the item the giver handed over has no line", "[quest][objective]") {
    // Quest 10120: Orion's report (28024), wanted once and the quest's own.
    GameHandler::QuestLogEntry q;
    q.objectives = "Bringt Orions Bericht.";
    q.sourceItemId = 28024;
    q.itemObjectives[0] = {28024, 1};
    CHECK(questObjectiveCount(q) == 0);
    // Met, there is the sentence instead.
    q.complete = true;
    CHECK(questObjectiveCount(q) == 1);

    // Wanted more than once it is an objective, and so is any other item.
    q.itemObjectives[0].required = 2;
    CHECK(questObjectiveCount(q) == 1);
    q.itemObjectives[1] = {600, 4};
    CHECK(questObjectiveCount(q) == 2);
}

TEST_CASE("a quest that requires nothing has no line when complete", "[quest][objective]") {
    GameHandler::QuestLogEntry q;
    q.objectives = "Sprecht mit Thrall.";
    q.complete = true;
    CHECK(questObjectiveCount(q) == 0);
    q.questFlags = 2;
    CHECK(questObjectiveCount(q) == 1);
}

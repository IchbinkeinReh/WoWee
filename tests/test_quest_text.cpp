// Which of a quest's strings GetQuestLogCompletionText answers (0x005e06d0):
// the CompletedText, or the objectives sentence where there is none or the quest
// is flagged to use it. Never the EndText, which names an objective.
#include <catch_amalgamated.hpp>

#include "game/quest_text.hpp"

using wowee::game::questCompletionText;

TEST_CASE("the completed line is the fifth string", "[quest][text]") {
    CHECK(questCompletionText(0, "Speak with Marshal Dughan.", "Bring 10 candles.") ==
          "Speak with Marshal Dughan.");
    // However short: nothing in the client asks how it reads.
    CHECK(questCompletionText(0, "Go.", "Bring 10 candles.") == "Go.");
}

TEST_CASE("the objectives sentence stands in when there is none", "[quest][text]") {
    CHECK(questCompletionText(0, "", "Bring 10 candles.") == "Bring 10 candles.");
    CHECK(questCompletionText(0, "", "").empty());
}

TEST_CASE("a quest flagged 0x40000 answers its objectives", "[quest][text]") {
    CHECK(questCompletionText(0x40000, "Speak with Marshal Dughan.", "Bring 10 candles.") ==
          "Bring 10 candles.");
    CHECK(questCompletionText(0x20008, "Speak with Marshal Dughan.", "Bring 10 candles.") ==
          "Speak with Marshal Dughan.");
}

#include <catch_amalgamated.hpp>

#include "game/chat_profanity.hpp"

using namespace wowee::game::chat_profanity;

// 0x00509dd0's profanity loop and 0x004fcf40.

TEST_CASE("Locales index as the client's table does", "[chat_profanity]") {
    CHECK(localeIndex("enUS") == 0);
    CHECK(localeIndex("enGB") == 0);
    CHECK(localeIndex("deDE") == 3);
    CHECK(localeIndex("ruRU") == 8);
}

TEST_CASE("Word anchors become word boundaries", "[chat_profanity]") {
    CHECK(toEcmaScript("\\<dildo\\>") == "\\bdildo\\b");
    CHECK(toEcmaScript("nig+a") == "nig+a");
}

TEST_CASE("A match is masked with the cycling symbols, case aside", "[chat_profanity]") {
    Filter f;
    f.load({"\\<twat\\>", "shit"});
    std::string line = "what a TWAT, bullshit";
    CHECK(f.apply(line));
    CHECK(line == "what a !@#$, bull%^&*");
    // The place in the symbols carries on to the next line.
    std::string next = "shit";
    CHECK(f.apply(next));
    CHECK(next == "!@#$");
    // A word boundary keeps a longer word whole.
    std::string clean = "twattle";
    CHECK_FALSE(f.apply(clean));
}

TEST_CASE("Nothing inside a coloured run is touched", "[chat_profanity]") {
    CHECK(insideColour("|cff00ff00shit|r", 12));
    CHECK_FALSE(insideColour("|cff00ff00x|r shit", 15));
    Filter f;
    f.load({"shit"});
    std::string link = "|cff00ff00[shit]|r";
    CHECK_FALSE(f.apply(link));
}

TEST_CASE("An expression that does not compile is left out", "[chat_profanity]") {
    Filter f;
    f.load({"(", "ok"});
    CHECK(f.size() == 1);
}

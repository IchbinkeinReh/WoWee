// A chat bubble's time on screen by its words (0x0056c1c0) and its 250 ms
// fades (0x0056c500).
#include <catch_amalgamated.hpp>

#include "ui/chat/chat_bubble_timing.hpp"
#include "ui/chat/chat_bubble_view.hpp"

namespace cb = wowee::ui::chat_bubble;

TEST_CASE("Others' bubbles last 2000 ms and 750 a word", "[chat_bubble]") {
    CHECK(cb::durationMs("", false) == 0u);
    CHECK(cb::durationMs("hello", false) == 2750u);
    CHECK(cb::durationMs("hello there friend", false) == 2000u + 3u * 750u);
    // Runs of blanks are one break; a trailing blank still ends a word, a
    // leading one does not.
    CHECK(cb::durationMs("hello  \t there", false) == 2000u + 2u * 750u);
    CHECK(cb::durationMs("hello ", false) == 2000u + 2u * 750u);
    CHECK(cb::durationMs(" hello", false) == 2000u + 750u);
}

TEST_CASE("The player's own bubbles last 1000 ms and 500 a word", "[chat_bubble]") {
    CHECK(cb::durationMs("hello", true) == 1500u);
    CHECK(cb::durationMs("hello there", true) == 2000u);
}

TEST_CASE("Fades take 250 ms each way and turn back where they are", "[chat_bubble]") {
    float a = 0.0f;
    a = cb::stepAlpha(a, true, 0.125f);
    CHECK(a == Catch::Approx(0.5f));
    a = cb::stepAlpha(a, true, 1.0f);
    CHECK(a == 1.0f);
    a = cb::stepAlpha(a, false, 0.0625f);
    CHECK(a == Catch::Approx(0.75f));
    a = cb::stepAlpha(a, false, 1.0f);
    CHECK(a == 0.0f);
    CHECK(cb::kRangeYards * cb::kRangeYards == 625.0f);
}

// 0x007e1680
TEST_CASE("a bubble's words are without their markup", "[chat_bubble]") {
    CHECK(cb::bubbleText("Hallo [du]") == "Hallo [du]");
    CHECK(cb::bubbleText("da: |cff9d9d9d|Hitem:1:0|h[Kaputte Reisszaehne]|h|r [so]") == "da: Kaputte Reisszaehne [so]");
    CHECK(cb::bubbleText("a|TInterface\\Icons\\x:0|tb") == "ab");
    CHECK(cb::bubbleText("a||b|nc|") == "a||b|nc|");
    CHECK(cb::bubbleText("a|cff") == "a");
    CHECK(cb::bubbleText("a|Hitem:1") == "a");
}

// 0x00720010
TEST_CASE("a bubble's words have their chat type's colour", "[chat_bubble]") {
    CHECK(std::string(cb::typeName(1)) == "SAY");
    CHECK(std::string(cb::typeName(2)) == "PARTY");
    CHECK(std::string(cb::typeName(6)) == "YELL");
    CHECK(std::string(cb::typeName(0x0c)) == "MONSTER_SAY");
    CHECK(std::string(cb::typeName(0x0e)) == "MONSTER_YELL");
    CHECK(std::string(cb::typeName(0x33)) == "PARTY_LEADER");
}

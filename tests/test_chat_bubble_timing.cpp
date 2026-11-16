// A chat bubble's time on screen by its words (0x0056c1c0) and its 250 ms
// fades (0x0056c500).
#include <catch_amalgamated.hpp>

#include "ui/chat/chat_bubble_timing.hpp"

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

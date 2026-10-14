// Which of a spell's two SpellVisuals the client shows (0x007fa290).
#include <catch_amalgamated.hpp>

#include "game/spell_visual_choice.hpp"

using namespace wowee::game::spell_visual;

TEST_CASE("violenceLevel under 2 shows a spell's second visual", "[spell_visual]") {
    CHECK(choose(749, 121, 2) == 749);
    CHECK(choose(749, 121, 1) == 121);
    CHECK(choose(749, 121, 0) == 121);
    // Without a second, the first.
    CHECK(choose(749, 0, 0) == 749);
}

TEST_CASE("violenceLevel keeps its number within the locale's limit", "[spell_visual]") {
    // 0x007f39b0: below 0 or above the limit is the limit.
    CHECK(violenceLevel("0") == 0);
    CHECK(violenceLevel("1") == 1);
    CHECK(violenceLevel("2") == 2);
    CHECK(violenceLevel("3") == kViolenceLevelLimit);
    CHECK(violenceLevel("-1") == kViolenceLevelLimit);
}

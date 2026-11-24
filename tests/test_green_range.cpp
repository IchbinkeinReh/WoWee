// The client's own green range (0x005e2860) and grey quests (0x0058aba0).
#include <catch_amalgamated.hpp>

#include "game/grey_triviality.hpp"

using wowee::game::triviality::greyGap;
using wowee::game::triviality::questIsGrey;

TEST_CASE("the green range is the table's", "[quest]") {
    CHECK(greyGap(1) == 4);
    CHECK(greyGap(9) == 4);
    CHECK(greyGap(10) == 5);
    CHECK(greyGap(29) == 6);
    CHECK(greyGap(39) == 7);
    CHECK(greyGap(40) == 8);
    CHECK(greyGap(57) == 8);
    CHECK(greyGap(80) == 8);
    CHECK(greyGap(255) == 8);
}

TEST_CASE("a quest is grey a level under the green range", "[quest]") {
    CHECK_FALSE(questIsGrey(80, 72));
    CHECK(questIsGrey(80, 71));
    CHECK_FALSE(questIsGrey(50, 42));
    CHECK(questIsGrey(50, 41));
    CHECK(questIsGrey(50, 40));
    // One with no level of its own is never; one of level 0 may be.
    CHECK_FALSE(questIsGrey(80, -1));
    CHECK(questIsGrey(80, 0));
    CHECK_FALSE(questIsGrey(4, 0));
    CHECK(questIsGrey(5, 0));
}

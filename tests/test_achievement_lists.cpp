#include <catch_amalgamated.hpp>

#include "game/achievement_list.hpp"

using namespace wowee::game;

// Rows of 3.3.5's Achievement.dbc (id, side, the one before, category, flags).
TEST_CASE("the achievements window lists one step of a chain, and the player's side", "[achievement]") {
    const std::vector<AchievementRow> rows = {
        {6, -1, 0, 92, 4},     {7, -1, 6, 92, 4},    {8, -1, 7, 92, 4},   {9, -1, 8, 92, 4},
        {705, -1, 0, 92, 0},   {2536, 1, 0, 92, 0},  {2537, 0, 0, 92, 0}, {1000, -1, 0, 92, 2},
        {457, -1, 0, 81, 0},   {458, -1, 0, 81, 0},  {1234, -1, 0, 92, 0x800},
    };
    const auto list = [&](std::vector<uint32_t> mine, std::vector<uint32_t> theirs = {}) {
        return AchievementList(rows, mine, theirs, 0);
    };
    // Nothing earned: the first of the chain, 705, the Horde's 2537.
    auto l = list({});
    CHECK(l.count(92, false) == std::pair<uint32_t, uint32_t>{3, 0});
    CHECK(l.at(92, 1) == 6);
    CHECK(l.at(92, 2) == 705);
    CHECK(l.at(92, 3) == 2537);
    // One not shown (flag 2) comes after what is counted; the binding then
    // answers nothing for it, as the client does.
    CHECK(l.at(92, 4) == 1000);
    CHECK(l.at(92, 5) == 0);
    CHECK(l.at(92, 0) == 0);
    // Every step with the second argument.
    CHECK(l.count(92, true) == std::pair<uint32_t, uint32_t>{6, 0});
    // No feat of strength that is not earned.
    CHECK(l.count(81, false) == std::pair<uint32_t, uint32_t>{0, 0});
    CHECK(l.at(81, 1) == 0);
    // Levels 10 and 20: the later is shown, and what comes next.
    l = list({7, 6, 458});
    CHECK(l.count(92, false) == std::pair<uint32_t, uint32_t>{4, 1});
    CHECK(l.at(92, 1) == 7);
    CHECK(l.at(92, 2) == 8);
    CHECK(l.at(92, 3) == 705);
    CHECK(l.at(92, 4) == 2537);
    CHECK(l.count(81, false) == std::pair<uint32_t, uint32_t>{1, 1});
    CHECK(l.at(81, 1) == 458);
    // All of them, the window's bar over everything (-1).
    CHECK(l.count(-1, false).second == 3);
    // The compared player's: what both have first, then the player's, theirs.
    l = list({705, 6}, {2536, 705});
    CHECK(l.at(92, 1) == 705);
    CHECK(l.at(92, 2) == 6);
    CHECK(l.at(92, 3) == 2536);
    CHECK(l.at(92, 4) == 7);
    CHECK(l.count(92, false) == std::pair<uint32_t, uint32_t>{5, 2});
    // Earned, what is hidden until then is there.
    CHECK(list({1234}).at(92, 1) == 1234);
}

// 0x005b28d0, 0x005b2ab0
TEST_CASE("earned achievements, the latest first and of one time by their order", "[achievement]") {
    const auto at = [](uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t weekday) {
        return year << 24 | month << 20 | day << 14 | weekday << 11 | hour << 6;
    };
    const uint32_t noon = at(26, 10, 3, 12, 2), later = at(26, 10, 3, 13, 2), sunday = at(26, 10, 1, 12, 7);
    CHECK(newestFirst({{1, noon, 5}, {2, sunday, 9}, {3, later, 1}, {4, noon, 7}}) ==
          std::vector<uint32_t>{3, 4, 1, 2});
}

TEST_CASE("every statistic is listed, earned or not", "[achievement]") {
    const std::vector<AchievementRow> rows = {{90, -1, 0, 130, 1}, {91, -1, 0, 130, 1}, {92, -1, 0, 92, 0}};
    const AchievementList l(rows, {91}, {}, 0);
    CHECK(l.at(-2, 1) == 91);
    CHECK(l.at(-2, 2) == 90);
    CHECK(l.at(-2, 3) == 91);
    CHECK(l.at(-1, 1) == 92);
    CHECK(l.count(-2, false) == std::pair<uint32_t, uint32_t>{2, 1});
    CHECK(achievementTold(0, false));
    CHECK_FALSE(achievementTold(0x2, true));
    CHECK_FALSE(achievementTold(0x800, false));
    CHECK(achievementTold(0x800, true));
}

// 0x005b3b30
TEST_CASE("the compared player's of a category", "[achievement]") {
    const std::vector<AchievementRow> rows = {{10, -1, 0, 92, 0},  {20, -1, 10, 92, 0}, {30, -1, 20, 92, 0},
                                              {50, 1, 0, 92, 0},   {51, 0, 0, 92, 0},   {60, -1, 0, 92, 2},
                                              {90, -1, 0, 130, 1}};
    const std::vector<uint32_t> theirs = {90, 60, 50, 20, 10};
    const auto compared = [&](std::vector<uint32_t> own, int32_t category, std::optional<int32_t> side) {
        return AchievementList(rows, own, theirs, 0).comparedCount(category, side);
    };
    CHECK(compared({20}, 92, 1) == 2);      // 20 and the Alliance's; 10 is passed
    CHECK(compared({}, 92, 1) == 3);
    CHECK(compared({}, 92, 0) == 2);
    CHECK(compared({}, 92, std::nullopt) == 3);
    CHECK(compared({}, 92, -2) == 2);
    CHECK(compared({20}, -1, 1) == 3);
    CHECK(compared({20}, -2, 1) == 1);
}

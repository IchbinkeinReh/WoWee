#include <catch_amalgamated.hpp>

#include "game/achievement_list.hpp"
#include "game/statistic_text.hpp"

using namespace wowee::game;

namespace {
std::string coins(uint64_t v) { return std::to_string(v) + "c"; }
}  // namespace

TEST_CASE("a statistic of money is written in coins", "[achievement]") {
    // 333 "Looted gold" (flags 0x1, criterion 0x20), 328 a sum (0x9), 1176 a bar (0x21).
    CHECK(statisticText(0x1, {{true, 31728, 0, 0x20, ""}}, coins) == "31728c");
    CHECK(statisticText(0x9, {{true, 100, 0, 0x20, ""}, {true, 20, 0, 0x20, ""}}, coins) == "120c");
    CHECK(statisticText(0x0, {{true, 104782, 1000000, 0x21, ""}}, coins) == "104782c / 1000000c");
    CHECK(statisticText(0x0, {{false, 0, 1000000, 0x21, ""}}, coins) == "0c / 1000000c");
    CHECK(statisticText(0x1, {{false, 0, 0, 0x20, ""}}, coins) == "--");
    // And what is none is a number.
    CHECK(statisticText(0x1, {{true, 12, 0, 0, ""}}, coins) == "12");
    CHECK(statisticText(0x0, {{true, 12, 10, 0x1, ""}}, coins) == "10 / 10");
    CHECK(statisticText(0x11, {{true, 3, 0, 0, "Ragnaros"}, {true, 5, 0, 0, "Nefarian"}}, coins) == "5 (Nefarian)");
    CHECK(statisticText(0x20, {{true, 3, 5, 0, ""}, {true, 5, 5, 0, ""}}, coins) == "1");
}

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

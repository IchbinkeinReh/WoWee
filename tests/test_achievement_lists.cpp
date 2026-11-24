#include <catch_amalgamated.hpp>

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

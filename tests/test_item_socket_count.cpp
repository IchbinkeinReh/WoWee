// How many sockets the socketing window shows: 0x007094e0 counts each of the
// template's three colours that is set and adds what a prismatic enchantment
// gives (0x00708140) - a belt buckle's socket on a belt with none (T454).
#include <catch_amalgamated.hpp>

#include "game/item_stat_summary.hpp"

using wowee::game::item_stats::socketCount;

TEST_CASE("a buckle's socket counts on a belt with none (T454)", "[item][sockets]") {
    CHECK(socketCount({0, 0, 0}, 0) == 0);
    CHECK(socketCount({0, 0, 0}, 1) == 1);
    CHECK(socketCount({2, 4, 0}, 1) == 3);
}

TEST_CASE("each set colour counts, a gap does not end the count", "[item][sockets]") {
    CHECK(socketCount({2, 4, 8}, 0) == 3);
    CHECK(socketCount({2, 0, 8}, 0) == 2);
    CHECK(socketCount({0, 0, 1}, 0) == 1);
}

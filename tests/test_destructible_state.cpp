#include <catch_amalgamated.hpp>

#include "game/destructible_state.hpp"

// A destructible building's answer to a change of its flags (0x0070ed00,
// 0x0070e6a0, 0x0070dfa0).

using namespace wowee::game::destructible;

TEST_CASE("the state comes from flags bits 9 to 11", "[destructible]") {
    CHECK(stateOfFlags(0) == 0);
    CHECK(stateOfFlags(0x200) == 1);
    CHECK(stateOfFlags(0x400) == 2);
    CHECK(stateOfFlags(0x600) == 3);
    CHECK(stateOfFlags(0x100) == 0);  // the hit bit is not a state
}

TEST_CASE("a hit plays when the hit bit changes either way", "[destructible]") {
    CHECK(impactPlays(0x000 ^ 0x100));
    CHECK(impactPlays(0x300 ^ 0x200));
    CHECK_FALSE(impactPlays(0x200 ^ 0x400));
    CHECK(stateChecked(0x200 ^ 0x400));
    CHECK(stateChecked(0x100));
    CHECK_FALSE(stateChecked(0x1 ^ 0x20));
}

TEST_CASE("the player drops through a building that worsened under it", "[destructible]") {
    // Intact to damaged, destroyed from either: inside, on foot.
    CHECK(playerFallsThrough(0, 1, true, false, 0, true));
    CHECK(playerFallsThrough(0, 2, true, false, 0, true));
    CHECK(playerFallsThrough(1, 2, true, false, 0, true));
    // Not getting better, nor into rebuilding.
    CHECK_FALSE(playerFallsThrough(2, 1, true, false, 0, true));
    CHECK_FALSE(playerFallsThrough(2, 3, true, false, 0, true));
    CHECK_FALSE(playerFallsThrough(1, 0, true, false, 0, true));
    // Nor outside it, in a vehicle, flying, or with no look before.
    CHECK_FALSE(playerFallsThrough(0, 2, true, false, 0, false));
    CHECK_FALSE(playerFallsThrough(0, 2, true, true, 0, true));
    CHECK_FALSE(playerFallsThrough(0, 2, true, false, 0x2000000u, true));
    CHECK_FALSE(playerFallsThrough(0, 2, false, false, 0, true));
}

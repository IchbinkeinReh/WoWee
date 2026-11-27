// Who may enter a battleground (0x0054ba00).
#include <catch_amalgamated.hpp>

#include "game/battleground_rules.hpp"

using namespace wowee::game::battleground_rules;

TEST_CASE("a bracket of the first map holds the player's level", "[battleground]") {
    const std::vector<Bracket> brackets = {{489, 10, 14}, {489, 15, 19}, {529, 20, 24}, {489, 1, 80}};
    CHECK(canEnter(2, 489, 12, brackets));
    CHECK(canEnter(2, 489, 19, brackets));
    // The walk ends with the map's own rows.
    CHECK_FALSE(canEnter(2, 489, 20, brackets));
    CHECK(canEnter(3, 529, 22, brackets));
    CHECK_FALSE(canEnter(3, 529, 19, brackets));
    CHECK_FALSE(canEnter(7, 566, 80, brackets));
    // The random one at eighty alone.
    CHECK(canEnter(kRandomBattleground, 30, 80, brackets));
    CHECK_FALSE(canEnter(kRandomBattleground, 30, 79, brackets));
}

// T410 (FIXER Z): 0x0054aa30 - a battleground row's side from its race's
// faction group; a player the name cache does not know is 0.
TEST_CASE("a battleground score row's side is its race's (0x0054aa30)", "[battleground]") {
    using wowee::game::battleground_rules::scoreRowSide;
    CHECK(scoreRowSide(false, 2) == 0);   // not cached: 0 whatever the race
    CHECK(scoreRowSide(true, 4) == 0);    // Horde
    CHECK(scoreRowSide(true, 2 | 1) == 1);   // Alliance (player bit set too)
    CHECK(scoreRowSide(true, 6) == 0);    // Horde wins over Alliance
    CHECK(scoreRowSide(true, 1) == -1);   // neither
}

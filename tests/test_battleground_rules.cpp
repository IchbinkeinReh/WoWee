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

// TargetNearestEnemy's filter, 0x00518e40 case 1 (nearest_enemy_filter.hpp).
#include <catch_amalgamated.hpp>

#include "game/nearest_enemy_filter.hpp"

using wowee::game::nearestEnemyCandidate;

TEST_CASE("TargetNearestEnemy takes an attackable living unit", "[target]") {
    CHECK(nearestEnemyCandidate(true, 100, 0, 0));
    CHECK(nearestEnemyCandidate(true, 1, 0x1 /* lootable */, 1 /* sitting */));
}

TEST_CASE("TargetNearestEnemy refuses what 0x00518e40 refuses", "[target]") {
    CHECK_FALSE(nearestEnemyCandidate(false, 100, 0, 0));      // 0x00729740 says no
    CHECK_FALSE(nearestEnemyCandidate(true, 0, 0, 0));         // vtable +0x128: dead
    CHECK_FALSE(nearestEnemyCandidate(true, 100, 0x20, 0));    // +0x124 bit 5: UNIT_DYNFLAG_DEAD
    CHECK_FALSE(nearestEnemyCandidate(true, 100, 0, 7));       // +0x138 == 7: UNIT_STAND_STATE_DEAD
}

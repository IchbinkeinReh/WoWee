// UnitLevel's answer (0x0060f9e0): the level, or -1 for the target frame's skull.
#include <catch_amalgamated.hpp>
#include "game/unit_level_display.hpp"

using wowee::game::displayedUnitLevel;

namespace {
constexpr int kHostile = 1;   // 0x007251c0's scale: 0 hated .. 7 exalted
constexpr int kNeutral = 3;
constexpr int kFriendly = 4;
constexpr unsigned kBoss = 0x4u;
}  // namespace

TEST_CASE("A hostile unit ten or more levels above the player is a skull", "[unit_level]") {
    CHECK(displayedUnitLevel(30, true, 20, kHostile, 0) == -1);
    CHECK(displayedUnitLevel(35, true, 20, kHostile, 0) == -1);
    CHECK(displayedUnitLevel(29, true, 20, kHostile, 0) == 29);  // nine above: the level
    CHECK(displayedUnitLevel(30, true, 20, 0, 0) == -1);         // hated counts as hostile
}

TEST_CASE("A neutral or friendly unit shows its level however high", "[unit_level]") {
    CHECK(displayedUnitLevel(80, true, 10, kNeutral, 0) == 80);
    CHECK(displayedUnitLevel(80, true, 10, kFriendly, 0) == 80);
    CHECK(displayedUnitLevel(80, true, 10, 2, 0) == 80);  // unfriendly is not hostile
}

TEST_CASE("A boss is a skull at any level and reaction", "[unit_level]") {
    CHECK(displayedUnitLevel(80, true, 80, kHostile, kBoss) == -1);
    CHECK(displayedUnitLevel(12, true, 80, kFriendly, kBoss) == -1);
    CHECK(displayedUnitLevel(83, false, 0, 0, kBoss) == -1);
    // Other type flags (tameable 0x1, ghost-visible 0x2) leave the level.
    CHECK(displayedUnitLevel(12, true, 80, kHostile, 0x1u | 0x2u) == 12);
}

TEST_CASE("Without a player only the boss rule applies", "[unit_level]") {
    CHECK(displayedUnitLevel(70, false, 0, kHostile, 0) == 70);
}

TEST_CASE("A dead or low mob far below the player keeps its level", "[unit_level]") {
    CHECK(displayedUnitLevel(1, true, 80, kHostile, 0) == 1);
}

TEST_CASE("A drunk player sees an enemy lower, by round(share x 5 + 0.5)", "[unit_level]") {
    using wowee::game::drunkLevelReduction;
    // Sober takes nothing off; fully drunk (100, or more held to it) six.
    CHECK(drunkLevelReduction(0, 0) == 0);
    CHECK(drunkLevelReduction(100, 0) == 6);
    CHECK(drunkLevelReduction(0, 250) == 6);
    // The x87 rounds to the nearest even: 1.5 -> 2, 2.5 -> 2, 3.5 -> 4.
    CHECK(drunkLevelReduction(10, 0) == 1);   // 0.5 + 0.5
    CHECK(drunkLevelReduction(20, 0) == 2);   // 1.0 + 0.5
    CHECK(drunkLevelReduction(40, 0) == 2);   // 2.0 + 0.5
    CHECK(drunkLevelReduction(60, 0) == 4);   // 3.0 + 0.5
    // The larger of the drunk byte and the fake inebriation counts.
    CHECK(drunkLevelReduction(20, 60) == 4);
    CHECK(drunkLevelReduction(60, 20) == 4);
}

TEST_CASE("The drunk view lowers only a unit the player may attack, never below 1", "[unit_level]") {
    // An enemy the player may attack, seen drunk.
    CHECK(displayedUnitLevel(30, true, 30, kHostile, 0, 100, 0, true) == 24);
    CHECK(displayedUnitLevel(3, true, 30, kHostile, 0, 100, 0, true) == 1);
    // Not attackable, or sober: the level stands.
    CHECK(displayedUnitLevel(30, true, 30, kFriendly, 0, 100, 0, false) == 30);
    CHECK(displayedUnitLevel(30, true, 30, kHostile, 0, 0, 0, true) == 30);
    // The skull and the boss come first and are not lowered.
    CHECK(displayedUnitLevel(45, true, 30, kHostile, 0, 100, 0, true) == -1);
    CHECK(displayedUnitLevel(30, true, 30, kHostile, kBoss, 100, 0, true) == -1);
    // Without a player there is nobody drunk.
    CHECK(displayedUnitLevel(30, false, 0, kHostile, 0, 100, 0, true) == 30);
}

// The liquid level the movement reads a swimmer's depth from
// (collision_liquid.hpp): a building group's liquid by its tiles and the
// tests on a chunk's.

#include <catch_amalgamated.hpp>

#include "rendering/collision_liquid.hpp"

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {
// Two tiles by one from (10, 20): heights 1, 2, 3 on the near row and 5, 6, 7
// on the far.
const glm::vec2 kCorner(10.0f, 20.0f);
const std::vector<float> kHeights = {1.0f, 2.0f, 3.0f, 5.0f, 6.0f, 7.0f};
}  // namespace

TEST_CASE("a building's liquid is read between its tile's corners", "[collision_liquid]") {
    const std::vector<uint8_t> flags = {0x00, 0x00};
    const float t = kBuildingLiquidTile;
    // The near corner of the first tile.
    auto level = buildingLiquidLevel({10.0f, 20.0f, -10.0f}, kCorner, 2, 1, kHeights, flags);
    REQUIRE(level);
    CHECK(*level == Approx(1.0f));
    // Half way across the second tile and half way up it.
    level = buildingLiquidLevel({10.0f + 1.5f * t, 20.0f + 0.5f * t, -10.0f}, kCorner, 2, 1, kHeights, flags);
    REQUIRE(level);
    CHECK(*level == Approx((2.5f + 6.5f) * 0.5f));
}

TEST_CASE("a building's liquid counts only under its surface and on its tiles",
          "[collision_liquid]") {
    std::vector<uint8_t> flags = {0x00, 0x00};
    // Above the surface: not in it.
    CHECK_FALSE(buildingLiquidLevel({10.1f, 20.1f, 5.0f}, kCorner, 2, 1, kHeights, flags));
    // Off the tiles.
    CHECK_FALSE(buildingLiquidLevel({9.0f, 20.1f, -10.0f}, kCorner, 2, 1, kHeights, flags));
    CHECK_FALSE(buildingLiquidLevel({10.1f, 20.0f + 1.5f * kBuildingLiquidTile, -10.0f}, kCorner, 2,
                                    1, kHeights, flags));
    // A tile whose low four bits are all set has none; other bits do not
    // matter.
    flags[0] = 0x0F;
    CHECK_FALSE(buildingLiquidLevel({10.1f, 20.1f, -10.0f}, kCorner, 2, 1, kHeights, flags));
    flags[0] = 0x4F;
    CHECK_FALSE(buildingLiquidLevel({10.1f, 20.1f, -10.0f}, kCorner, 2, 1, kHeights, flags));
    flags[0] = 0x47;
    CHECK(buildingLiquidLevel({10.1f, 20.1f, -10.0f}, kCorner, 2, 1, kHeights, flags));
}

TEST_CASE("a chunk's liquid holds a point under it with the ground lower still",
          "[collision_liquid]") {
    CHECK(groundLiquidHolds(4.0f, 5.0f, 3.0f));
    // A hundredth above the surface still counts.
    CHECK(groundLiquidHolds(5.005f, 5.0f, 3.0f));
    CHECK_FALSE(groundLiquidHolds(5.02f, 5.0f, 3.0f));
    // The ground over the point: the liquid is under the terrain.
    CHECK_FALSE(groundLiquidHolds(4.0f, 5.0f, 4.5f));
    // No ground known (a hole): the liquid counts.
    CHECK(groundLiquidHolds(4.0f, 5.0f, std::nullopt));
}

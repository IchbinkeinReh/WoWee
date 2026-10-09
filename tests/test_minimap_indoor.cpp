// The indoor minimap: the WMO groups' own pictures as the client lays them
// out (0x007f5ba0, 0x007af8d0, 0x007f5070, 0x007afc70, 0x0057bd10).
#include <catch_amalgamated.hpp>

#include "rendering/minimap_indoor.hpp"

#include <algorithm>

namespace mi = wowee::rendering::minimap_indoor;

TEST_CASE("a picture is a power of two, 32 to 256 pixels", "[minimap_indoor]") {
    CHECK(mi::tilePixels(5.0f) == 32);     // 10 pixels wanted
    CHECK(mi::tilePixels(16.0f) == 32);
    CHECK(mi::tilePixels(16.5f) == 64);
    CHECK(mi::tilePixels(40.0f) == 128);   // 80 wanted
    CHECK(mi::tilePixels(64.0f) == 128);
    CHECK(mi::tilePixels(100.0f) == 256);
    CHECK(mi::tilePixels(1000.0f) == 256);
}

TEST_CASE("a small group is one picture, stretched a yard past its box", "[minimap_indoor]") {
    const auto tiles = mi::groupTiles({10.0f, 20.0f, 0.0f}, {50.0f, 40.0f, 5.0f},
                                      {-1000.0f, -1000.0f}, {1000.0f, 1000.0f});
    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].x == 0);
    CHECK(tiles[0].y == 0);
    // 40 yards across is 128 pixels, 64 yards; 20 up is 64 pixels, 32 yards.
    CHECK(tiles[0].min.x == Catch::Approx(9.0f));
    CHECK(tiles[0].min.y == Catch::Approx(19.0f));
    CHECK(tiles[0].max.x == Catch::Approx(10.0f + 64.0f + 1.0f));
    CHECK(tiles[0].max.y == Catch::Approx(20.0f + 32.0f + 1.0f));
}

TEST_CASE("a big group is 128-yard pictures from its low corner", "[minimap_indoor]") {
    const auto tiles = mi::groupTiles({0.0f, 0.0f, 0.0f}, {300.0f, 130.0f, 5.0f},
                                      {-1000.0f, -1000.0f}, {1000.0f, 1000.0f});
    // ceil(300 / 128) = 3 across, ceil(130 / 128) = 2 up.
    REQUIRE(tiles.size() == 6);
    CHECK(tiles[0].x == 0);
    CHECK(tiles[0].y == 0);
    CHECK(tiles[1].x == 1);
    CHECK(tiles[1].min.x == Catch::Approx(128.0f));
    CHECK(tiles[1].max.x == Catch::Approx(256.0f));
    // The last across reaches past the box's far edge, plus the yard.
    CHECK(tiles[2].max.x == Catch::Approx(385.0f));
    CHECK(tiles[3].y == 1);
    CHECK(tiles[3].min.y == Catch::Approx(128.0f));
    CHECK(tiles[3].max.y == Catch::Approx(257.0f));
}

TEST_CASE("only the pictures reaching the area", "[minimap_indoor]") {
    const auto tiles = mi::groupTiles({0.0f, 0.0f, 0.0f}, {300.0f, 130.0f, 5.0f},
                                      {130.0f, 0.0f}, {200.0f, 100.0f});
    REQUIRE(tiles.size() == 1);
    CHECK(tiles[0].x == 1);
    CHECK(tiles[0].y == 0);
    // Touching counts.
    CHECK(mi::groupTiles({0.0f, 0.0f, 0.0f}, {300.0f, 130.0f, 5.0f},
                         {256.0f, 0.0f}, {256.0f, 0.0f}).size() == 2);
}

TEST_CASE("pictures are named after the WMO, the group and the tile", "[minimap_indoor]") {
    const std::string base = mi::wmoBaseName("World\\wmo\\Dungeon\\KL_Deadmines\\Deadmines.wmo");
    CHECK(base == "World\\wmo\\Dungeon\\KL_Deadmines\\Deadmines");
    CHECK(mi::tileName(base, 7, 0, 12) ==
          "World\\wmo\\Dungeon\\KL_Deadmines\\Deadmines_007_00_12");
    CHECK(mi::wmoBaseName("World\\wmo.dir\\Thing") == "World\\wmo.dir\\Thing");
}

TEST_CASE("the area is the radius's cell grown by the radius", "[minimap_indoor]") {
    const auto a = mi::area({130.0f, -10.0f}, 60.0f);
    CHECK(a.min.x == Catch::Approx(60.0f));
    CHECK(a.max.x == Catch::Approx(240.0f));
    CHECK(a.min.y == Catch::Approx(-120.0f));
    CHECK(a.max.y == Catch::Approx(60.0f));
    CHECK(a.span() == Catch::Approx(180.0f));
    // The player is somewhere in the middle third.
    CHECK(a.center().x == Catch::Approx(150.0f));
}

TEST_CASE("lower groups go down first and the player's own last", "[minimap_indoor]") {
    CHECK(mi::drawKey(5.0f, 10.0f, false) < mi::drawKey(15.0f, 10.0f, false));
    CHECK(mi::drawKey(1e6f, 10.0f, false) < mi::drawKey(0.0f, 10.0f, true));
}

TEST_CASE("the groups through the portals, inside staying inside", "[minimap_indoor]") {
    using G = mi::GroupInfo;
    // 0 the player's room, 1 the next room, 2 a courtyard open to the sky,
    // 3 a room past the courtyard, 4 an unreachable room, 5 a room far away.
    std::vector<G> groups = {
        {mi::kGroupInterior, {0, 0, 0}, {10, 10, 5}},
        {mi::kGroupInterior, {10, 0, 0}, {20, 10, 5}},
        {mi::kGroupExterior, {20, 0, 0}, {30, 10, 5}},
        {mi::kGroupInterior, {30, 0, 0}, {40, 10, 5}},
        {mi::kGroupInterior | mi::kGroupUnreachable, {0, 10, 0}, {10, 20, 5}},
        {mi::kGroupInterior, {500, 500, 0}, {510, 510, 5}},
    };
    std::vector<std::vector<uint32_t>> neighbours = {{1, 4}, {0, 2, 5}, {1, 3}, {2}, {0}, {1}};
    auto found = mi::connectedGroups(groups, neighbours, 0, {-100, -100}, {100, 100});
    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<uint32_t>{0, 1});
}

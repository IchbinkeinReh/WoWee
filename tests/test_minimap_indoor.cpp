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
    // 0x007f3a70: past "World\\", and up to the last dot.
    const std::string base = mi::wmoBaseName("World\\wmo\\Dungeon\\KL_Deadmines\\Deadmines.wmo");
    CHECK(base == "wmo\\Dungeon\\KL_Deadmines\\Deadmines");
    CHECK(mi::tileName(base, 7, 0, 12) == "wmo\\Dungeon\\KL_Deadmines\\Deadmines_007_00_12");
    CHECK(mi::wmoBaseName("WORLD/wmo/A/B.wmo") == "wmo/A/B");
    CHECK(mi::wmoBaseName("wmo\\A\\B.wmo") == "wmo\\A\\B");
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

namespace {
// A portal square in the plane x = `x`, from y0 to y1, z 0 to 5.
std::vector<glm::vec3> doorway(float x, float y0, float y1, float z0 = 0.0f, float z1 = 5.0f) {
    return {{x, y0, z0}, {x, y1, z0}, {x, y1, z1}, {x, y0, z1}};
}
}  // namespace

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
    // Portal p links the groups either side of it.
    std::vector<std::vector<glm::vec3>> portals = {
        doorway(10, 2, 8), doorway(20, 2, 8), doorway(30, 2, 8), doorway(5, 10, 10.5f), doorway(15, 9, 10)};
    std::vector<std::vector<mi::PortalLink>> links = {
        {{1, 0}, {4, 3}}, {{0, 0}, {2, 1}, {5, 4}}, {{1, 1}, {3, 2}}, {{2, 2}}, {{0, 3}}, {{1, 4}}};
    const mi::WalkModel m{&groups, &links, &portals};
    auto found = mi::connectedGroups(m, 0, {-100, -100, -2.5f}, {100, 100, 2.5f}, true);
    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<uint32_t>{0, 1});

    // Not inside: the exterior groups, through the portals, from an exterior one.
    found = mi::connectedGroups(m, 2, {-100, -100, -2.5f}, {100, 100, 2.5f}, false);
    CHECK(found == std::vector<uint32_t>{2});
}

TEST_CASE("the walk: portals wholly outside the area, the stop groups, the limit", "[minimap_indoor]") {
    using G = mi::GroupInfo;
    std::vector<G> groups = {
        {mi::kGroupInterior, {0, 0, 0}, {10, 10, 5}},
        {mi::kGroupInterior, {10, 0, 0}, {20, 10, 5}},
        {mi::kGroupInterior, {20, 0, 0}, {30, 10, 5}},
        {mi::kGroupInterior, {30, 0, 0}, {40, 10, 5}},
    };
    std::vector<std::vector<glm::vec3>> portals = {doorway(10, 2, 8), doorway(20, 2, 8), doorway(30, 2, 8)};
    std::vector<std::vector<mi::PortalLink>> links = {{{1, 0}}, {{0, 0}, {2, 1}}, {{1, 1}, {3, 2}}, {{2, 2}}};
    const mi::WalkModel m{&groups, &links, &portals};

    // All four through the doorways.
    auto found = mi::connectedGroups(m, 0, {-100, -100, -2.5f}, {100, 100, 2.5f}, true);
    CHECK(found == std::vector<uint32_t>{0, 1, 2, 3});
    // A doorway wholly above the area's height is not gone through.
    portals[1] = doorway(20, 2, 8, 10.0f, 12.0f);
    found = mi::connectedGroups(m, 0, {-100, -100, -2.5f}, {100, 100, 2.5f}, true);
    CHECK(found == std::vector<uint32_t>{0, 1});
    portals[1] = doorway(20, 2, 8);
    // No further than the limit.
    found = mi::connectedGroups(m, 0, {-100, -100, -2.5f}, {100, 100, 2.5f}, true, 2);
    CHECK(found == std::vector<uint32_t>{0, 1, 2});
    // Never past or into WMOAreaTable groups 0x59e7 and 0x59e8.
    groups[1].areaGroupId = 0x59e7;
    found = mi::connectedGroups(m, 0, {-100, -100, -2.5f}, {100, 100, 2.5f}, true);
    CHECK(found == std::vector<uint32_t>{0});
}

TEST_CASE("indoors by the area tables", "[minimap_indoor]") {
    const uint32_t blocked = 0x20, outdoorsHere = 0x1, open = 0x4;
    // An inside group is indoors without any rows.
    CHECK(mi::isIndoors(mi::kGroupInterior, nullptr, nullptr));
    CHECK(mi::isIndoors(0, nullptr, nullptr));
    // An exterior one only by the building's row.
    CHECK_FALSE(mi::isIndoors(mi::kGroupExterior, nullptr, nullptr));
    CHECK(mi::isIndoors(mi::kGroupExterior, nullptr, &outdoorsHere));
    CHECK_FALSE(mi::isIndoors(mi::kGroupExterior, nullptr, &open));
    // The group's row's 0x20 rules it out.
    CHECK_FALSE(mi::isIndoors(mi::kGroupInterior, &blocked, &outdoorsHere));
    // 0x8/0x10 on the building with an exterior group: the building's own
    // pictures rather than the walk.
    const uint32_t own = 0x10;
    CHECK(mi::buildingPictures(mi::kGroupExterior, &own));
    CHECK_FALSE(mi::buildingPictures(mi::kGroupInterior, &own));
    CHECK_FALSE(mi::buildingPictures(mi::kGroupExterior, &outdoorsHere));
    // Dalaran's: its building's row is 0x9, its streets exterior groups.
    const uint32_t dalaran = 0x9;
    CHECK(mi::isIndoors(0x8020080d, nullptr, &dalaran));
    CHECK(mi::buildingPictures(0x8020080d, &dalaran));
}

TEST_CASE("the building's own pictures: one past its groups, over its box", "[minimap_indoor]") {
    // ND_Dalaran: 91 groups, MOHD's box 614 by 702 yards - its pictures are
    // ND_Dalaran_091_00_00 to _04_05, five across and six up.
    CHECK(mi::buildingPictureGroup(91) == 91u);
    const glm::vec3 lo(-336.42f, -379.13f, -325.46f), hi(277.27f, 323.17f, 549.88f);
    const auto all = mi::groupTiles(lo, hi, {-1000.0f, -1000.0f}, {1000.0f, 1000.0f});
    REQUIRE(all.size() == 30u);
    CHECK(std::any_of(all.begin(), all.end(), [](const mi::Tile& t) { return t.x == 4 && t.y == 5; }));
    CHECK(mi::tileName("wmo\\Northrend\\Dalaran\\ND_Dalaran", 91, 4, 5) ==
          "wmo\\Northrend\\Dalaran\\ND_Dalaran_091_04_05");
}

TEST_CASE("the area tables' rows, exactly by building, name set and group", "[minimap_indoor]") {
    mi::AreaRows rows;
    rows[mi::areaKey(5164, 0, -1)] = {0x9, 4395, ""};
    rows[mi::areaKey(5164, 0, 24042)] = {0x0, 4601, "Dalaran"};
    const auto* root = mi::areaRow(&rows, 5164, 0, -1);
    REQUIRE(root);
    CHECK(root->areaId == 4395u);
    CHECK(mi::areaRow(&rows, 5164, 0, 24042)->name == "Dalaran");
    CHECK_FALSE(mi::areaRow(&rows, 5164, 1, 24042));
    CHECK_FALSE(mi::areaRow(&rows, 5164, 0, 24043));
    CHECK_FALSE(mi::areaRow(nullptr, 5164, 0, -1));
}

TEST_CASE("the outcode", "[minimap_indoor]") {
    const glm::vec3 lo(0.0f), hi(1.0f);
    CHECK(mi::outcode({0.5f, 0.5f, 0.5f}, lo, hi) == 0u);
    CHECK(mi::outcode({-1.0f, 0.5f, 2.0f}, lo, hi) == (1u | 0x20u));
    CHECK(mi::outcode({2.0f, -1.0f, -1.0f}, lo, hi) == (8u | 2u | 4u));
    CHECK(mi::outcode({0.5f, 2.0f, 0.5f}, lo, hi) == 0x10u);
}

// The world's ambience as the client picks it (audio/zone_ambience.hpp):
// the slots and the one that plays (0x004c86f0), a place's SoundAmbience
// (0x0078e9a0), a world state's (0x004cbe70), and the liquid's sound
// (0x004d2390).
#include <catch_amalgamated.hpp>

#include "audio/zone_ambience.hpp"
#include "rendering/water_surface_grid.hpp"

#include <unordered_map>

namespace za = wowee::audio::zone_ambience;

TEST_CASE("the highest slot with a sound for the time plays", "[audio][ambience]") {
    za::Slots slots{};
    CHECK_FALSE(za::pick(slots, true, false));

    slots[za::kSlotPlace] = {4183, 4184};  // ForestNormalDay / Night
    CHECK(za::pick(slots, true, false) == za::Pick{za::kSlotPlace, 4183});
    CHECK(za::pick(slots, false, false) == za::Pick{za::kSlotPlace, 4184});

    // The weather's over the place's - outdoors only.
    slots[za::kSlotWeather] = {8533, 8533};  // RainLight
    CHECK(za::pick(slots, true, false) == za::Pick{za::kSlotWeather, 8533});
    CHECK(za::pick(slots, true, true) == za::Pick{za::kSlotPlace, 4183});

    // Under liquid over both, indoors or out.
    slots[za::kSlotUnderwater] = {za::kUnderwaterSound, za::kUnderwaterSound};
    CHECK(za::pick(slots, true, false) == za::Pick{za::kSlotUnderwater, 4209});
    CHECK(za::pick(slots, false, true) == za::Pick{za::kSlotUnderwater, 4209});

    // And the screen effect's over everything.
    slots[za::kSlotScreenEffect] = {100, 101};
    CHECK(za::pick(slots, false, false) == za::Pick{za::kSlotScreenEffect, 101});
}

TEST_CASE("a slot with only one half of the day holds the slots under it back for that half only",
          "[audio][ambience]") {
    za::Slots slots{};
    slots[za::kSlotPlace] = {10, 11};
    slots[za::kSlotWorldState] = {-1, 0};  // a sound 0 at night: silence
    CHECK(za::pick(slots, true, false) == za::Pick{za::kSlotPlace, 10});
    CHECK(za::pick(slots, false, false) == za::Pick{za::kSlotWorldState, 0});
}

TEST_CASE("a place's ambience is its building group's, its building's, then its area's and zone's",
          "[audio][ambience]") {
    // Outside: the group's row, the building's, the area's, the zone's.
    CHECK(za::placeAmbience(35, 47, 3, 4, false) == 4);
    CHECK(za::placeAmbience(35, 47, 3, 0, false) == 3);
    CHECK(za::placeAmbience(35, 47, 0, 0, false) == 47);
    CHECK(za::placeAmbience(35, 0, 0, 0, false) == 35);
    // Inside a building nothing comes from the ground: a tavern with no row
    // of its own is quiet, not the forest outside.
    CHECK(za::placeAmbience(35, 47, 3, 4, true) == 4);
    CHECK(za::placeAmbience(35, 47, 3, 0, true) == 3);
    CHECK(za::placeAmbience(35, 47, 0, 0, true) == 0);
}

TEST_CASE("a world state's ambience is the row whose value it has and which names the place",
          "[audio][ambience]") {
    // Rows as WorldStateZoneSounds.dbc has them (Wintergrasp's and Arathi's).
    const std::vector<za::WorldStateSound> rows = {
        {2019, 1, 159, 0, 36},
        {2019, 1, 0, 18154, 43},
        {2019, 1, 0, 18153, 103},
        {3710, 1, 4197, 0, 463},
    };
    std::unordered_map<uint32_t, uint32_t> states;
    const auto value = [&](uint32_t s) -> uint32_t {
        const auto it = states.find(s);
        return it != states.end() ? it->second : 0;  // never sent: 0 (0x00548d10)
    };

    za::Place wintergrasp{.zone = 4197};
    CHECK_FALSE(za::worldStateAmbience(rows, wintergrasp, value));
    states[3710] = 1;
    CHECK(za::worldStateAmbience(rows, wintergrasp, value) == 463u);

    // By the area, outside.
    za::Place area{.zone = 45, .area = 159};
    states[2019] = 1;
    CHECK(za::worldStateAmbience(rows, area, value) == 36u);

    // Inside, by the building's or the group's row only.
    za::Place inside{.zone = 45, .area = 159, .buildingRow = 18153, .groupRow = 18154, .indoors = true};
    CHECK(za::worldStateAmbience(rows, inside, value) == 43u);
    inside.groupRow = 1;
    CHECK(za::worldStateAmbience(rows, inside, value) == 103u);

    // Outside with a building: the group's row first, then the area's, the
    // zone's; the building's own is not asked.
    za::Place court{.zone = 45, .area = 159, .buildingRow = 18153, .groupRow = 18154};
    CHECK(za::worldStateAmbience(rows, court, value) == 43u);
    court.groupRow = 7;
    CHECK(za::worldStateAmbience(rows, court, value) == 36u);

    // A state with another value names nothing.
    states[2019] = 0;
    CHECK_FALSE(za::worldStateAmbience(rows, court, value));
}

TEST_CASE("the liquid nearest across the ground is the one heard", "[audio][ambience][liquid]") {
    const glm::vec3 player(100.0f, 100.0f, 0.0f);
    CHECK_FALSE(za::nearestLiquid({}, player));
    // Far below but close across the ground beats near in height but further
    // across it (0x004d21b0 measures x and y only).
    const std::vector<za::LiquidCell> cells = {
        {1, {110.0f, 100.0f, 0.0f}},
        {2, {104.0f, 100.0f, -40.0f}},
        {5, {96.0f, 100.0f, 0.0f}},  // as near: the first stays
    };
    const auto n = za::nearestLiquid(cells, player);
    REQUIRE(n);
    CHECK(n->liquidType == 2);
}

TEST_CASE("the liquid's sound follows within 20 yards and starts anew past them", "[audio][ambience][liquid]") {
    const glm::vec3 at(0.0f, 0.0f, 0.0f);
    CHECK(za::liquidChange(1111, at, 1111, at) == za::LiquidChange::Keep);
    CHECK(za::liquidChange(1111, at, 1111, {19.0f, 0.0f, 0.0f}) == za::LiquidChange::Move);
    CHECK(za::liquidChange(1111, at, 1111, {20.0f, 0.0f, 0.0f}) == za::LiquidChange::Restart);
    // Another sound, or none (0), starts again where it is.
    CHECK(za::liquidChange(1111, at, 1114, at) == za::LiquidChange::Restart);
    CHECK(za::liquidChange(1111, at, 0, at) == za::LiquidChange::Restart);
    CHECK(za::liquidChange(0, at, 0, at) == za::LiquidChange::Keep);
}

TEST_CASE("a liquid cell is heard from its middle", "[audio][ambience][liquid]") {
    // A chunk's grid: 8 cells of 4.1666 yards each way, stepping down x and y
    // as the client's do.
    const float cell = 33.333333f / 8.0f;
    const glm::vec3 origin(1000.0f, 2000.0f, 5.0f);
    const glm::vec3 stepX(-cell, 0.0f, 0.0f), stepY(0.0f, -cell, 0.0f);
    const glm::vec2 c = wowee::rendering::surfaceCellCentre(origin, stepX, stepY, 2, 3);
    CHECK(c.x == Catch::Approx(1000.0f - 2.5f * cell));
    CHECK(c.y == Catch::Approx(2000.0f - 3.5f * cell));
}

TEST_CASE("the liquid's portal walk from the player's group", "[audio][ambience][liquid]") {
    // Three groups in a row: 0 (the player's, inside) - 1 (inside) - 2
    // (open to the sky), and 3 past 2, all boxes holding the player.
    const glm::vec3 lo(-10.0f), hi(10.0f);
    std::vector<za::WalkGroup> groups(4);
    for (auto& g : groups) {
        g.min = lo;
        g.max = hi;
        g.loaded = true;
    }
    groups[0].past = {1};
    groups[1].past = {0, 2, 0xffff};
    groups[2].past = {1, 3};
    groups[2].flags = za::kGroupOpenToSky;
    groups[3].past = {2};
    const glm::vec3 player(0.0f);

    std::vector<uint32_t> asked;
    uint32_t openDepth = za::kNoOpenDepth;
    za::walkLiquidGroups(groups, 0, 0, 0, player, openDepth, [&](uint32_t g) { asked.push_back(g); });
    // Two portals deep: 3 is past the limit.
    CHECK(asked == std::vector<uint32_t>{0, 1, 2});
    CHECK(openDepth == 2);
    // Open to the sky two portals off: the ground's liquid is not asked.
    CHECK_FALSE(za::groundLiquidAsked(true, openDepth));
    CHECK(za::groundLiquidAsked(false, openDepth));

    // From the middle one it is one portal off.
    asked.clear();
    openDepth = za::kNoOpenDepth;
    za::walkLiquidGroups(groups, 1, 1, 0, player, openDepth, [&](uint32_t g) { asked.push_back(g); });
    CHECK(asked == std::vector<uint32_t>{1, 0, 2, 3});
    CHECK(openDepth == 1);
    CHECK(za::groundLiquidAsked(true, openDepth));

    // A group whose box does not hold the player ends the walk there.
    groups[1].max = glm::vec3(-5.0f);
    asked.clear();
    openDepth = za::kNoOpenDepth;
    za::walkLiquidGroups(groups, 0, 0, 0, player, openDepth, [&](uint32_t g) { asked.push_back(g); });
    CHECK(asked == std::vector<uint32_t>{0});
    CHECK(openDepth == za::kNoOpenDepth);
}

namespace {
/// A building liquid of `w` by `h` tiles at the origin, every vertex at
/// `height`, every tile with liquid.
struct TestLiquid {
    std::vector<float> heights;
    std::vector<uint8_t> flags;
    za::BuildingLiquid at(uint32_t type, int w, int h) const {
        return {type, w, h, glm::vec2(0.0f), heights, flags};
    }
    TestLiquid(int w, int h, float height)
        : heights(static_cast<size_t>((w + 1) * (h + 1)), height), flags(static_cast<size_t>(w * h), 0) {}
};
}  // namespace

TEST_CASE("a building's liquid: the nearest tile by the growing disc", "[audio][ambience]") {
    constexpr float T = za::kBuildingLiquidTile;
    TestLiquid water(10, 10, 7.0f);
    std::vector<za::BuildingLiquidFound> found;

    // On a tile: that tile, the way to its corner (0x0079c1a0).
    za::searchBuildingLiquid(water.at(1, 10, 10), {10.0f, 10.0f, 5.0f}, found);
    REQUIRE(found.size() == 1);
    CHECK(found[0].liquidType == 1);
    CHECK(found[0].offset.x == Catch::Approx(2 * T - 10.0f));
    CHECK(found[0].offset.y == Catch::Approx(2 * T - 10.0f));
    CHECK(found[0].offset.z == Catch::Approx(2.0f));

    // A hole there: the ring of nine. (1,2) is kept first, (3,2) is further
    // than 2 up and a tile across make and replaces it, and the rest are
    // not further than that.
    water.flags[2 * 10 + 2] = 0x0f;
    found.clear();
    za::searchBuildingLiquid(water.at(1, 10, 10), {10.0f, 10.0f, 5.0f}, found);
    REQUIRE(found.size() == 1);
    CHECK(found[0].offset.x == Catch::Approx(3 * T - 10.0f));
    CHECK(found[0].offset.y == Catch::Approx(2 * T - 10.0f));
}

TEST_CASE("a building's liquid is found far off, and one entry per type", "[audio][ambience]") {
    constexpr float T = za::kBuildingLiquidTile;
    const TestLiquid pool(3, 1, 0.0f);
    std::vector<za::BuildingLiquidFound> found;

    // Thirty tiles off - 125 yards, past the ground's 60 - and still found.
    const glm::vec3 far(-30.0f * T - 1.0f, 1.0f, 0.0f);
    za::searchBuildingLiquid(pool.at(5, 3, 1), far, found);
    REQUIRE(found.size() == 1);
    CHECK(found[0].offset.x == Catch::Approx(30.0f * T + 1.0f));
    CHECK(found[0].offset.y == Catch::Approx(-1.0f));

    // Sixty-one tiles off is past the last disc.
    found.clear();
    za::searchBuildingLiquid(pool.at(5, 3, 1), {-61.0f * T - 1.0f, 1.0f, 0.0f}, found);
    CHECK(found.empty());

    // Another group of the same type further off keeps what is there; one
    // of another type adds its own.
    const TestLiquid near(4, 4, 0.0f);
    found.clear();
    za::searchBuildingLiquid(near.at(5, 4, 4), {1.0f, 1.0f, 0.0f}, found);
    za::searchBuildingLiquid(pool.at(5, 3, 1), far, found);
    za::searchBuildingLiquid(pool.at(6, 3, 1), far, found);
    REQUIRE(found.size() == 2);
    CHECK(found[0].liquidType == 5);
    CHECK(found[0].offset.x == Catch::Approx(-1.0f));
    CHECK(found[1].liquidType == 6);
    CHECK(found[1].offset.x == Catch::Approx(30.0f * T + 1.0f));

    // No type, or no tiles: nothing is asked.
    found.clear();
    za::searchBuildingLiquid(pool.at(0, 3, 1), far, found);
    CHECK(found.empty());
}

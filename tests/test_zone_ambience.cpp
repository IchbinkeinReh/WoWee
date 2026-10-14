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

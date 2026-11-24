// Tests for the extracted world map coordinate projection module
#include <catch_amalgamated.hpp>
#include "core/coordinates.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "rendering/world_map/world_map_types.hpp"

#include <glm/glm.hpp>
#include <cmath>
#include <vector>

using namespace wowee::rendering::world_map;

// ── Helper: build a minimal zone for testing ─────────────────

static Zone makeZone(uint32_t wmaID, uint32_t areaID,
                     float locLeft, float locRight,
                     float locTop, float locBottom,
                     uint32_t displayMapID = 0,
                     uint32_t parentWorldMapID = 0,
                     const std::string& name = "") {
    Zone z;
    z.wmaID = wmaID;
    z.areaID = areaID;
    z.areaName = name;
    z.bounds.locLeft = locLeft;
    z.bounds.locRight = locRight;
    z.bounds.locTop = locTop;
    z.bounds.locBottom = locBottom;
    z.displayMapID = displayMapID;
    z.parentWorldMapID = parentWorldMapID;
    return z;
}

// ── renderPosToMapUV ─────────────────────────────────────────

TEST_CASE("renderPosToMapUV: center of zone maps to (0.5, ~0.5)", "[world_map][coordinate_projection]") {
    ZoneBounds bounds;
    bounds.locLeft = 1000.0f;
    bounds.locRight = -1000.0f;
    bounds.locTop = 1000.0f;
    bounds.locBottom = -1000.0f;

    // renderPos.y = wowX, renderPos.x = wowY
    glm::vec3 center(0.0f, 0.0f, 0.0f);
    glm::vec2 uv = renderPosToMapUV(center, bounds, /*isContinent=*/false);
    REQUIRE(std::abs(uv.x - 0.5f) < 0.01f);
    REQUIRE(std::abs(uv.y - 0.5f) < 0.01f);
}

TEST_CASE("renderPosToMapUV: degenerate bounds returns (0.5, 0.5)", "[world_map][coordinate_projection]") {
    ZoneBounds bounds{};  // all zeros
    glm::vec3 pos(100.0f, 200.0f, 0.0f);
    glm::vec2 uv = renderPosToMapUV(pos, bounds, false);
    REQUIRE(uv.x == Catch::Approx(0.5f));
    REQUIRE(uv.y == Catch::Approx(0.5f));
}

TEST_CASE("renderPosToMapUV: top-left corner maps to (0, ~0)", "[world_map][coordinate_projection]") {
    ZoneBounds bounds;
    bounds.locLeft = 1000.0f;
    bounds.locRight = -1000.0f;
    bounds.locTop = 1000.0f;
    bounds.locBottom = -1000.0f;

    // wowX = renderPos.y = locLeft = 1000, wowY = renderPos.x = locTop = 1000
    glm::vec3 topLeft(1000.0f, 1000.0f, 0.0f);
    glm::vec2 uv = renderPosToMapUV(topLeft, bounds, false);
    REQUIRE(uv.x == Catch::Approx(0.0f).margin(0.01f));
    REQUIRE(uv.y == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("renderPosToMapUV: continent applies vertical offset", "[world_map][coordinate_projection]") {
    ZoneBounds bounds;
    bounds.locLeft = 1000.0f;
    bounds.locRight = -1000.0f;
    bounds.locTop = 1000.0f;
    bounds.locBottom = -1000.0f;

    glm::vec3 center(0.0f, 0.0f, 0.0f);
    glm::vec2 zone_uv = renderPosToMapUV(center, bounds, false);
    glm::vec2 cont_uv = renderPosToMapUV(center, bounds, true);

    // No vertical offset - continent and zone UV should be identical
    REQUIRE(zone_uv.x == Catch::Approx(cont_uv.x).margin(0.01f));
    REQUIRE(cont_uv.y == Catch::Approx(zone_uv.y).margin(0.01f));
}

// ── zoneBelongsToContinent ───────────────────────────────────

// Dalaran, whose parent is the zone below it, opened the continent.
TEST_CASE("a zone is its continent's by the map it is shown on", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(485, 0, 9217.0f, -8534.0f, 10593.0f, -1240.0f, 571, 0, "Northrend"));
    zones.push_back(makeZone(510, 2817, 1443.0f, -1279.0f, 6502.0f, 4687.0f, 571, 0, "CrystalsongForest"));
    zones.push_back(makeZone(504, 4395, 0.0f, 0.0f, 0.0f, 0.0f, 571, 510, "Dalaran"));
    zones.push_back(makeZone(30, 12, 1535.0f, -1935.0f, -7939.0f, -10254.0f, 0, 0, "Elwynn"));
    zones[0].mapID = zones[1].mapID = zones[2].mapID = 571;
    CHECK(zoneBelongsToContinent(zones, 1, 0));
    CHECK(zoneBelongsToContinent(zones, 2, 0));
    CHECK_FALSE(zoneBelongsToContinent(zones, 3, 0));
    CHECK_FALSE(zoneBelongsToContinent(zones, 0, 0));
    CHECK_FALSE(zoneBelongsToContinent(zones, 2, 1));
}

TEST_CASE("a floor's map spans the floor", "[world_map][coordinate_projection]") {
    Zone dalaran = makeZone(504, 4395, 1052.5f, 222.5f, 6066.7f, 5513.3f, 571, 510, "Dalaran");
    dalaran.dungeonFloors = {{27, 1, 504, {1052.5f, 222.5f, 6066.7f, 5513.3f}},
                             {26, 2, 504, {915.9f, 352.6f, 5975.3f, 5599.9f}}};
    CHECK(shownBounds(dalaran, 27).locLeft == 1052.5f);
    CHECK(shownBounds(dalaran, 26).locLeft == 915.9f);
    CHECK(shownBounds(dalaran, 26).locBottom == 5599.9f);
    CHECK(shownBounds(dalaran, -1).locLeft == 1052.5f);
}

// On a zone's map the player, the party, a corpse and the quests' places were
// at the continent's fraction of it.
TEST_CASE("what is on a zone's map is placed by the zone's rectangle", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(13, 0, 17066.6f, -19733.21f, 12799.9f, -11733.3f, 1, 0, "Kalimdor"));
    zones.push_back(makeZone(9, 215, 2047.917f, -3089.583f, -272.917f, -3697.917f, 1, 0, "Mulgore"));
    const glm::vec3 place = wowee::core::coords::canonicalToRender(
        wowee::core::coords::serverToCanonical(glm::vec3(-2200.0f, -900.0f, 0.0f)));
    const glm::vec2 onZone = renderPosToMapUV(place, mapBounds(zones, 1, 0, 0), false);
    CHECK(onZone.x == Catch::Approx(0.5738f).margin(0.001f));
    CHECK(onZone.y == Catch::Approx(0.5626f).margin(0.001f));
    const glm::vec2 onContinent = renderPosToMapUV(place, mapBounds(zones, 0, 0, 0), true);
    CHECK(onContinent.x == Catch::Approx(0.4882f).margin(0.001f));
    CHECK(onContinent.y == Catch::Approx(0.6114f).margin(0.001f));
}

TEST_CASE("zoneBelongsToContinent: out of bounds returns false", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 0, 5000.0f, -5000.0f, 5000.0f, -5000.0f));
    REQUIRE(zoneBelongsToContinent(zones, -1, 0) == false);
    REQUIRE(zoneBelongsToContinent(zones, 5, 0) == false);
}

// ── isRootContinent / isLeafContinent ────────────────────────

TEST_CASE("isRootContinent detects root with leaf children", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 0, 5000.0f, -5000.0f, 5000.0f, -5000.0f, 0, 0, "Root"));
    zones.push_back(makeZone(2, 0, 3000.0f, -3000.0f, 3000.0f, -3000.0f, 0, 1, "Leaf"));

    REQUIRE(isRootContinent(zones, 0) == true);
    REQUIRE(isLeafContinent(zones, 1) == true);
    REQUIRE(isRootContinent(zones, 1) == false);
    REQUIRE(isLeafContinent(zones, 0) == false);
}

TEST_CASE("isRootContinent: lone continent is not root (no children)", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 0, 5000.0f, -5000.0f, 5000.0f, -5000.0f, 0, 0, "Solo"));
    REQUIRE(isRootContinent(zones, 0) == false);
}

TEST_CASE("isRootContinent: out of bounds returns false", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    REQUIRE(isRootContinent(zones, 0) == false);
    REQUIRE(isRootContinent(zones, -1) == false);
    REQUIRE(isLeafContinent(zones, 0) == false);
}

// ── findZoneForPlayer ────────────────────────────────────────

TEST_CASE("findZoneForPlayer: finds smallest containing zone", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    // Continent (ignored: areaID == 0)
    zones.push_back(makeZone(1, 0, 10000.0f, -10000.0f, 10000.0f, -10000.0f, 0, 0, "Cont"));
    // Large zone
    zones.push_back(makeZone(2, 100, 5000.0f, -5000.0f, 5000.0f, -5000.0f, 0, 1, "Large"));
    // Small zone fully inside large
    zones.push_back(makeZone(3, 200, 1000.0f, -1000.0f, 1000.0f, -1000.0f, 0, 1, "Small"));

    // Player at center - should find the smaller zone
    glm::vec3 playerPos(0.0f, 0.0f, 0.0f);
    int found = findZoneForPlayer(zones, playerPos);
    REQUIRE(found == 2);  // Small zone
}

TEST_CASE("findZoneForPlayer: returns -1 when no zone contains position", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 100, 100.0f, -100.0f, 100.0f, -100.0f, 0, 0, "Tiny"));

    glm::vec3 farAway(9999.0f, 9999.0f, 0.0f);
    REQUIRE(findZoneForPlayer(zones, farAway) == -1);
}

// ── getContinentProjectionBounds ─────────────────────────────

TEST_CASE("getContinentProjectionBounds: uses continent's own bounds if available", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 0, 5000.0f, -5000.0f, 3000.0f, -3000.0f, 0, 0, "EK"));

    float l, r, t, b;
    bool ok = getContinentProjectionBounds(zones, 0, l, r, t, b);
    REQUIRE(ok == true);
    REQUIRE(l == Catch::Approx(5000.0f));
    REQUIRE(r == Catch::Approx(-5000.0f));
    REQUIRE(t == Catch::Approx(3000.0f));
    REQUIRE(b == Catch::Approx(-3000.0f));
}

TEST_CASE("getContinentProjectionBounds: returns false for out of bounds", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    float l, r, t, b;
    REQUIRE(getContinentProjectionBounds(zones, 0, l, r, t, b) == false);
    REQUIRE(getContinentProjectionBounds(zones, -1, l, r, t, b) == false);
}

TEST_CASE("getContinentProjectionBounds: rejects non-continent zones", "[world_map][coordinate_projection]") {
    std::vector<Zone> zones;
    zones.push_back(makeZone(1, 100, 5000.0f, -5000.0f, 3000.0f, -3000.0f, 0, 0, "Zone"));
    float l, r, t, b;
    bool ok = getContinentProjectionBounds(zones, 0, l, r, t, b);
    REQUIRE(ok == false);
}

// ── findZoneForPlayer: the server's zone beats the geometry ──────

// The map opened on whichever WorldMapArea box the player sat deepest inside.
// Those boxes are axis-aligned rectangles around irregular zones, so neighbours
// overlap heavily and the deepest-inside answer is only ever a guess - hence a
// map that opened on a zone the player was merely near. The server tells us the
// zone outright; these pin that it is used, and that the guess still stands in
// when it has not said.
//
// findZoneForPlayer takes render-space, where renderX = wowY and renderY = wowX.
static glm::vec3 atWow(float wowX, float wowY) { return glm::vec3(wowY, wowX, 0.0f); }

TEST_CASE("findZoneForPlayer: the server's zone id wins over the geometry",
          "[world_map][coordinate_projection]") {
    std::vector<Zone> zones = {
        // A big zone the player is standing deep inside.
        makeZone(1, 100, 1000.0f, -1000.0f, 1000.0f, -1000.0f, 0, 0, "Big"),
        // A neighbour whose box reaches over the same spot.
        makeZone(2, 200, 200.0f, -200.0f, 200.0f, -200.0f, 0, 0, "Neighbour"),
    };
    const glm::vec3 pos = atWow(0.0f, 0.0f);

    // With nothing from the server the geometry picks the tighter, more central box.
    REQUIRE(findZoneForPlayer(zones, pos, 0) == 1);

    // Told which zone the player is in, it says so regardless of the boxes.
    REQUIRE(findZoneForPlayer(zones, pos, 100) == 0);
    REQUIRE(findZoneForPlayer(zones, pos, 200) == 1);
}

TEST_CASE("findZoneForPlayer: a zone id not on this map falls back to geometry",
          "[world_map][coordinate_projection]") {
    // Standing on one continent with the map showing another, the id names no
    // zone here. Guessing is better than answering "nowhere".
    std::vector<Zone> zones = {
        makeZone(1, 100, 500.0f, -500.0f, 500.0f, -500.0f, 0, 0, "Here"),
    };
    REQUIRE(findZoneForPlayer(zones, atWow(0.0f, 0.0f), 999999) == 0);
}

TEST_CASE("findZoneForPlayer: the server's zone is honoured outside its own box",
          "[world_map][coordinate_projection]") {
    // The point of the fix. WorldMapArea bounds do not cover every part of a
    // zone, so a player near an edge could fall inside a neighbour's box only.
    // The id must still win - that is what "sort of close to" was.
    std::vector<Zone> zones = {
        makeZone(1, 100, 10.0f, -10.0f, 10.0f, -10.0f, 0, 0, "Real zone"),
        makeZone(2, 200, 1000.0f, -1000.0f, 1000.0f, -1000.0f, 0, 0, "Neighbour"),
    };
    const glm::vec3 outsideReal = atWow(500.0f, 500.0f);

    REQUIRE(findZoneForPlayer(zones, outsideReal, 0) == 1);   // geometry: neighbour
    REQUIRE(findZoneForPlayer(zones, outsideReal, 100) == 0); // server: real zone
}

TEST_CASE("findZoneByAreaId: exact, and -1 for unknown or zero",
          "[world_map][coordinate_projection]") {
    std::vector<Zone> zones = {
        makeZone(1, 0,   100.0f, -100.0f, 100.0f, -100.0f, 0, 0, "Continent"),
        makeZone(2, 331, 100.0f, -100.0f, 100.0f, -100.0f, 0, 0, "Ashenvale"),
    };
    REQUIRE(findZoneByAreaId(zones, 331) == 1);
    REQUIRE(findZoneByAreaId(zones, 12345) == -1);
    // 0 is "no zone", not the continent entry, whose areaID is also 0.
    REQUIRE(findZoneByAreaId(zones, 0) == -1);
}

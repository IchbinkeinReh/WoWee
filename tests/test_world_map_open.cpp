// Tests for what the world map opens on and how it lays out its art: reading
// WorldMapArea's DisplayMapID as the client does, the tile and overlay layout
// of a 1002x668 map, the zone a position falls in, the Map.dbc name column and
// the full-screen scale.
#include <catch_amalgamated.hpp>
#include "rendering/world_map/data_repository.hpp"
#include "rendering/world_map/map_opening.hpp"
#include "rendering/world_map/map_resolver.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "rendering/world_map/world_map_types.hpp"
#include "ui/fullscreen_scale.hpp"
#include "core/coordinates.hpp"

#include <cstdint>

using namespace wowee::rendering::world_map;

namespace {

Zone zoneRow(uint32_t mapId, uint32_t areaId, float left, float right, float top, float bottom) {
    Zone z;
    z.mapID = mapId;
    z.areaID = areaId;
    z.bounds.locLeft = left;
    z.bounds.locRight = right;
    z.bounds.locTop = top;
    z.bounds.locBottom = bottom;
    return z;
}

/// The zone at a position as the server sends it - north, then west - by way
/// of the client's own conversion, which is how the loading screen asks.
uint32_t zoneAtWire(const std::vector<Zone>& rows, uint32_t mapId, float north, float west) {
    const glm::vec3 canonical =
        wowee::core::coords::serverToCanonical(glm::vec3(north, west, 0.0f));
    return zoneAreaAtPosition(rows, mapId, canonical.x, canonical.y);
}

}  // namespace

// ── DisplayMapID ─────────────────────────────────────────────

TEST_CASE("worldMapDisplayMap: -1 is the row's own map", "[world_map][open]") {
    // Elwynn, Durotar, Hellfire: every ordinary row of 3.3.5's file. Read as a
    // map id, -1 named the "World" view and the map opened on nothing.
    REQUIRE(worldMapDisplayMap(0, UINT32_MAX, true) == 0u);
    REQUIRE(worldMapDisplayMap(1, UINT32_MAX, true) == 1u);
    REQUIRE(worldMapDisplayMap(530, UINT32_MAX, true) == 530u);
}

TEST_CASE("worldMapDisplayMap: zero is the Eastern Kingdoms where -1 means none",
          "[world_map][open]") {
    // Eversong Woods: map 530 terrain, drawn on the Eastern Kingdoms.
    REQUIRE(worldMapDisplayMap(530, 0, true) == 0u);
    // Azuremyst Isle: map 530 terrain, drawn on Kalimdor.
    REQUIRE(worldMapDisplayMap(530, 1, true) == 1u);
}

TEST_CASE("worldMapDisplayMap: a file without the column moves nothing",
          "[world_map][open]") {
    // Vanilla's file has no such field and reads as zero throughout; there a
    // Kalimdor row is not on the Eastern Kingdoms.
    REQUIRE(worldMapDisplayMap(1, 0, false) == 1u);
    REQUIRE(worldMapDisplayMap(0, 0, false) == 0u);
}

// ── Tiles and overlays ───────────────────────────────────────

TEST_CASE("worldMapTilePath: twelve tiles named after the folder", "[world_map][open]") {
    REQUIRE(worldMapTilePath("Elwynn", 0, 1) == "Interface\\WorldMap\\Elwynn\\Elwynn1.blp");
    REQUIRE(worldMapTilePath("Elwynn", 0, 12) == "Interface\\WorldMap\\Elwynn\\Elwynn12.blp");
}

TEST_CASE("worldMapTilePath: a dungeon floor names its tiles by floor", "[world_map][open]") {
    REQUIRE(worldMapTilePath("Dalaran", 1, 1) == "Interface\\WorldMap\\Dalaran\\Dalaran1_1.blp");
    REQUIRE(worldMapTilePath("UtgardeKeep", 3, 12) ==
            "Interface\\WorldMap\\UtgardeKeep\\UtgardeKeep3_12.blp");
}

TEST_CASE("overlayPiece: pieces of 256 across then down, the last ones partial",
          "[world_map][open]") {
    OverlayEntry ov;
    ov.texWidth = 300;
    ov.texHeight = 200;
    ov.offsetX = 10;
    ov.offsetY = 20;
    ov.tileCols = 2;
    ov.tileRows = 1;

    const OverlayPiece first = overlayPiece(ov, 0);
    REQUIRE(first.x == 10);
    REQUIRE(first.y == 20);
    REQUIRE(first.width == 256);
    REQUIRE(first.height == 200);
    REQUIRE(first.uMax == Catch::Approx(1.0f));
    // 200 high in a file of 256.
    REQUIRE(first.vMax == Catch::Approx(200.0f / 256.0f));

    const OverlayPiece second = overlayPiece(ov, 1);
    REQUIRE(second.x == 266);
    REQUIRE(second.y == 20);
    REQUIRE(second.width == 44);
    // 44 across in a file of 64.
    REQUIRE(second.uMax == Catch::Approx(44.0f / 64.0f));

    // There is no third piece.
    REQUIRE(overlayPiece(ov, 2).width == 0);
}

TEST_CASE("overlayPiece: a second row starts 256 below the first", "[world_map][open]") {
    OverlayEntry ov;
    ov.texWidth = 512;
    ov.texHeight = 260;
    ov.offsetX = 0;
    ov.offsetY = 100;
    ov.tileCols = 2;
    ov.tileRows = 2;

    const OverlayPiece belowLeft = overlayPiece(ov, 2);
    REQUIRE(belowLeft.x == 0);
    REQUIRE(belowLeft.y == 356);
    REQUIRE(belowLeft.width == 256);
    REQUIRE(belowLeft.height == 4);
    // Four high, in the smallest file there is.
    REQUIRE(belowLeft.vMax == Catch::Approx(4.0f / 16.0f));
}

// ── The zone a position is in ────────────────────────────────

TEST_CASE("zoneAreaAtPosition: Eversong is on map 530 and is not Outland",
          "[world_map][open]") {
    std::vector<Zone> rows;
    rows.push_back(zoneRow(530, 0, 12996.04f, -4468.04f, 5821.36f, -5821.36f));      // Outland
    rows.push_back(zoneRow(530, 3483, 5539.58f, 375.0f, 1481.25f, -1962.5f));         // Hellfire
    rows.push_back(zoneRow(530, 3430, -4487.5f, -9412.5f, 11041.67f, 7758.33f));      // Eversong
    rows.push_back(zoneRow(0, 12, 1535.42f, -1935.42f, -7939.58f, -10254.17f));       // Elwynn

    // A blood elf at home.
    REQUIRE(zoneAtWire(rows, 530, 9500.0f, -6800.0f) == 3430u);
    REQUIRE(zoneAtWire(rows, 530, 0.0f, 3000.0f) == 3483u);
}

TEST_CASE("zoneAreaAtPosition: only rows of that map, and none outside them",
          "[world_map][open]") {
    std::vector<Zone> rows;
    rows.push_back(zoneRow(0, 12, 1535.42f, -1935.42f, -7939.58f, -10254.17f));       // Elwynn
    // Goldshire.
    REQUIRE(zoneAtWire(rows, 0, -9464.0f, 62.0f) == 12u);
    // The same spot on another map is not Elwynn.
    REQUIRE(zoneAtWire(rows, 1, -9464.0f, 62.0f) == 0u);
    REQUIRE(zoneAtWire(rows, 0, 5000.0f, 5000.0f) == 0u);
}

// ── Map.dbc ──────────────────────────────────────────────────

TEST_CASE("mapDbcNameField: 3.3.5's name is past its Flags column", "[world_map][open]") {
    REQUIRE(mapDbcNameField(66) == 5u);
    // Vanilla's and TBC's files have no Flags column.
    REQUIRE(mapDbcNameField(42) == 4u);
    REQUIRE(mapDbcNameField(53) == 4u);
}

// ── The full-size map's scale ────────────────────────────────

TEST_CASE("fullscreenFrameScale: the guide fills the screen's height",
          "[world_map][open]") {
    using wowee::ui::fullscreenFrameScale;
    // Interface scale 1 on a 4:3 screen: the root is the guide.
    REQUIRE(fullscreenFrameScale(1024.0f, 768.0f) == Catch::Approx(1.0f));
    // A frame without a parent is in a screen 768 high whatever the interface
    // scale (T448): at 0.9 on 1920x1032 the root is 853.33 high, and the map
    // keeps 1 - not 853.33 / 768, which made it a ninth too tall.
    REQUIRE(fullscreenFrameScale(1587.6f, 853.33f) == Catch::Approx(1.0f));
    REQUIRE(fullscreenFrameScale(1706.67f, 960.0f) == Catch::Approx(1.0f));
    REQUIRE(fullscreenFrameScale(1066.67f, 600.0f) == Catch::Approx(1.0f));
}

TEST_CASE("fullscreenFrameScale: narrower than 4:3 fits the width, as the client's",
          "[world_map][open]") {
    using wowee::ui::fullscreenFrameScale;
    // 5:4 at interface scale 1: the client's min(1, aspect * 0.75).
    REQUIRE(fullscreenFrameScale(960.0f, 768.0f) == Catch::Approx(1.25f * 0.75f));
    REQUIRE(fullscreenFrameScale(0.0f, 768.0f) == Catch::Approx(1.0f));
}

// ── Where an instance without a map of its own opens ─────────

TEST_CASE("an instance opens where its entrance is shown", "[world_map][open]") {
    using Entrance = DataRepository::Entrance;
    // WorldMapTransforms.dbc rows 2 and 3.
    const std::vector<wowee::game::WorldMapTransform> transforms = {
        {530, 4800.0f, -10133.33f, 16000.0f, -2666.67f, 0, -2400.0f, 2400.0f},
        {530, -6933.33f, -16000.0f, 533.33f, -8000.0f, 1, 10133.33f, 17600.0f},
    };
    const auto entrance = [](uint32_t mapId, float x, float y) {
        Entrance e;
        e.valid = true;
        e.mapId = mapId;
        e.x = x;
        e.y = y;
        return e;
    };
    // Magisters' Terrace (585): Quel'Danas, on the Eastern Kingdoms' map.
    const Entrance terrace = DataRepository::entranceShown(transforms, entrance(530, 12881.9f, -7343.1f));
    CHECK(terrace.valid);
    CHECK(terrace.mapId == 0);
    CHECK(terrace.x == Catch::Approx(10481.9f));
    CHECK(terrace.y == Catch::Approx(-4943.1f));
    // Shadow Labyrinth's is Outland's own, the Deadmines' nobody's to move.
    CHECK(DataRepository::entranceShown(transforms, entrance(530, -3640.0f, 4943.0f)).mapId == 530);
    CHECK(DataRepository::entranceShown(transforms, entrance(0, -11208.0f, 1672.0f)).x == Catch::Approx(-11208.0f));
    CHECK_FALSE(DataRepository::entranceShown(transforms, Entrance{}).valid);
}

// 0x00547170: SetMapToCurrentZone sets the map at once, and the last call counts.
TEST_CASE("a map the interface has set stays where it is", "[world_map]") {
    MapOpening m;
    m.inInterface = true;
    m.placed = true;
    m.firstTime = true;
    m.afterGap = true;
    CHECK_FALSE(opensOnPlayer(m));
    // Nobody has set it, it is asked for, or there is none.
    m.placed = false;
    CHECK(opensOnPlayer(m));
    m.placed = true;
    m.asked = true;
    CHECK(opensOnPlayer(m));
    m.asked = false;
    m.nothingLoaded = true;
    CHECK(opensOnPlayer(m));
}

TEST_CASE("the client's own map opens on the player", "[world_map]") {
    MapOpening m;
    m.placed = true;
    CHECK_FALSE(opensOnPlayer(m));
    m.afterGap = true;
    CHECK(opensOnPlayer(m));
    m.afterGap = false;
    m.firstTime = true;
    CHECK(opensOnPlayer(m));
}

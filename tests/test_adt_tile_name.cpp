// The map and tile an ADT's path names (T332).
//
// The first tile of a world entry was found by cutting the file name at its
// FIRST underscore. Maps whose own name holds one (Azjol_LowerCity,
// Azjol_Uppercity, the Transport_* maps) then read a word as the tile's X,
// failed, and asked for the default tile [32,49], which they do not have.
#include <catch_amalgamated.hpp>

#include <string>

#include "pipeline/adt_tile_name.hpp"

using wowee::pipeline::AdtTileName;
using wowee::pipeline::parseAdtTileName;

TEST_CASE("A map's name may hold underscores: the tile is the last two fields", "[adt_tile_name]") {
    AdtTileName t;
    REQUIRE(parseAdtTileName("World\\Maps\\Azjol_LowerCity\\Azjol_LowerCity_34_28.adt", t));
    CHECK(t.mapName == "Azjol_LowerCity");
    CHECK(t.tileX == 34);
    CHECK(t.tileY == 28);

    REQUIRE(parseAdtTileName("World/Maps/Transport_Tirisfal_Grom/Transport_Tirisfal_Grom_0_63.ADT", t));
    CHECK(t.mapName == "Transport_Tirisfal_Grom");
    CHECK(t.tileX == 0);
    CHECK(t.tileY == 63);
}

TEST_CASE("A plain map's tile as before", "[adt_tile_name]") {
    AdtTileName t;
    REQUIRE(parseAdtTileName("World\\Maps\\Azeroth\\Azeroth_32_49.adt", t));
    CHECK(t.mapName == "Azeroth");
    CHECK(t.tileX == 32);
    CHECK(t.tileY == 49);
    REQUIRE(parseAdtTileName("Kalimdor_40_30.adt", t));
    CHECK(t.mapName == "Kalimdor");
}

TEST_CASE("What is not <Map>_<X>_<Y>.adt is refused and leaves the result alone", "[adt_tile_name]") {
    AdtTileName t;
    t.mapName = "kept";
    t.tileX = 7;
    t.tileY = 9;
    CHECK_FALSE(parseAdtTileName("World\\Maps\\Azeroth\\Azeroth_32_49_obj0.adt", t));
    CHECK_FALSE(parseAdtTileName("World\\Maps\\Azeroth\\Azeroth_32.adt", t));
    CHECK_FALSE(parseAdtTileName("World\\Maps\\Azeroth\\Azeroth_32_49.wdt", t));
    CHECK_FALSE(parseAdtTileName("World\\Maps\\Azeroth\\Azeroth_64_1.adt", t));
    CHECK_FALSE(parseAdtTileName("_1_2.adt", t));
    CHECK(t.mapName == "kept");
    CHECK(t.tileX == 7);
    CHECK(t.tileY == 9);
}

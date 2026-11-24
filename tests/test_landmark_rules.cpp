// Which of AreaPOI's points a map shows (rendering/world_map/landmark_rules.hpp,
// 0x00546310 -> 0x005445f0, 0x00545a30). Every point on the continent was listed
// on a zone's map, each with its icon.
#include <catch_amalgamated.hpp>

#include "rendering/world_map/landmark_rules.hpp"

using namespace wowee::rendering::world_map;

TEST_CASE("a map takes the points flagged for its kind", "[landmark]") {
    // Bloodhoof Village (0x205): on a zone's map and the battle map, not on
    // Kalimdor's. The Crossroads (0x20d) on both.
    CHECK(landmarkOnMap(0x205, LandmarkMap::Zone, false));
    CHECK_FALSE(landmarkOnMap(0x205, LandmarkMap::Continent, false));
    CHECK(landmarkOnMap(0x20d, LandmarkMap::Continent, false));
    CHECK_FALSE(landmarkOnMap(0x20d, LandmarkMap::World, false));
    CHECK(landmarkOnMap(0x21d, LandmarkMap::World, false));
    // The world's map is a zoneless one too (0x8) besides its own bit.
    CHECK_FALSE(landmarkOnMap(0x10, LandmarkMap::World, false));
    CHECK_FALSE(landmarkOnMap(0x8, LandmarkMap::Zone, false));
    CHECK(landmarkOnMap(0x4, LandmarkMap::Other, false));
    // 0x400: only its own area's map.
    CHECK_FALSE(landmarkOnMap(0x404, LandmarkMap::Zone, false));
    CHECK(landmarkOnMap(0x404, LandmarkMap::Zone, true));
}

TEST_CASE("a point of an area shows once the area is explored", "[landmark]") {
    CHECK(landmarkKnown(0, std::nullopt, false));
    CHECK_FALSE(landmarkKnown(222, 10, false));
    CHECK(landmarkKnown(222, 10, true));
    CHECK(landmarkKnown(222, 0, true));
    CHECK_FALSE(landmarkKnown(222, 0, false));
    CHECK(landmarkKnown(222, -1, false));
    CHECK_FALSE(landmarkKnown(999999, std::nullopt, true));
}

TEST_CASE("its icon is its own on a continent and where 0x80 has it on a zone", "[landmark]") {
    const std::array<int32_t, 9> icons = {7, 11, 12, 0, 0, 0, 0, 0, 19};
    CHECK(landmarkIcon(0x205, LandmarkMap::Zone, icons, 0, 0) == 0);
    CHECK(landmarkIcon(0x285, LandmarkMap::Zone, icons, 0, 0) == 7);
    CHECK(landmarkIcon(0x20d, LandmarkMap::Continent, icons, 0, 0) == 7);
    // A state's value picks it, from one.
    CHECK(landmarkIcon(0x80, LandmarkMap::Other, icons, 3601, 2) == 11);
    CHECK(landmarkIcon(0x80, LandmarkMap::Other, icons, 3601, 9) == 19);
    CHECK(landmarkIcon(0x80, LandmarkMap::Other, icons, 3601, 10) == 7);
    CHECK(landmarkStateSet(0, 0));
    CHECK_FALSE(landmarkStateSet(3601, 0));
    CHECK(landmarkStateSet(3601, 1));
}

TEST_CASE("Azeroth's map has a place by its continent's offset and scale (0x00544140)", "[landmark]") {
    // The frame's middle for a continent's origin with no offset; south of it
    // is down, west of it left.
    const LandmarkPoint origin = landmarkOnWorld(0.0f, 0.0f, 0.7f, 0.0f, 0.0f);
    CHECK(origin.x == Catch::Approx(0.5f));
    CHECK(origin.y == Catch::Approx(0.5f));
    const LandmarkPoint southWest = landmarkOnWorld(0.0f, 0.0f, 0.7f, -1000.0f, 1000.0f);
    CHECK(southWest.x < origin.x);
    CHECK(southWest.y > origin.y);
    // An offset of a grid cell is 16 of the frame's 1002 by 668 pixels.
    const LandmarkPoint moved = landmarkOnWorld(1.0f, 1.0f, 0.7f, 0.0f, 0.0f);
    CHECK((moved.x - origin.x) * 1002.0f == Catch::Approx(16.0f));
    CHECK((moved.y - origin.y) * 668.0f == Catch::Approx(16.0f));
}

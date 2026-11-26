// The minimap's zoom levels as the client has them (0x007f3b60, 0x007f3ae0,
// 0x007f3b90, 0x00a41e04, 0x00a41e1c).
#include <catch_amalgamated.hpp>

#include "rendering/minimap_zoom.hpp"

namespace mz = wowee::rendering::minimap_zoom;

TEST_CASE("six levels, starting at 3", "[minimap_zoom]") {
    CHECK(mz::kLevels == 6);
    CHECK(mz::kDefaultLevel == 3);
}

TEST_CASE("outdoors the map reaches half its width in chunks", "[minimap_zoom]") {
    CHECK(mz::radius(0) == Catch::Approx(233.33333f));
    CHECK(mz::radius(1) == Catch::Approx(200.0f));
    CHECK(mz::radius(2) == Catch::Approx(166.66667f));
    CHECK(mz::radius(3) == Catch::Approx(133.33333f));
    CHECK(mz::radius(4) == Catch::Approx(100.0f));
    CHECK(mz::radius(5) == Catch::Approx(66.66667f));
}

TEST_CASE("indoors the levels read their own table", "[minimap_zoom]") {
    CHECK(mz::radius(0, true) == 150.0f);
    CHECK(mz::radius(3, true) == 60.0f);
    CHECK(mz::radius(5, true) == 25.0f);
}

TEST_CASE("a level past 4, or below 0, is kept as 5", "[minimap_zoom]") {
    CHECK(mz::clampLevel(0) == 0);
    CHECK(mz::clampLevel(4) == 4);
    CHECK(mz::clampLevel(5) == 5);
    CHECK(mz::clampLevel(9) == 5);
    CHECK(mz::clampLevel(-1) == 5);
}

namespace {
// The frame's side of it, as the widget keeps it.
struct Frame {
    int level = mz::kDefaultLevel;
    bool indoors = false;
    int savedInside = -1;
    int savedOutside = -1;
    mz::FrameZoom zoom() { return {level, indoors, savedInside, savedOutside}; }
};
}  // namespace

TEST_CASE("a /reload indoors keeps the indoor level (T368)", "[minimap_zoom]") {
    // Gryshka's inn: zoomed out once indoors, inside 4, outside 3.
    bool wasIndoors = true;
    int mapLevel = 4;  // the indoor map's, in use
    // The frame made anew by the /reload, its CVars read back.
    Frame f;
    f.savedOutside = 3;
    f.savedInside = 4;
    CHECK(mz::syncFrame(f.zoom(), wasIndoors, true, mapLevel));
    CHECK(f.level == 4);  // GetZoom: the indoor level
    CHECK(f.indoors);
    CHECK(mapLevel == 4);
    CHECK(f.savedInside == -1);
    CHECK(f.savedOutside == 3);  // waits for the outdoor map
    // Nothing more to do while nothing changes.
    CHECK_FALSE(mz::syncFrame(f.zoom(), wasIndoors, true, mapLevel));
    // Out: the outdoor map's level, from the saved one.
    int outdoorLevel = 3;
    CHECK(mz::syncFrame(f.zoom(), wasIndoors, false, outdoorLevel));
    CHECK(f.level == 3);
    CHECK_FALSE(f.indoors);
    CHECK(f.savedOutside == -1);
}

TEST_CASE("a login indoors takes the saved indoor level, however late it comes (T368)", "[minimap_zoom]") {
    bool wasIndoors = false;
    int mapLevel = mz::kDefaultLevel;
    Frame f;
    CHECK(mz::syncFrame(f.zoom(), wasIndoors, true, mapLevel));  // in: the map's level
    f.savedInside = 2;  // the CVars read back after the first frames
    CHECK(mz::syncFrame(f.zoom(), wasIndoors, true, mapLevel));
    CHECK(mapLevel == 2);
    CHECK(f.level == 2);
}

TEST_CASE("the frame's own SetZoom moves the map in use", "[minimap_zoom]") {
    bool wasIndoors = false;
    int mapLevel = 3;
    Frame f;
    f.level = 3;
    f.level = 5;  // Minimap:SetZoom(5)
    CHECK_FALSE(mz::syncFrame(f.zoom(), wasIndoors, false, mapLevel));
    CHECK(mapLevel == 5);
}

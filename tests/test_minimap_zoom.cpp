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

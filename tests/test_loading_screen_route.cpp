// The crossing a ship's loading screen draws (0x00409880, 0x0040b0b0,
// 0x005440e0, 0x00408e10).
#include <catch_amalgamated.hpp>

#include "rendering/loading_screen_route.hpp"

namespace lr = wowee::rendering::loading_screen_route;

namespace {

// Path 241 in the shape 3.3.5a ships it: a stop on Kalimdor, then the nodes
// on the Eastern Kingdoms with a stop there; path 242 after it.
std::vector<lr::PathNode> ship() {
    std::vector<lr::PathNode> n;
    for (uint32_t i = 0; i < 6; ++i) n.push_back({100 + i, 241, 1, -3800.0f, -4500.0f, i == 2 ? 2u : 0u});
    for (uint32_t i = 6; i < 12; ++i) n.push_back({100 + i, 241, 0, -3700.0f, -600.0f, i == 9 ? 2u : 0u});
    n.push_back({200, 242, 0, 0.0f, 0.0f, 2u});
    return n;
}

std::optional<lr::Continent> continents(uint32_t map) {
    if (map == 0) return lr::Continent{16.0f, 4.0f, 0.75f};
    if (map == 1) return lr::Continent{-14.0f, 4.0f, 0.7f};
    return std::nullopt;
}

}  // namespace

TEST_CASE("a path's legs run between stops, closed at a map change", "[loading_screen_route]") {
    const auto legs = lr::legsOf(ship(), 241);
    REQUIRE(legs.size() == 2);
    CHECK(legs[0].firstStop == 102);
    CHECK(legs[0].lastStop == 102);
    CHECK(legs[1].firstStop == 109);
    CHECK(legs[1].lastStop == 109);

    // A teleport node closes a leg too, and takes no stop of its own.
    std::vector<lr::PathNode> tp{{1, 5, 0, 0, 0, 2u}, {2, 5, 0, 0, 0, 2u}, {3, 5, 0, 0, 0, 3u},
                                 {4, 5, 0, 0, 0, 2u}, {9, 6, 0, 0, 0, 0u}};
    const auto t = lr::legsOf(tp, 5);
    REQUIRE(t.size() == 2);
    CHECK(t[0].firstStop == 1);
    CHECK(t[0].lastStop == 2);
    CHECK(t[1].firstStop == 4);
}

TEST_CASE("a continent's place on the Azeroth map (0x005440e0)", "[loading_screen_route]") {
    const lr::Point p = lr::mapPoint({16.0f, 4.0f, 0.75f}, 0.0f, 0.0f);
    CHECK(p.x == Catch::Approx(16.0f * 16.0f / 1002.0f + 0.5f));
    CHECK(p.y == Catch::Approx(1.0f - (4.0f * 16.0f / 668.0f + 0.5f)));
    // East (wire Y up) is left on the map; north (wire X up) is up.
    CHECK(lr::mapPoint({0, 0, 1}, 0.0f, 1000.0f).x < 0.5f);
    CHECK(lr::mapPoint({0, 0, 1}, 1000.0f, 0.0f).y > 0.5f);
}

TEST_CASE("the crossing runs dock to dock through the row's points", "[loading_screen_route]") {
    lr::SplineRow row;
    row.pathId = 241;
    row.legIndex = 0;
    row.x = {0.5f, 0.7f};
    row.y = {0.4f, 0.2f};
    const auto route = lr::build(ship(), {row}, 241, 1, continents);
    REQUIRE(route);
    const lr::Point start = lr::mapPoint(*continents(1), -3800.0f, -4500.0f);
    const lr::Point end = lr::mapPoint(*continents(0), -3700.0f, -600.0f);
    CHECK(route->start.x == Catch::Approx(start.x));
    CHECK(route->end.y == Catch::Approx(end.y));
    REQUIRE(route->dots.size() >= 2);
    CHECK(route->dots.front().x == Catch::Approx(start.x));
    CHECK(route->dots.back().x == Catch::Approx(end.x));
    CHECK(route->dots.back().y == Catch::Approx(end.y));

    // Leaving the other way takes leg 1, which has no row here.
    CHECK_FALSE(lr::build(ship(), {row}, 241, 0, continents));
    // A map no leg starts on draws nothing.
    CHECK_FALSE(lr::build(ship(), {row}, 241, 530, continents));
}

TEST_CASE("the trail fills in with the progress (0x00408e10)", "[loading_screen_route]") {
    lr::Route r;
    r.dots.resize(5);   // n = 4
    CHECK(r.shownDots(0.0f) == 1);
    CHECK(r.shownDots(0.49f) == 2);
    CHECK(r.shownDots(0.5f) == 3);
    CHECK(r.shownDots(1.0f) == 5);
}

TEST_CASE("the twelve tiles cover the 1002x668 frame (0x00407c50)", "[loading_screen_route]") {
    const lr::Tile first = lr::tile(0, 0);
    CHECK(first.x0 == 0.0f);
    CHECK(first.yTop == 1.0f);
    const lr::Tile last = lr::tile(3, 2);
    CHECK(last.x1 == Catch::Approx(1.0f));
    CHECK(last.yBottom == Catch::Approx(0.0f));
    CHECK(last.uMax == 0.9140625f);
    CHECK(last.vMax == 0.609375f);
}

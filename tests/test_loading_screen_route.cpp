// The loading screen of a transport's crossing (loading_screen_route.hpp).

#include <catch_amalgamated.hpp>

#include "rendering/loading_screen_route.hpp"

using namespace wowee::rendering::loading_route;

// WorldMapContinent rows 1 and 2 (Eastern Kingdoms, Kalimdor) and the
// Thundercaller's two stops (TaxiPath 302): Orgrimmar's tower and Undercity's.
TEST_CASE("a stop is put on the world map by its continent", "[loading_route]") {
    const Continent kalimdor{-20.1875f, 3.3125f, 0.7f};
    const Continent kingdoms{16.875f, -1.5f, 0.7f};
    const glm::vec2 org = worldMapPoint(kalimdor, 1318.1068f, -4658.0469f);
    const glm::vec2 uc = worldMapPoint(kingdoms, 2062.376f, 292.998f);
    // Kalimdor is the map's left half, the Eastern Kingdoms its right.
    CHECK(org.x == Catch::Approx(0.2753f).margin(1e-3));
    CHECK(org.y == Catch::Approx(0.4621f).margin(1e-3));
    CHECK(uc.x == Catch::Approx(0.7633f).margin(1e-3));
    CHECK(uc.y == Catch::Approx(0.6008f).margin(1e-3));
}

TEST_CASE("the spline runs from the start through the leg's points to the end", "[loading_route]") {
    const glm::vec2 a(0.2f, 0.3f), b(0.8f, 0.6f);
    // A zero point ends the list.
    const auto cps = controlPoints(a, {{0.4f, 0.5f}, {0.6f, 0.55f}, {0.0f, 0.0f}, {0.9f, 0.9f}}, b);
    REQUIRE(cps.size() == 6);
    CHECK(evaluate(cps, 0.0f).x == Catch::Approx(a.x));
    CHECK(evaluate(cps, 1.0f).y == Catch::Approx(b.y));
    CHECK(evaluate(cps, 1.0f / 3.0f).x == Catch::Approx(0.4f));
    // Dotted 66.67 to the map's width.
    const float len = length(cps);
    CHECK(len > glm::length(b - a));
    CHECK(dotSegments(len) == static_cast<int>(std::lround(len * 66.66667f)));
    CHECK(dotSegments(0.0f) == 1);
}

TEST_CASE("the dots come in with the load", "[loading_route]") {
    CHECK(dotsShown(11, 0.0f) == 1);
    CHECK(dotsShown(11, 0.5f) == 6);
    CHECK(dotsShown(11, 1.0f) == 11);
}

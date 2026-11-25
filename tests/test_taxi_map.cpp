// The flight map (0x005932e0): the known nodes shown on the current node's
// shown map, both through WorldMapTransforms, placed on the picture's extent
// out of WorldMapContinent. Rows below are the 12340 client's.
#include <catch_amalgamated.hpp>

#include "game/taxi_map.hpp"

#include <algorithm>
#include <map>
#include <set>

using namespace wowee::game;
using Catch::Approx;

namespace {

struct Node {
    uint32_t mapId;
    float x, y;
};

// TaxiNodes.dbc.
const std::map<uint32_t, Node> kNodes = {
    {2, {0, -8840.6f, 489.7f}},        // Stormwind
    {67, {0, 2271.1f, -5340.8f}},      // Light's Hope Chapel (Alliance)
    {82, {530, 9375.2f, -7165.9f}},    // Silvermoon
    {83, {530, 7594.5f, -6784.3f}},    // Tranquillien
    {94, {530, -4054.9f, -11793.3f}},  // Blood Watch
    {99, {530, 228.5f, 2633.6f}},      // Thrallmar
    {128, {530, -1837.2f, 5301.9f}},   // Shattrath
    {205, {530, 6789.8f, -7747.6f}},   // Zul'Aman
};

// WorldMapTransforms.dbc rows 2 and 3.
const std::vector<WorldMapTransform> kTransforms = {
    {530, 4800.0f, -10133.33f, 16000.0f, -2666.67f, 0, -2400.0f, 2400.0f},
    {530, -6933.33f, -16000.0f, 533.33f, -8000.0f, 1, 10133.33f, 17600.0f},
};

// WorldMapContinent.dbc.
TaxiMapBounds boundsOf(uint32_t mapId) {
    switch (mapId) {
        case 0: return {-16530.0f, -16530.0f, 12270.0f, 12270.0f};
        case 1: return {-11870.0f, -13370.0f, 12470.0f, 10970.0f};
        case 530: return {-5867.0f, -1600.0f, 6400.0f, 10670.0f};
        default: return {};
    }
}

TaxiMap mapFor(uint32_t current, const std::set<uint32_t>& unknown = {}, uint32_t expansion = 2) {
    return buildTaxiMap(
        kNodes, current, kTransforms, boundsOf, [&](uint32_t id) { return !unknown.count(id); },
        [&](uint32_t mapId) { return (mapId == 530 ? 1u : 0u) <= expansion; });
}

std::vector<uint32_t> idsOf(const TaxiMap& map) {
    std::vector<uint32_t> out;
    for (const auto& n : map.nodes) out.push_back(n.id);
    return out;
}

}  // namespace

TEST_CASE("Outland's flight map has no node of the blood elves' or draenei's zones", "[taxi][map]") {
    const TaxiMap map = mapFor(128);
    CHECK(map.mapId == 530);
    CHECK(idsOf(map) == std::vector<uint32_t>{99, 128});
    for (const auto& n : map.nodes) {
        CHECK(n.u > 0.0f);
        CHECK(n.u < 1.0f);
        CHECK(n.v > 0.0f);
        CHECK(n.v < 1.0f);
    }
}

TEST_CASE("those nodes are on the map they are shown on", "[taxi][map]") {
    const TaxiMap east = mapFor(82);
    CHECK(east.mapId == 0);
    CHECK(idsOf(east) == std::vector<uint32_t>{2, 67, 82, 83, 205});
    // From the Eastern Kingdoms too.
    CHECK(idsOf(mapFor(2)) == idsOf(east));

    const TaxiMap kalimdor = mapFor(94);
    CHECK(kalimdor.mapId == 1);
    CHECK(idsOf(kalimdor) == std::vector<uint32_t>{94});

    // Silvermoon at its shown place: (9375.2 - 2400, -7165.9 + 2400).
    const auto it = std::find_if(east.nodes.begin(), east.nodes.end(),
                                 [](const TaxiMapNode& n) { return n.id == 82; });
    REQUIRE(it != east.nodes.end());
    CHECK(it->u == Approx((12270.0f + 4765.9f) / 28800.0f));
    CHECK(it->v == Approx((6975.2f + 16530.0f) / 28800.0f));
}

TEST_CASE("a node is placed on the picture's own extent, from the lower left", "[taxi][map]") {
    const TaxiMap map = mapFor(2);
    REQUIRE(map.nodes.front().id == 2);
    CHECK(map.nodes.front().u == Approx((12270.0f - 489.7f) / 28800.0f));
    CHECK(map.nodes.front().v == Approx((-8840.6f + 16530.0f) / 28800.0f));
    // A leg's end is there as well.
    const auto [u, v] = taxiLegPos(map, kTransforms, 530, 9375.2f, -7165.9f);
    CHECK(u == Approx((12270.0f + 4765.9f) / 28800.0f));
    CHECK(v == Approx((6975.2f + 16530.0f) / 28800.0f));
}

TEST_CASE("only known nodes are listed, and those of an expansion owned", "[taxi][map]") {
    CHECK(idsOf(mapFor(82, {67, 205})) == std::vector<uint32_t>{2, 82, 83});
    CHECK(idsOf(mapFor(2, {}, 0)) == std::vector<uint32_t>{2, 67});
    // A map without a picture lists nothing.
    const auto none = [](uint32_t) { return TaxiMapBounds{}; };
    const auto yes = [](uint32_t) { return true; };
    CHECK(buildTaxiMap(kNodes, 2, kTransforms, none, yes, yes).nodes.empty());
    CHECK(mapFor(9999).nodes.empty());
}

TEST_CASE("both ends of a transform's region belong to it", "[taxi][map]") {
    CHECK(shownPlace(kTransforms, 530, 4800.0f, -10133.33f).mapId == 0);
    CHECK(shownPlace(kTransforms, 530, 16000.0f, -2666.67f).mapId == 0);
    CHECK(shownPlace(kTransforms, 530, 4799.0f, -3000.0f).mapId == 530);
    CHECK(shownPlace(kTransforms, 571, 9375.2f, -7165.9f).mapId == 571);
}

TEST_CASE("a flight takes the shortest way over the map's nodes", "[taxi][route]") {
    TaxiMap map;
    for (uint32_t id : {1u, 2u, 3u, 4u, 5u}) map.nodes.push_back({id, 0, 0});
    const auto open = [](uint32_t) { return false; };
    // 1 to 5 over 2 is two stops and long, over 3 and 4 three and short; 9 is
    // a node the map does not list.
    const std::vector<TaxiLeg> legs = {
        {1, 2, 10, 500.0f}, {2, 5, 10, 500.0f}, {1, 3, 20, 100.0f}, {3, 4, 30, 100.0f},
        {4, 5, 40, 100.0f}, {1, 9, 1, 1.0f},    {9, 5, 1, 1.0f},    {1, 4, 99, 900.0f},
    };
    TaxiRoutes routes;
    routes.build(map, legs, 1, open);
    CHECK(routes.to(5) == std::vector<uint32_t>{1, 3, 4, 5});
    CHECK(routes.cost(5) == 90);
    // Friendly to exalted with the flight master's faction (0x00591350): each
    // stop's fare by itself.
    CHECK(routes.cost(5, 0) == 90);
    CHECK(routes.cost(5, 5) == 18 + 27 + 36);
    CHECK(routes.cost(5, 7) == 16 + 24 + 32);
    CHECK(routes.cost(4, 6) == 84);
    CHECK(taxiFare(43, 4) == 41);
    // A path of its own is flown however long it is - but not on the way on.
    CHECK(routes.to(4) == std::vector<uint32_t>{1, 4});
    CHECK(routes.cost(4) == 99);
    CHECK_FALSE(routes.reaches(9));
    CHECK_FALSE(routes.reaches(1));
    CHECK(routes.to(1).empty());

    // A node the player's side does not fly to is no stop either.
    routes.build(map, legs, 1, [](uint32_t id) { return id == 3; });
    CHECK(routes.to(5) == std::vector<uint32_t>{1, 2, 5});
    CHECK_FALSE(routes.reaches(3));
}

TEST_CASE("a node of the other side's beasts only is refused", "[taxi][route]") {
    CHECK(taxiNodeRefused(true, 2224, 0));       // wyvern
    CHECK_FALSE(taxiNodeRefused(false, 2224, 0));
    CHECK(taxiNodeRefused(false, 0, 541));       // gryphon
    CHECK_FALSE(taxiNodeRefused(true, 0, 541));
    CHECK_FALSE(taxiNodeRefused(true, 2224, 541));
    CHECK_FALSE(taxiNodeRefused(false, 2224, 541));
    CHECK_FALSE(taxiNodeRefused(true, 18345, 18345));
}

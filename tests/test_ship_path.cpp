// ShipPath: a ship's route timed and posed as the client does (0x007f90f0,
// 0x007f8660, 0x007f82b0).
#include <catch2/catch_amalgamated.hpp>
#include "game/ship_path.hpp"
#include <cmath>
#include <vector>

using wowee::game::ShipPath;
using wowee::game::ShipPathNode;

namespace {

// Nine nodes along x, 100 apart, on one map; node 4 a stop of 10 s.
std::vector<ShipPathNode> straightRoute(uint32_t map = 0) {
    std::vector<ShipPathNode> nodes;
    for (int i = 0; i < 9; ++i) {
        ShipPathNode n;
        n.server = glm::vec3(100.0f * i, 0.0f, 50.0f);
        n.mapId = map;
        if (i == 4) {
            n.flags = 2;
            n.delaySeconds = 10;
        }
        nodes.push_back(n);
    }
    return nodes;
}

}  // namespace

TEST_CASE("ShipPath times a leg: slowing into the stop, waiting, speeding away", "[ship_path]") {
    const ShipPath path = ShipPath::build(straightRoute(), 30.0f, 1.0f);
    REQUIRE(path.valid());
    // The first and last nodes only steer: 300 yd in, 300 yd out, each
    // sqrt(2 * 300 / 1) s at 1 yd/s2 (the ramp to 30 yd/s needs 450 yd).
    const uint32_t run = static_cast<uint32_t>(std::lround(std::sqrt(600.0f) * 1000.0f));
    REQUIRE(path.periodMs() == run + 10000u + run);

    const auto start = path.evaluate(0);
    REQUIRE(start.server.x == Catch::Approx(100.0f).margin(0.01f));

    // At the stop, exactly at the node, for the whole wait.
    for (uint32_t t : {run, run + 5000u, run + 9999u}) {
        const auto at = path.evaluate(t);
        REQUIRE(at.motion == ShipPath::Motion::Stopped);
        REQUIRE(at.server.x == Catch::Approx(400.0f).margin(0.01f));
    }
    // A tenth of a second before it, all but there and slowing - no overshoot.
    const auto before = path.evaluate(run - 100u);
    REQUIRE(before.motion == ShipPath::Motion::Decelerating);
    REQUIRE(before.server.x < 400.0f);
    REQUIRE(before.server.x > 399.9f);
    // And easing away after it.
    const auto after = path.evaluate(run + 10100u);
    REQUIRE(after.motion == ShipPath::Motion::Accelerating);
    REQUIRE(after.server.x > 400.0f);
    REQUIRE(after.server.x < 400.1f);
    // Its heading through the stop is the route's line.
    REQUIRE(before.direction.x > 0.0f);
    REQUIRE(path.evaluate(run + 5000u).direction.x > 0.0f);
    REQUIRE(std::abs(path.evaluate(run + 5000u).direction.y) < 1e-4f);
}

TEST_CASE("ShipPath never runs back along a leg", "[ship_path]") {
    const ShipPath path = ShipPath::build(straightRoute(), 30.0f, 1.0f);
    float last = -1.0f;
    for (uint32_t t = 0; t < path.periodMs(); t += 50) {
        const float x = path.evaluate(t).server.x;
        REQUIRE(x >= last - 0.01f);   // the ms rounding of a run: under a centimetre
        last = x;
    }
}

TEST_CASE("ShipPath cuts legs at a change of map and after a teleport node", "[ship_path]") {
    std::vector<ShipPathNode> nodes = straightRoute(1);
    for (ShipPathNode n : straightRoute(0)) {
        n.server.y = 5000.0f;
        nodes.push_back(n);
    }
    const ShipPath path = ShipPath::build(nodes, 30.0f, 1.0f);
    REQUIRE(path.valid());
    const uint32_t half = path.periodMs() / 2;
    REQUIRE(path.onMapAt(10, 1));
    REQUIRE_FALSE(path.onMapAt(10, 0));
    REQUIRE(path.onMapAt(half + 10, 0));
    REQUIRE(path.evaluate(half + 10).server.y == Catch::Approx(5000.0f));

    // A teleport flag on node 4 of one map cuts it there; node 4 is then no
    // longer a stop of the leg it would end (a leg's last node only steers).
    std::vector<ShipPathNode> tele = straightRoute(0);
    tele[4].flags = 1;
    const ShipPath cut = ShipPath::build(tele, 30.0f, 1.0f);
    REQUIRE(cut.valid());
    REQUIRE(cut.evaluate(0).motion == ShipPath::Motion::Moving);
}

TEST_CASE("ShipPath without a speed or an acceleration is empty", "[ship_path]") {
    REQUIRE_FALSE(ShipPath::build(straightRoute(), 0.0f, 1.0f).valid());
    REQUIRE_FALSE(ShipPath::build(straightRoute(), 30.0f, 0.0f).valid());
}

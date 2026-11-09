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

TEST_CASE("ShipPath measures a segment by twenty chords and keeps the spline's pace", "[ship_path]") {
    // Uneven nodes along x: the segments from 100 to 200, 200 to 300, 300
    // to 600 and 600 to 900 (the first and last nodes only steer).
    std::vector<ShipPathNode> nodes;
    for (float x : {0.0f, 100.0f, 200.0f, 300.0f, 600.0f, 900.0f, 1000.0f}) {
        ShipPathNode n;
        n.server = glm::vec3(x, 0.0f, 0.0f);
        nodes.push_back(n);
    }
    const ShipPath path = ShipPath::build(nodes, 30.0f, 1.0f);
    REQUIRE(path.valid());
    // 0x004c3b10's twenty chords of each segment, at a steady 30 yd/s with no
    // stop: a straight route's lengths are its chords, 800 yd in all.
    REQUIRE(path.periodMs() == static_cast<uint32_t>(std::lround(800.0f / 30.0f * 1000.0f)));
    // 0x004c3bd0: the parameter in proportion to the distance into a
    // segment. Through the node at 300, from a segment a third as long as
    // the next, the hull's pace through the world drops at once.
    const uint32_t atNode = static_cast<uint32_t>(std::lround(200.0f / 30.0f * 1000.0f));
    auto pace = [&](uint32_t t0) {
        return (path.evaluate(t0 + 20u).server.x - path.evaluate(t0).server.x) / 0.02f;
    };
    const float before = pace(atNode - 40u);
    const float after = pace(atNode + 20u);
    REQUIRE(before > 2.0f * after);
    // Facing along the route all the way.
    for (uint32_t t = 0; t < path.periodMs(); t += 500) REQUIRE(path.evaluate(t).direction.x > 0.0f);
}

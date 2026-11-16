// A siege missile meeting a unit on its way (0x006fcb60, 0x006fbff0,
// 0x006fc4a0, 0x006fc360).
#include <catch_amalgamated.hpp>

#include <cmath>

#include "rendering/missile_collision.hpp"

namespace mc = wowee::rendering::missile_collision;
using Catch::Approx;

TEST_CASE("the path's cosine is the smooth curve of the pitch over pi", "[missile_collision]") {
    CHECK(mc::smoothCos(0.0f) == Approx(1.0f));  // a pitch not above 0 takes one less whole
    CHECK(mc::smoothCos(0.5f * 3.14159265f) == Approx(0.0f).margin(1e-5));
    CHECK(mc::smoothCos(0.25f * 3.14159265f) == Approx(0.6875f).margin(1e-5));  // the curve, not the cosine
    CHECK(mc::smoothCos(3.14159265f * 0.999f) == Approx(-1.0f).margin(1e-4));
    CHECK(mc::smoothCos(-0.25f * 3.14159265f) == Approx(0.6875f).margin(1e-5));
    CHECK(mc::smoothCos(1.25f * 3.14159265f) == Approx(-0.6875f).margin(1e-5));
}

TEST_CASE("the path runs from the launch to the landing at the speed across the ground", "[missile_collision]") {
    const auto p = mc::makePath({0, 0, 5}, {30, 40, 0}, 0.0f, 20.0f, 1.5f);
    CHECK(p.length == Approx(50.0f));
    CHECK(p.dir.x == Approx(0.6f));
    CHECK(p.dir.y == Approx(0.8f));
    CHECK(p.side.x == Approx(-0.8f));
    CHECK(p.side.y == Approx(0.6f));
    CHECK(p.speed == Approx(20.0f));
    CHECK(p.time == Approx(2.5f));
    CHECK(p.rowRadius == 1.5f);
}

TEST_CASE("a shot straight up is given a yard's run east over its whole flight", "[missile_collision]") {
    const auto p = mc::makePath({10, 10, 0}, {10, 10, 20}, 1.5f, 10.0f, 0.0f);
    CHECK(p.start.x == Approx(9.99f));
    CHECK(p.length == 1.0f);
    CHECK(p.dir.x == 1.0f);
    CHECK(p.speed == Approx(0.005f));  // 0.01 over the 2 s it flies
    CHECK(p.time == Approx(200.0f));
}

TEST_CASE("a unit's sphere is its model's missile radius, pushed back and raised", "[missile_collision]") {
    mc::UnitShape u;
    u.position = {10, 0, 0};
    u.scale = 2.0f;
    u.hasModelData = true;
    u.collisionHeight = 3.0f;
    u.missileRadius = 1.0f;
    u.missilePush = 0.5f;
    u.missileRaise = 0.25f;
    const auto s = mc::unitSphere(u, {1, 0}, 0.5f);
    CHECK(s.radius == Approx(2.5f));
    CHECK(s.centre.x == Approx(9.0f));
    CHECK(s.centre.z == Approx(0.5f + 2.0f));

    u.missileRadius = 0.0f;  // half the collision height, not scaled
    CHECK(mc::unitSphere(u, {1, 0}, 0.0f).radius == Approx(1.5f));

    mc::UnitShape bare;
    bare.position = {0, 0, 7};
    const auto b = mc::unitSphere(bare, {1, 0}, 0.25f);
    CHECK(b.radius == Approx(1.25f));
    CHECK(b.centre.z == Approx(8.0f));
}

TEST_CASE("the row's flags pick which units a missile meets", "[missile_collision]") {
    mc::UnitKind hostilePlayer{true, false, true, true};
    mc::UnitKind friendlyPlayer{true, false, false, true};
    CHECK(mc::rowMeets(0x04, hostilePlayer));
    CHECK_FALSE(mc::rowMeets(0x04, friendlyPlayer));
    CHECK(mc::rowMeets(0x08, friendlyPlayer));
    CHECK(mc::rowMeets(0x0c, friendlyPlayer));
    CHECK_FALSE(mc::rowMeets(0x30, hostilePlayer));

    mc::UnitKind creature{false, true, true, true};
    CHECK(mc::rowMeets(0x10, creature));
    CHECK_FALSE(mc::rowMeets(0x20, creature));
    CHECK(mc::rowMeets(0x30, creature));
    creature.collidesWithMissiles = false;  // type flag 0x80000 needed
    CHECK_FALSE(mc::rowMeets(0x30, creature));

    mc::UnitKind noMover{true, false, true, false};
    CHECK_FALSE(mc::rowMeets(0x04, noMover));
    CHECK(mc::rowMeets(0x0c, noMover));
}

TEST_CASE("a unit beside the path is measured, one far off is not", "[missile_collision]") {
    const auto p = mc::makePath({0, 0, 0}, {100, 0, 0}, 0.0f, 50.0f, 0.0f);  // 2 s
    mc::Sphere near{{50, 3, 0}, 2.0f};
    CHECK(mc::unitScore(p, near, 7.0f) > 0.0f);
    mc::Sphere far{{50, 200, 0}, 2.0f};
    CHECK(mc::unitScore(p, far, 7.0f) == -1.0f);
    mc::Sphere none{{50, 0, 0}, 0.0f};
    CHECK(mc::unitScore(p, none, 7.0f) == -1.0f);
}

TEST_CASE("past fifteen units the greatest measures are kept", "[missile_collision]") {
    std::vector<mc::Scored> found;
    for (uint64_t i = 1; i <= 20; ++i) found.push_back({i, static_cast<float>(i)});
    const auto kept = mc::keepUnits(found);
    REQUIRE(kept.size() == 15u);
    CHECK(kept.front() == 20u);
    CHECK(kept.back() == 6u);
    found.resize(3);
    CHECK(mc::keepUnits(found) == std::vector<uint64_t>{1, 2, 3});
}

TEST_CASE("a step meets the first sphere it passes within, at its closest", "[missile_collision]") {
    std::vector<mc::Sphere> spheres{{{5, 0.5f, 0}, 1.0f}, {{2, 0, 0}, 1.0f}};
    CHECK(mc::stepHit({0, 0, 0}, {10, 0, 0}, spheres) == Approx(0.5f));
    CHECK(mc::stepHit({0, 0, 0}, {1, 0, 0}, spheres) == -1.0f);  // short of both
    CHECK(mc::stepHit({0, 0, 0}, {0, 0, 0}, spheres) == -1.0f);
    // A sphere with no radius ends the list.
    std::vector<mc::Sphere> ended{{{5, 0, 0}, 0.0f}, {{2, 0, 0}, 1.0f}};
    CHECK(mc::stepHit({0, 0, 0}, {10, 0, 0}, ended) == -1.0f);
}

// A particle's step (0x00979bb0) and the spline emitter's curve (0x00981d40
// and the Bezier spline at vtable 0x00aa2e20).
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_motion.hpp"

using namespace wowee::rendering::m2_particle;
using Catch::Approx;

TEST_CASE("Particle step: gravity's half-square term, then gravity on the velocity") {
    glm::vec3 pos(0.0f), vel(1.0f, 0.0f, 0.0f);
    StepParams p;
    p.gravity = 2.0f;
    step(pos, vel, 0.0f, 0.5f, p);
    REQUIRE(pos.x == Approx(0.5f));
    REQUIRE(pos.z == Approx(-0.25f));
    REQUIRE(vel.z == Approx(-1.0f));
}

TEST_CASE("Particle step: drag takes dt * drag of the velocity, after the move") {
    glm::vec3 pos(0.0f), vel(4.0f, 0.0f, 0.0f);
    StepParams p;
    p.drag = 0.5f;
    step(pos, vel, 0.0f, 0.5f, p);
    REQUIRE(pos.x == Approx(2.0f));
    REQUIRE(vel.x == Approx(3.0f));
    // Never more than all of it.
    p.drag = 10.0f;
    step(pos, vel, 0.0f, 0.5f, p);
    REQUIRE(vel.x == Approx(0.0f));
}

TEST_CASE("Particle step: the wind blows while the particle is younger than the wind time") {
    StepParams p;
    p.wind = glm::vec3(2.0f, 0.0f, 0.0f);
    p.windTime = 1.0f;
    glm::vec3 pos(0.0f), vel(0.0f);
    step(pos, vel, 0.5f, 0.25f, p);
    REQUIRE(vel.x == Approx(0.5f));
    REQUIRE(pos.x == Approx(0.125f));
    glm::vec3 pos2(0.0f), vel2(0.0f);
    step(pos2, vel2, 1.0f, 0.25f, p);
    REQUIRE(vel2.x == Approx(0.0f));
}

TEST_CASE("zSource: under a thousandth is unset; set, it aims away from the point above") {
    REQUIRE(zSourceValue(0.0005f) == 0.0f);
    REQUIRE(zSourceValue(-2.0f) == -2.0f);
    const glm::vec3 d = zSourceDirection(glm::vec3(3.0f, 0.0f, 0.0f), 4.0f);
    REQUIRE(d.x == Approx(0.6f));
    REQUIRE(d.z == Approx(-0.8f));
}

TEST_CASE("Spline: one segment is a cubic Bezier from end to end") {
    BezierSpline s;
    s.setPoints({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}});
    REQUIRE(s.segmentCount() == 1);
    REQUIRE(s.position(0.0f).x == Approx(0.0f));
    REQUIRE(s.position(1.0f).x == Approx(3.0f));
    REQUIRE(s.position(0.5f).x == Approx(1.5f));
    REQUIRE(s.totalLength() == Approx(3.0f));
    REQUIRE(s.tangent(0.5f).x == Approx(3.0f));
}

TEST_CASE("Spline: two segments are walked by arc length") {
    BezierSpline s;
    // A straight segment of 1 then one of 3.
    s.setPoints({{0, 0, 0}, {1.0f / 3, 0, 0}, {2.0f / 3, 0, 0}, {1, 0, 0},
                 {2, 0, 0}, {3, 0, 0}, {4, 0, 0}});
    REQUIRE(s.segmentCount() == 2);
    REQUIRE(s.totalLength() == Approx(4.0f));
    // A quarter of the way is the end of the first segment...
    REQUIRE(s.position(0.25f).x == Approx(1.0f).margin(1e-4));
    // ...and half way is half way along the second.
    REQUIRE(s.position(0.625f).x == Approx(2.5f).margin(1e-4));
}

TEST_CASE("Spline: n points make n / 3 segments; fewer than three make none") {
    BezierSpline s;
    s.setPoints({{0, 0, 0}, {1, 0, 0}});
    REQUIRE(s.empty());
    s.setPoints({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {9, 9, 9}});
    REQUIRE(s.segmentCount() == 1);
    REQUIRE(s.position(1.0f).x == Approx(3.0f));
}

TEST_CASE("Spline direction: up, turned about the tangent") {
    const glm::vec3 up = splineDirection(glm::vec3(1, 0, 0), 0.0f);
    REQUIRE(up.z == Approx(1.0f));
    const glm::vec3 side = splineDirection(glm::vec3(2, 0, 0), 1.5707963f);
    REQUIRE(std::fabs(side.y) == Approx(1.0f));
    REQUIRE(side.z == Approx(0.0f).margin(1e-5));
}

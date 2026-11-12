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

TEST_CASE("Emission distance: all of it to 50 yards, then down to a quarter") {
    REQUIRE(emissionDistanceFactor(0.0f) == Approx(1.0f));
    REQUIRE(emissionDistanceFactor(50.0f) == Approx(1.0f));
    REQUIRE(emissionDistanceFactor(75.0f) == Approx(0.5f));
    REQUIRE(emissionDistanceFactor(87.5f) == Approx(0.25f));
    REQUIRE(emissionDistanceFactor(500.0f) == Approx(0.25f));
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

TEST_CASE("Particle turn: the client's sine and cosine at the quarter turns") {
    const SinCos z = clientSinCos(0.0f);
    REQUIRE(z.s == Approx(0.0f).margin(1e-6));
    REQUIRE(z.c == Approx(1.0f));
    const SinCos q = clientSinCos(1.5707963f);
    REQUIRE(q.s == Approx(1.0f).margin(1e-5));
    REQUIRE(q.c == Approx(0.0f).margin(1e-5));
    const SinCos h = clientSinCos(3.1415927f);
    REQUIRE(h.c == Approx(-1.0f).margin(1e-5));
    const SinCos m = clientSinCos(-1.5707963f);
    REQUIRE(m.s == Approx(-1.0f).margin(1e-5));
    // Between, the cubic: at a sixth of a turn 1 - (6 - 4/3) / 9.
    REQUIRE(clientSinCos(1.0471976f).c == Approx(1.0f - (6.0f - 4.0f / 3.0f) / 9.0f).margin(1e-5));
}

TEST_CASE("Particle turn: none without a rate or its spread, else start plus age times rate") {
    bool turns = true;
    REQUIRE(particleSpinAngle(0.0f, 0.0f, 2.0f, 1.0f, 3.0f, false, turns) == 0.0f);
    REQUIRE_FALSE(turns);
    REQUIRE(particleSpinAngle(5.0f, 0.0f, 2.0f, 1.0f, 5.0f, false, turns) == Approx(11.0f));
    REQUIRE(turns);
    REQUIRE(particleSpinAngle(0.0f, 0.5f, 2.0f, 1.0f, 0.25f, true, turns) == Approx(-1.5f));
}

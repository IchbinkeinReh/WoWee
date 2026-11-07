// The client's fall (collision_fall.hpp, 0x007618b0 -> 0x007612b0): its
// curve and the time read back off it, landing, a jump's arc, a ceiling,
// and a fall slid down past a wall.

#include <catch_amalgamated.hpp>

#include "rendering/collision_fall.hpp"

#include <vector>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {

void addQuad(std::vector<Facet>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::vec3& d) {
    out.push_back(makeFacet(windingNormal(a, b, c), a, b, c));
    out.push_back(makeFacet(windingNormal(a, c, d), a, c, d));
}

void addFloor(std::vector<Facet>& f, float z) {
    addQuad(f, {-50, -50, z}, {50, -50, z}, {50, 50, z}, {-50, 50, z});
}

Sweeper sweeperOver(const std::vector<Facet>& world) {
    return Sweeper([&world](const glm::vec3&, const glm::vec3&, std::vector<Facet>& out) {
        out.insert(out.end(), world.begin(), world.end());
    });
}

// Steps of 20 ms until it lands or `limit` steps have gone.
int fallUntilLanded(Mover& m, float hspeed, const glm::vec2& dir, Sweeper& s, int limit = 400) {
    for (int i = 0; i < limit; ++i) {
        const FallResult r = fallStep(m, 20, hspeed * 0.02f, dir, s);
        if (r.landed || !m.falling) return i + 1;
    }
    return -1;
}

}  // namespace

TEST_CASE("a fall's curve: gravity, then the terminal speed", "[collision_fall]") {
    CHECK(fallDistance(1.0f, false, 0.0f) == Approx(9.645553f));
    CHECK(fallDistance(0.5f, false, 0.0f) == Approx(9.645553f * 0.25f));
    // Terminal after 60.148 / 19.291 = 3.118 s, then linear.
    const float tT = kTerminalSpeed / kGravity;
    const float dT = fallDistance(tT, false, 0.0f);
    CHECK(fallDistance(tT + 1.0f, false, 0.0f) == Approx(dT + kTerminalSpeed).epsilon(1e-4));
    // A slow fall tops out at 7 yards a second.
    CHECK(fallDistance(10.0f, true, 0.0f) - fallDistance(9.0f, true, 0.0f) == Approx(7.0f).epsilon(1e-4));
    // A jump goes up first: back where it started after 2 v / g.
    CHECK(fallDistance(-2.0f * kJumpSpeed / kGravity, false, kJumpSpeed) == Approx(0.0f).margin(1e-4f));
    CHECK(fallDistance(0.2f, false, kJumpSpeed) < 0.0f);
}

TEST_CASE("the time read back off the curve", "[collision_fall]") {
    Mover m;
    m.falling = true;
    for (float t : {0.1f, 0.7f, 2.0f, 5.0f}) {
        CHECK(fallTimeFor(m, fallDistance(t, false, 0.0f), false) == Approx(t).epsilon(1e-3));
    }
    m.fallV0 = kJumpSpeed;
    // Half a yard up: on the way up and on the way down.
    const float up = fallTimeFor(m, -0.5f, true);
    const float down = fallTimeFor(m, -0.5f, false);
    CHECK(up < timeToApex(m));
    CHECK(down > timeToApex(m));
    CHECK(fallDistance(up, false, kJumpSpeed) == Approx(-0.5f).epsilon(1e-3));
    CHECK(fallDistance(down, false, kJumpSpeed) == Approx(-0.5f).epsilon(1e-3));
}

TEST_CASE("a fall lands on the floor under it", "[collision_fall]") {
    std::vector<Facet> world;
    addFloor(world, 0.0f);
    auto s = sweeperOver(world);
    Mover m;
    m.pos = {0, 0, 10};
    startFallAt(m, 0.0f);
    const int steps = fallUntilLanded(m, 0.0f, {1, 0}, s);
    REQUIRE(steps > 0);
    CHECK_FALSE(m.falling);
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
    // sqrt(2 h / g) = 1.018 s
    CHECK(steps * 20 == Approx(1018).margin(25));
    // Landing clears falling far as well (0x00988490).
    CHECK_FALSE(m.fallingFar);
}

TEST_CASE("a jump comes down where the run carries it", "[collision_fall]") {
    std::vector<Facet> world;
    addFloor(world, 0.0f);
    auto s = sweeperOver(world);
    Mover m;
    startFallAt(m, kJumpSpeed);
    m.fallSpeed = 7.0f;
    m.fallDir = {1, 0};
    float top = 0.0f;
    int steps = -1;
    for (int i = 0; i < 200; ++i) {
        const FallResult r = fallStep(m, 20, 7.0f * 0.02f, {1, 0}, s);
        top = std::max(top, m.pos.z);
        if (r.landed) {
            steps = i + 1;
            break;
        }
    }
    REQUIRE(steps > 0);
    // v^2 / 2g = 1.64 yards up, 0.825 s in the air.
    CHECK(top == Approx(kJumpSpeed * kJumpSpeed / (2.0f * kGravity)).margin(0.05f));
    CHECK(steps * 20 == Approx(825).margin(25));
    CHECK(m.pos.x == Approx(7.0f * 0.825f).margin(0.2f));
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
}

TEST_CASE("a jump into a ceiling stops rising there", "[collision_fall]") {
    std::vector<Facet> world;
    addFloor(world, 0.0f);
    // A ceiling 2.5 yards up, facing down: the head (2.03) meets it first.
    addQuad(world, {-50, -50, 2.5f}, {-50, 50, 2.5f}, {50, 50, 2.5f}, {50, -50, 2.5f});
    REQUIRE(world.back().n.z < 0.0f);
    auto s = sweeperOver(world);
    Mover m;
    startFallAt(m, kJumpSpeed);
    m.fallSpeed = 7.0f;
    m.fallDir = {1, 0};
    bool hitCeiling = false;
    float top = 0.0f;
    for (int i = 0; i < 100 && m.falling; ++i) {
        const FallResult r = fallStep(m, 20, 7.0f * 0.02f, {1, 0}, s);
        hitCeiling |= r.ceiling;
        top = std::max(top, m.pos.z);
    }
    CHECK(hitCeiling);
    CHECK(top <= 2.5f - kPlayerHeight + 0.01f);
    CHECK_FALSE(m.falling);
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
}

TEST_CASE("a fall against a wall slides down it", "[collision_fall]") {
    std::vector<Facet> world;
    addFloor(world, 0.0f);
    // A wall across x = 1 facing -x.
    addQuad(world, {1, -50, -1}, {1, -50, 30}, {1, 50, 30}, {1, 50, -1});
    auto s = sweeperOver(world);
    Mover m;
    m.pos = {0, 0, 10};
    startFallAt(m, 0.0f);
    m.fallSpeed = 7.0f;
    m.fallDir = {1, 0};
    const int steps = fallUntilLanded(m, 7.0f, {1, 0}, s);
    REQUIRE(steps > 0);
    CHECK(m.pos.x <= 1.0f - kPlayerHalfWidth + 0.01f);
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
}

TEST_CASE("a walk off an edge falls to the ground below", "[collision_fall]") {
    std::vector<Facet> world;
    // A ledge to x = 0.5 at height 3, the ground at 0.
    addQuad(world, {-20, -20, 3}, {0.5f, -20, 3}, {0.5f, 20, 3}, {-20, 20, 3});
    addQuad(world, {0.5f, -20, 3}, {0.5f, -20, 0}, {0.5f, 20, 0}, {0.5f, 20, 3});
    addFloor(world, 0.0f);
    auto s = sweeperOver(world);
    Mover m;
    m.pos = {0, 0, 3};
    bool fell = false;
    for (int i = 0; i < 10 && !fell; ++i) fell = walk(m, {1, 0}, 0.14f, 20, s).fell;
    REQUIRE(fell);
    m.fallSpeed = 7.0f;
    m.fallDir = {1, 0};
    REQUIRE(fallUntilLanded(m, 7.0f, {1, 0}, s) > 0);
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
    CHECK(m.pos.x > 0.5f);
}

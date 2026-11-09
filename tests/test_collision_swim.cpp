// Swimming and flying (collision_swim.hpp, 0x00760b40 and 0x00730d10):
// when a unit swims, the surface holding a swimmer three quarters of its
// height under it, the jump out, and the plain sweep a flyer makes.

#include <catch_amalgamated.hpp>

#include "rendering/collision_fall.hpp"
#include "rendering/collision_swim.hpp"

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

}  // namespace

TEST_CASE("a unit swims past three quarters of its height and stops a 36th short of it",
          "[collision_swim]") {
    Mover m;
    const float start = kPlayerHeight * 0.75f;
    m.pos.z = 0.0f;
    CHECK_FALSE(updateSwimming(m, start - 0.01f, true, false));
    CHECK_FALSE(m.swimming);
    CHECK(updateSwimming(m, start + 0.01f, true, false));
    CHECK(m.swimming);
    // Between the two marks it goes on swimming.
    CHECK_FALSE(updateSwimming(m, start - 0.02f, true, false));
    CHECK(m.swimming);
    // Under the lower mark it stops, and falls from there.
    CHECK(updateSwimming(m, start - kSwimStopSlack - 0.001f, true, false));
    CHECK_FALSE(m.swimming);
    CHECK(m.falling);
}

TEST_CASE("out of the liquid altogether a swimmer stops, unless it flies", "[collision_swim]") {
    Mover m;
    m.swimming = true;
    m.flying = true;
    CHECK_FALSE(updateSwimming(m, std::nullopt, true, false));
    CHECK(m.swimming);
    m.flying = false;
    CHECK(updateSwimming(m, std::nullopt, true, true));
    CHECK_FALSE(m.swimming);
    // Gravity off: no fall.
    CHECK_FALSE(m.falling);
}

TEST_CASE("a jump on its way up does not start a swim", "[collision_swim]") {
    Mover m;
    startFallAt(m, kJumpSpeed);
    CHECK(risingInJump(m));
    CHECK_FALSE(updateSwimming(m, 3.0f, true, false));
    m.fallMs = 600;  // past the top (0.41 s)
    CHECK_FALSE(risingInJump(m));
    CHECK(updateSwimming(m, 3.0f, true, false));
}

TEST_CASE("the surface holds a swimmer three quarters of a body under it", "[collision_swim]") {
    std::vector<Facet> ground;
    addFloor(ground, -20.0f);
    std::vector<Facet> water;
    std::vector<Facet> up;
    addFloor(up, 0.0f);
    for (const auto& f : up) water.push_back(underside(f));
    REQUIRE(water[0].n.z < 0.0f);
    auto solid = sweeperOver(ground);
    auto liquid = sweeperOver(water);
    Mover m;
    m.swimming = true;
    m.pos = {0, 0, -5};
    // Straight up five yards, not rising on purpose (swimming up by pitch).
    const bool jumped = swimFlyStep(m, 500, 5.0f, {0, 0, 1}, solid, &liquid, false, false);
    CHECK_FALSE(jumped);
    CHECK(m.pos.z + kPlayerHeight * 0.75f == Approx(0.0f).margin(0.01f));
    // Swimming along under it, it keeps that depth.
    swimFlyStep(m, 500, 3.0f, glm::normalize(glm::vec3(1, 0, 0.2f)), solid, &liquid, false, false);
    CHECK(m.pos.z + kPlayerHeight * 0.75f == Approx(0.0f).margin(0.01f));
    CHECK(m.pos.x > 2.5f);
}

TEST_CASE("ascending into the surface jumps out of the water", "[collision_swim]") {
    std::vector<Facet> ground;
    addFloor(ground, -20.0f);
    std::vector<Facet> water;
    std::vector<Facet> up;
    addFloor(up, 0.0f);
    for (const auto& f : up) water.push_back(underside(f));
    auto solid = sweeperOver(ground);
    auto liquid = sweeperOver(water);
    Mover m;
    m.swimming = true;
    m.pos = {0, 0, -2};
    const bool jumped = swimFlyStep(m, 500, 3.0f, {0, 0, 1}, solid, &liquid, false, true);
    CHECK(jumped);
    CHECK_FALSE(m.swimming);
    CHECK(m.falling);
    CHECK(m.fallV0 == Approx(kSwimJumpSpeed));
}

TEST_CASE("a flyer slides along a wall and keeps its height", "[collision_swim]") {
    std::vector<Facet> world;
    // A wall across x = 1, facing -x; nothing under it.
    addQuad(world, {1, -50, -50}, {1, -50, 50}, {1, 50, 50}, {1, 50, -50});
    auto solid = sweeperOver(world);
    Mover m;
    m.flying = true;
    m.pos = {0, 0, 10};
    swimFlyStep(m, 500, 3.0f, glm::normalize(glm::vec3(1, 1, 0)), solid, nullptr, true, false);
    CHECK(m.pos.x <= 1.0f - kPlayerHalfWidth + 0.01f);
    CHECK(m.pos.y > 1.5f);
    CHECK(m.pos.z == Approx(10.0f).margin(1e-4f));
    CHECK_FALSE(m.falling);
}

TEST_CASE("a flyer meets the ground like any face and does not keep to it", "[collision_swim]") {
    std::vector<Facet> world;
    addFloor(world, 0.0f);
    auto solid = sweeperOver(world);
    Mover m;
    m.flying = true;
    m.pos = {0, 0, 1};
    // Diving at 45 degrees: stopped by the ground and slid along it.
    swimFlyStep(m, 500, 4.0f, glm::normalize(glm::vec3(1, 0, -1)), solid, nullptr, true, false);
    CHECK(m.pos.z == Approx(0.0f).margin(0.01f));
    CHECK(m.pos.x > 1.5f);
}

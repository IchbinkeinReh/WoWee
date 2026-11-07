// The client's ground movement (collision_walk.hpp, 0x007620f0 with the
// step-up 0x00761b00): walking on, up, into and off things.

#include <catch_amalgamated.hpp>

#include "rendering/collision_walk.hpp"

#include <cmath>
#include <vector>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {

void addQuad(std::vector<Facet>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::vec3& d) {
    out.push_back(makeFacet(windingNormal(a, b, c), a, b, c));
    out.push_back(makeFacet(windingNormal(a, c, d), a, c, d));
}

// A floor at height z over x in [x0, x1], y in [-10, 10], facing up.
void addFloor(std::vector<Facet>& f, float x0, float x1, float z) {
    addQuad(f, {x0, -10, z}, {x1, -10, z}, {x1, 10, z}, {x0, 10, z});
}

// A block from x0 on, `h` high: its front face facing -x and its top.
void addBlock(std::vector<Facet>& f, float x0, float h) {
    addQuad(f, {x0, -10, 0}, {x0, -10, h}, {x0, 10, h}, {x0, 10, 0});
    addFloor(f, x0, 20.0f, h);
}

Sweeper sweeperOver(const std::vector<Facet>& world) {
    return Sweeper([&world](const glm::vec3&, const glm::vec3&, std::vector<Facet>& out) {
        out.insert(out.end(), world.begin(), world.end());
    });
}

}  // namespace

TEST_CASE("a walk on level ground goes the whole way and stays on it", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 20, 0);
    auto s = sweeperOver(world);
    Mover m;
    const WalkResult r = walk(m, {1, 0}, 1.0f, 140, s);
    CHECK_FALSE(r.fell);
    CHECK(m.pos.x == Approx(1.0f).margin(1e-3f));
    CHECK(m.pos.z == Approx(0.0f).margin(1e-3f));
    CHECK(r.groundFacet >= 0);
}

TEST_CASE("a wall stops the walk at the body's side", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 20, 0);
    addBlock(world, 0.8f, 10.0f);
    auto s = sweeperOver(world);
    Mover m;
    const WalkResult r = walk(m, {1, 0}, 2.0f, 280, s);
    CHECK_FALSE(r.fell);
    CHECK(r.stopped);
    CHECK(m.pos.x == Approx(0.8f - kPlayerHalfWidth).margin(0.01f));
    CHECK(m.pos.z == Approx(0.0f).margin(1e-3f));
    CHECK_FALSE(m.stepping);
}

TEST_CASE("a wall met at an angle is slid along", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 20, 0);
    addBlock(world, 0.8f, 10.0f);
    auto s = sweeperOver(world);
    Mover m;
    const glm::vec2 dir = glm::normalize(glm::vec2(1, 1));
    walk(m, dir, 2.0f, 280, s);
    CHECK(m.pos.x == Approx(0.8f - kPlayerHalfWidth).margin(0.01f));
    CHECK(m.pos.y > 0.8f);
}

TEST_CASE("a step lower than the step height is climbed", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 1.0f, 0);
    addBlock(world, 1.0f, 0.5f);
    auto s = sweeperOver(world);
    Mover m;
    // Several frames, as the movement runs them.
    for (int i = 0; i < 12; ++i) walk(m, {1, 0}, 0.25f, 35, s);
    CHECK_FALSE(m.falling);
    CHECK(m.pos.x > 1.5f);
    CHECK(m.pos.z == Approx(0.5f).margin(0.02f));
}

TEST_CASE("a ledge higher than the step height is not climbed", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 1.0f, 0);
    addBlock(world, 1.0f, 1.5f);
    auto s = sweeperOver(world);
    Mover m;
    for (int i = 0; i < 12; ++i) walk(m, {1, 0}, 0.25f, 35, s);
    CHECK(m.pos.x < 1.0f);
    CHECK(m.pos.z < 1.1f);
}

TEST_CASE("walking off an edge starts a fall", "[collision_walk]") {
    std::vector<Facet> world;
    addFloor(world, -20, 0.5f, 0);
    auto s = sweeperOver(world);
    Mover m;
    bool fell = false;
    for (int i = 0; i < 8 && !fell; ++i) fell = walk(m, {1, 0}, 0.25f, 35, s).fell;
    CHECK(fell);
    CHECK(m.falling);
    CHECK(m.pos.x > 0.5f);
}

TEST_CASE("a slope gentler than 50 degrees is walked up", "[collision_walk]") {
    const float rise = std::tan(glm::radians(30.0f));
    std::vector<Facet> world;
    addFloor(world, -20, 1.0f, 0);
    addQuad(world, {1, -10, 0}, {11, -10, 10 * rise}, {11, 10, 10 * rise}, {1, 10, 0});
    auto s = sweeperOver(world);
    Mover m;
    for (int i = 0; i < 12; ++i) walk(m, {1, 0}, 0.25f, 35, s);
    CHECK_FALSE(m.falling);
    CHECK(m.pos.x > 2.5f);
    CHECK(m.pos.z == Approx((m.pos.x - 1.0f) * rise).margin(0.05f));
}

TEST_CASE("a slope steeper than 50 degrees stops the walk once the step height is used",
          "[collision_walk]") {
    const float rise = std::tan(glm::radians(65.0f));
    std::vector<Facet> world;
    addFloor(world, -20, 1.0f, 0);
    addQuad(world, {1, -10, 0}, {11, -10, 10 * rise}, {11, 10, 10 * rise}, {1, 10, 0});
    auto s = sweeperOver(world);
    Mover m;
    for (int i = 0; i < 30; ++i) walk(m, {1, 0}, 0.25f, 35, s);
    // No higher than a step up it.
    CHECK(m.pos.z <= 1.0f + 0.05f);
    CHECK(m.pos.x < 1.0f + 1.0f / rise + 0.1f);
}

TEST_CASE("walking down a gentle slope keeps to it", "[collision_walk]") {
    const float drop = std::tan(glm::radians(30.0f));
    std::vector<Facet> world;
    addQuad(world, {-1, -10, 0}, {10, -10, -11 * drop}, {10, 10, -11 * drop}, {-1, 10, 0});
    auto s = sweeperOver(world);
    Mover m;
    m.pos = {0.0f, 0.0f, -drop};
    for (int i = 0; i < 12; ++i) {
        const WalkResult r = walk(m, {1, 0}, 0.25f, 35, s);
        REQUIRE_FALSE(r.fell);
    }
    CHECK(m.pos.z == Approx(-(m.pos.x + 1.0f) * drop).margin(0.05f));
}

TEST_CASE("the step budget is the step height less what stepping has climbed",
          "[collision_walk]") {
    Mover m;
    CHECK(stepBudget(m) == Approx(1.0f));
    m.stepping = true;
    m.stepBaseZ = 0.0f;
    m.pos.z = 0.4f;
    CHECK(stepBudget(m) == Approx(0.6f));
    m.pos.z = 1.5f;
    CHECK(stepBudget(m) == 0.0f);
    m.player = false;
    m.stepping = false;
    CHECK(stepBudget(m) == Approx(2.0f));
}

TEST_CASE("a slide pushes off the face and keeps within what is left", "[collision_walk]") {
    // Head on: nothing left but the push-off back.
    glm::vec3 slide = slideAlong({1, 0, 0}, 1.0f, 1.0f, {-1, 0, 0});
    CHECK(slide.x == Approx(-1.001f));
    // At 45 degrees: along the face.
    const glm::vec3 d = glm::normalize(glm::vec3(1, 1, 0));
    slide = slideAlong(d, 1.0f, 1.0f, {-1, 0, 0});
    const glm::vec3 total = d + slide;
    CHECK(total.x == Approx(-0.001f).margin(1e-4f));
    CHECK(total.y == Approx(d.y));
    // A face overhead does not slide.
    CHECK(slideAlong(d, 1.0f, 1.0f, glm::normalize(glm::vec3(-1, 0, -0.3f))) == glm::vec3(0.0f));
}

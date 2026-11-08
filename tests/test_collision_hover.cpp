// Hovering (collision_hover.hpp, 0x00762980 with the probe 0x0075f520):
// held the hover height over the ground, made up at 3.5 yards a second,
// a fall where the ground drops away, and a slide along a wall.

#include <catch_amalgamated.hpp>

#include "rendering/collision_hover.hpp"

#include <vector>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {

void addQuad(std::vector<Facet>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::vec3& d) {
    out.push_back(makeFacet(windingNormal(a, b, c), a, b, c));
    out.push_back(makeFacet(windingNormal(a, c, d), a, c, d));
}

Sweeper sweeperOver(const std::vector<Facet>& world) {
    return Sweeper([&world](const glm::vec3&, const glm::vec3&, std::vector<Facet>& out) {
        out.insert(out.end(), world.begin(), world.end());
    });
}

}  // namespace

TEST_CASE("the probe measures how far over the ground a hoverer is", "[collision_hover]") {
    std::vector<Facet> world;
    addQuad(world, {-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0});
    auto s = sweeperOver(world);
    Mover m;
    m.hoverHeight = 2.0f;
    m.pos = {0, 0, 1.5f};
    HoverProbe p = hoverProbe(m, s);
    REQUIRE(p.found);
    CHECK(p.height == Approx(1.5f).margin(1e-3f));
    // Beyond twice the hover height: no ground.
    m.pos.z = 4.5f;
    CHECK_FALSE(hoverProbe(m, s).found);
}

TEST_CASE("a hoverer rises to its hover height at 3.5 yards a second", "[collision_hover]") {
    std::vector<Facet> world;
    addQuad(world, {-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0});
    auto s = sweeperOver(world);
    Mover m;
    m.hoverHeight = 2.0f;
    m.pos = {0, 0, 0.5f};
    // A tenth of a second: up 0.35 at most.
    REQUIRE(hoverStep(m, 100, 0.0f, {1, 0, 0}, s, false));
    CHECK(m.pos.z == Approx(0.85f).margin(1e-3f));
    for (int i = 0; i < 20; ++i) hoverStep(m, 100, 0.0f, {1, 0, 0}, s, false);
    CHECK(m.pos.z == Approx(2.0f).margin(0.01f));
    // Moving along, it keeps the height.
    for (int i = 0; i < 10; ++i) hoverStep(m, 100, 0.7f, {1, 0, 0}, s, false);
    CHECK(m.pos.z == Approx(2.0f).margin(0.01f));
    CHECK(m.pos.x == Approx(7.0f).margin(0.01f));
}

TEST_CASE("with no ground under it a hoverer falls", "[collision_hover]") {
    std::vector<Facet> world;
    auto s = sweeperOver(world);
    Mover m;
    m.hoverHeight = 2.0f;
    m.pos = {0, 0, 10};
    CHECK_FALSE(hoverStep(m, 100, 0.5f, {1, 0, 0}, s, false));
    CHECK(m.falling);
    // Gravity off: it does not.
    Mover g;
    g.hoverHeight = 2.0f;
    g.pos = {0, 0, 10};
    CHECK(hoverStep(g, 100, 0.5f, {1, 0, 0}, s, true));
    CHECK_FALSE(g.falling);
}

TEST_CASE("a hoverer slides along a wall", "[collision_hover]") {
    std::vector<Facet> world;
    addQuad(world, {-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0});
    addQuad(world, {1, -50, -1}, {1, -50, 30}, {1, 50, 30}, {1, 50, -1});
    auto s = sweeperOver(world);
    Mover m;
    m.hoverHeight = 1.0f;
    m.pos = {0, 0, 1.0f};
    hoverStep(m, 500, 3.0f, glm::normalize(glm::vec3(1, 1, 0)), s, false);
    CHECK(m.pos.x <= 1.0f - kPlayerHalfWidth + 0.01f);
    CHECK(m.pos.y > 1.5f);
    CHECK(m.pos.z == Approx(1.0f).margin(0.01f));
}

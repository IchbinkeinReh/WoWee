// The client's body sweep (collision_sweep.hpp, 0x0075f9d0): the body's
// shape, the clip and the solve it is built from, and what a sweep answers
// against floors, walls and slopes.

#include <catch_amalgamated.hpp>

#include "rendering/collision_sweep.hpp"

#include <cmath>
#include <vector>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {

// Two triangles making a square with the given corners, wound so the
// normal is `n`.
void addQuad(std::vector<Facet>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
             const glm::vec3& d) {
    out.push_back(makeFacet(windingNormal(a, b, c), a, b, c));
    out.push_back(makeFacet(windingNormal(a, c, d), a, c, d));
}

std::vector<Facet> floorAt(float z, float half = 10.0f) {
    std::vector<Facet> f;
    addQuad(f, {-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z});
    return f;
}

// A wall across x = `x`, facing -x (towards a body coming from the origin).
std::vector<Facet> wallAt(float x) {
    std::vector<Facet> f;
    addQuad(f, {x, -10, -10}, {x, -10, 10}, {x, 10, 10}, {x, 10, -10});
    return f;
}

}  // namespace

TEST_CASE("the body is a column on a pyramid", "[collision_sweep]") {
    const Body b({1, 2, 3}, kPlayerHalfWidth, kPlayerHeight);
    CHECK(b.v[0] == glm::vec3(1, 2, 3));
    // The column's foot 1.849399 r up: 0.6164663 for a player.
    CHECK(b.v[1].z - 3.0f == Approx(0.6164663f).margin(1e-6f));
    CHECK(b.v[5].z - 3.0f == Approx(73.0f / 36.0f));
    // Every vertex is on or behind every plane, and each face's own
    // vertices are on its plane.
    for (int i = 0; i < 9; ++i) {
        for (int k = 0; k < 9; ++k) CHECK(b.p[i].at(b.v[k]) <= 1e-5f);
        for (int k = 0; k < 4 && kBodyFaces[i][k] >= 0; ++k)
            CHECK(b.p[i].at(b.v[kBodyFaces[i][k]]) == Approx(0.0f).margin(1e-5f));
        CHECK(glm::length(b.p[i].n) == Approx(1.0f).margin(1e-6f));
    }
}

TEST_CASE("a clip keeps what is behind the plane, with the client's tolerance",
          "[collision_sweep]") {
    std::vector<glm::vec3> scratch;
    const Plane px{{1, 0, 0}, -1.0f};  // keeps x < 1
    std::vector<glm::vec3> tri = {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}};
    clipFacet(tri, px, scratch);
    REQUIRE(tri.size() == 4);
    for (const auto& v : tri) CHECK(v.x <= 1.0f + 1e-6f);
    // Within 1/720 in front counts as behind: untouched.
    std::vector<glm::vec3> near = {{0, 0, 0}, {1.001f, 0, 0}, {0, 2, 0}};
    clipFacet(near, px, scratch);
    CHECK(near.size() == 3);
    CHECK(near[1].x == Approx(1.001f));
    // None of it more than 1/720 behind: gone.
    std::vector<glm::vec3> beyond = {{0.9995f, 0, 0}, {3, 0, 0}, {3, 2, 0}};
    clipFacet(beyond, px, scratch);
    CHECK(beyond.empty());
}

TEST_CASE("falling onto a floor stops the point on it", "[collision_sweep]") {
    const auto floor = floorAt(0.0f);
    const Body b({0.2f, -0.1f, 0.5f}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {0, 0, -1}, 2.0f, floor);
    REQUIRE(r.hit);
    CHECK(r.allowed == Approx(0.5f).margin(1e-4f));
    // The four pyramid faces touch at once, at the point.
    CHECK(r.contacts.size() >= 1);
    for (int c : r.contacts) CHECK(c >= 5);
}

TEST_CASE("a free fall short of the floor is not stopped", "[collision_sweep]") {
    const auto floor = floorAt(0.0f);
    const Body b({0, 0, 3.0f}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {0, 0, -1}, 1.0f, floor);
    CHECK_FALSE(r.hit);
    CHECK(r.allowed == Approx(1.0f));
}

TEST_CASE("a wall stops the column's face", "[collision_sweep]") {
    const auto wall = wallAt(1.0f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 2.0f, wall);
    REQUIRE(r.hit);
    CHECK(r.allowed == Approx(1.0f - kPlayerHalfWidth).margin(1e-4f));
    bool column = false;
    for (int c : r.contacts) column |= (c == 1);
    CHECK(column);
}

TEST_CASE("a face is solid from its front only", "[collision_sweep]") {
    // The same wall turned to face +x: walked into from behind, not there.
    std::vector<Facet> back;
    addQuad(back, {1, 10, -10}, {1, 10, 10}, {1, -10, 10}, {1, -10, -10});
    REQUIRE(back[0].n.x > 0.9f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 2.0f, back);
    CHECK_FALSE(r.hit);
    CHECK(r.allowed == Approx(2.0f));
}

TEST_CASE("walking along a floor stood on meets nothing", "[collision_sweep]") {
    const auto floor = floorAt(0.0f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 1.0f, floor);
    CHECK_FALSE(r.hit);
    CHECK(r.allowed == Approx(1.0f));
}

TEST_CASE("a slope gentler than the pyramid's faces meets the point", "[collision_sweep]") {
    // A ramp rising at 20 degrees from x = 1, walked into level.
    const float rise = std::tan(glm::radians(20.0f));
    std::vector<Facet> ramp;
    addQuad(ramp, {1, -5, 0}, {6, -5, 5 * rise}, {6, 5, 5 * rise}, {1, 5, 0});
    REQUIRE(ramp[0].n.z > 0.9f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 3.0f, ramp);
    REQUIRE(r.hit);
    // The point reaches the foot of the ramp first.
    CHECK(r.allowed == Approx(1.0f).margin(1e-3f));
    bool pyramid = false;
    for (int c : r.contacts) pyramid |= (c >= 5);
    CHECK(pyramid);
}

TEST_CASE("a slope steeper than the pyramid meets the column's foot", "[collision_sweep]") {
    // 75 degrees from x = 1, steeper than the pyramid's 61.6: the column's
    // foot reaches it before the point does.
    const float rise = std::tan(glm::radians(75.0f));
    std::vector<Facet> ramp;
    addQuad(ramp, {1, -5, 0}, {4, -5, 3 * rise}, {4, 5, 3 * rise}, {1, 5, 0});
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 3.0f, ramp);
    REQUIRE(r.hit);
    // The column's foot edge at x = r, z = 0.6165 meets the ramp where
    // x = 1 + 0.6165 / tan 75.
    const float zb = kPyramidRise * kPlayerHalfWidth;
    CHECK(r.allowed == Approx(1.0f + zb / rise - kPlayerHalfWidth).margin(2e-3f));
}

TEST_CASE("a face already deep in the body is passed through", "[collision_sweep]") {
    // A small floor a yard up, inside the column: the body stands through
    // it, and a step down does not stop on it.
    const auto inside = floorAt(1.0f, 0.1f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {0, 0, -1}, 1.0f, inside);
    CHECK_FALSE(r.hit);
}

TEST_CASE("a floor cutting through the pyramid stops a step down at once", "[collision_sweep]") {
    // Half a yard up the floor crosses the pyramid's faces; the part of it
    // under them is outside the body and right in their way.
    const auto floor = floorAt(0.5f);
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {0, 0, -1}, 1.0f, floor);
    REQUIRE(r.hit);
    CHECK(r.allowed == 0.0f);
}

TEST_CASE("a move too small to measure goes nowhere", "[collision_sweep]") {
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 1e-7f, wallAt(0.4f));
    CHECK_FALSE(r.hit);
    CHECK(r.allowed == 0.0f);
}

TEST_CASE("a touch closer than 1/720 allows no move at all", "[collision_sweep]") {
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    const SweepResult r = sweep(b, {1, 0, 0}, 1.0f, wallAt(kPlayerHalfWidth + 0.0005f));
    REQUIRE(r.hit);
    CHECK(r.allowed == 0.0f);
}

TEST_CASE("the gather box holds the body at both ends with a margin", "[collision_sweep]") {
    const Body b({0, 0, 0}, kPlayerHalfWidth, kPlayerHeight);
    glm::vec3 lo, hi;
    sweepBox(b, {1, 0, 0}, 2.0f, lo, hi);
    CHECK(lo.x == Approx(-kPlayerHalfWidth - kGatherPad));
    CHECK(hi.x == Approx(2.0f + kPlayerHalfWidth + kGatherPad));
    CHECK(lo.z == Approx(-kGatherPad));
    CHECK(hi.z == Approx(kPlayerHeight + kGatherPad));
}

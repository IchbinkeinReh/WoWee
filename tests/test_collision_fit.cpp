// Whether a grown unit still fits where it stands (collision_fit.hpp,
// 0x0072a560): the grown body's size (0x006e8ff0), the body's overlap test
// with its 1/36-yard allowance at the feet (0x0075c800, 0x0075c6f0), the
// mounted rider's box (0x006d7720) and its overlap test (0x0075c750).

#include <catch_amalgamated.hpp>

#include "rendering/collision_fit.hpp"

#include <cmath>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {
Facet tri(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return makeFacet(windingNormal(a, b, c), a, b, c);
}
// A wide level face at height z, as two triangles.
std::vector<Facet> levelAt(float z) {
    return {tri({-10, -10, z}, {10, -10, z}, {10, 10, z}), tri({-10, -10, z}, {10, 10, z}, {-10, 10, z})};
}
// A level face at height z from x = x0 outward.
std::vector<Facet> ledgeAt(float x0, float z) {
    return {tri({x0, -5, z}, {5, -5, z}, {5, 5, z}), tri({x0, -5, z}, {5, 5, z}, {x0, 5, z})};
}
}  // namespace

TEST_CASE("the grown body is the model's box at the collision scale", "[collision_fit]") {
    // 0.6 wide and 2 tall at scale 1.5: the half-width 0.45 times the
    // pyramid's rise is 0.83, under the scale, so it stands.
    const FitExtents e = growthFitExtents(0.6f, 2.0f, 1.5f);
    CHECK(e.height == Approx(3.0f));
    CHECK(e.halfWidth == Approx(0.45f));
}

TEST_CASE("a wide model's grown half-width is capped by the scale", "[collision_fit]") {
    // Half-width 1 at scale 1 rises 1.85, more than the scale.
    const FitExtents e = growthFitExtents(2.0f, 3.0f, 1.0f);
    CHECK(e.halfWidth == Approx(kFitHalfWidthCap));
    const FitExtents big = growthFitExtents(2.0f, 3.0f, 2.0f);
    CHECK(big.halfWidth == Approx(2.0f * kFitHalfWidthCap));
    CHECK(big.height == Approx(6.0f));
}

TEST_CASE("the ground under the feet does not stop a body growing", "[collision_fit]") {
    const Body body(glm::vec3(0.0f), 0.45f, 3.0f);
    CHECK_FALSE(bodyOverlapsFacets(body, levelAt(0.0f)));
    CHECK_FALSE(bodyOverlapsFacets(body, levelAt(-0.5f)));
}

TEST_CASE("a ceiling inside the grown height is in the way", "[collision_fit]") {
    const Body body(glm::vec3(0.0f), 0.45f, 3.0f);
    CHECK(bodyOverlapsFacets(body, levelAt(2.5f)));
    CHECK_FALSE(bodyOverlapsFacets(body, levelAt(3.5f)));
}

TEST_CASE("a wall the column reaches is in the way, one beside it is not", "[collision_fit]") {
    const Body body(glm::vec3(0.0f), 0.45f, 3.0f);
    CHECK(bodyOverlapsFacets(body, {tri({0.3f, -5, 0}, {0.3f, 5, 0}, {0.3f, 0, 5})}));
    CHECK_FALSE(bodyOverlapsFacets(body, {tri({0.6f, -5, 0}, {0.6f, 5, 0}, {0.6f, 0, 5})}));
}

TEST_CASE("the pyramid under the column leaves low ground round the feet out", "[collision_fit]") {
    const Body body(glm::vec3(0.0f), 0.45f, 3.0f);
    // At x = 0.4 the pyramid's face is 1.85 x 0.4 = 0.74 over the feet: a
    // ledge at 0.3 there is under the body, one at 1.0 reaches into it.
    CHECK_FALSE(bodyOverlapsFacets(body, ledgeAt(0.4f, 0.3f)));
    CHECK(bodyOverlapsFacets(body, ledgeAt(0.4f, 1.0f)));
}

TEST_CASE("what is left within 1/36 of a yard of the feet does not count", "[collision_fit]") {
    const Body body(glm::vec3(0.0f), 0.45f, 3.0f);
    // A sliver rising through the point at the feet: only its tip, 1/72 of
    // a yard over them, is inside the body.
    const float k = 1.0f / 72.0f;
    CHECK_FALSE(bodyOverlapsFacets(body, {tri({-k, -k, -1.0f}, {k, -k, -1.0f}, {0, 0, k})}));
}

TEST_CASE("the mounted box is the slab the rider grows into", "[collision_fit]") {
    // A rider 2 tall on a mount height of 1.5 at display scale 1 stands
    // 1 + 1.5 = 2.5 high.
    CHECK(mountedRiderHeight(2.0f, 1.5f, 1.0f) == Approx(2.5f));
    const auto box = mountedGrowthBox(glm::vec3(1, 2, 3), 0.3f, 2.5f, 2.0f, 1.5f, 1.0f, 1.5f);
    REQUIRE(box);
    CHECK(box->lo.x == Approx(0.7f));
    CHECK(box->lo.y == Approx(1.7f));
    CHECK(box->lo.z == Approx(5.5f));
    CHECK(box->hi.x == Approx(1.3f));
    CHECK(box->hi.y == Approx(2.3f));
    CHECK(box->hi.z == Approx(3.0f + 2.5f * 1.5f));
}

TEST_CASE("a rider taller than its seat height is not tested", "[collision_fit]") {
    // Half of 4 plus a mount height of 1 is 3, under the rider's own 4.
    CHECK(mountedRiderHeight(4.0f, 1.0f, 1.0f) == Approx(4.0f));
    CHECK_FALSE(mountedGrowthBox(glm::vec3(0.0f), 0.3f, 4.0f, 4.0f, 1.0f, 1.0f, 2.0f));
}

TEST_CASE("a face is in the mounted box only where some of it is inside", "[collision_fit]") {
    const glm::vec3 lo(-0.5f, -0.5f, 2.0f), hi(0.5f, 0.5f, 3.0f);
    CHECK(boxOverlapsFacets(lo, hi, levelAt(2.5f)));
    CHECK_FALSE(boxOverlapsFacets(lo, hi, levelAt(3.5f)));
    CHECK_FALSE(boxOverlapsFacets(lo, hi, levelAt(1.0f)));
    CHECK(boxOverlapsFacets(lo, hi, {tri({-0.1f, -0.1f, 2.5f}, {0.1f, -0.1f, 2.5f}, {0, 0.1f, 2.5f})}));
    CHECK_FALSE(boxOverlapsFacets(lo, hi, {}));
}

TEST_CASE("the six box planes keep the inside", "[collision_fit]") {
    Plane p[6];
    boxPlanes(glm::vec3(-1, -2, -3), glm::vec3(1, 2, 3), p);
    for (const Plane& plane : p) CHECK(plane.at(glm::vec3(0.0f)) < 0.0f);
    CHECK(p[0].at(glm::vec3(-1.5f, 0, 0)) > 0.0f);
    CHECK(p[5].at(glm::vec3(0, 0, 3.5f)) > 0.0f);
}

TEST_CASE("only growing is tested", "[collision_fit]") {
    CHECK(scaleGrew(1.5f, 1.0f));
    CHECK_FALSE(scaleGrew(1.0f, 1.0f));
    CHECK_FALSE(scaleGrew(1.0f, 1.5f));
    CHECK(scaleGrew(std::nanf(""), 1.0f));
}

TEST_CASE("the gather is only for a box inside the map", "[collision_fit]") {
    CHECK(boxInsideMap(glm::vec3(-100.0f), glm::vec3(100.0f)));
    CHECK_FALSE(boxInsideMap(glm::vec3(17066.0f, 0, 0), glm::vec3(17067.0f, 1, 1)));
    CHECK_FALSE(boxInsideMap(glm::vec3(-17067.0f, 0, 0), glm::vec3(-17066.0f, 1, 1)));
}

// The faces the movement sweep is given (collision_facets.hpp): which way each
// source's normals point, which faces each source leaves out, and the box
// test that drops a triangle only when all of it lies beyond one side.

#include <catch_amalgamated.hpp>

#include "rendering/collision_facets.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering::collision;
using Catch::Approx;

namespace {
const glm::vec3 kBigLo(-100.0f), kBigHi(100.0f);
}

TEST_CASE("a facet's plane passes through its corners", "[collision_facets]") {
    const glm::vec3 a(1, 2, 3), b(4, 2, 3), c(1, 5, 3);
    const Facet f = makeFacet(windingNormal(a, b, c), a, b, c);
    for (const auto& v : f.v) CHECK(glm::dot(f.n, v) + f.d == Approx(0.0f).margin(1e-5f));
    CHECK(f.n.z == Approx(1.0f));
}

TEST_CASE("a facet is met only by a motion into its front", "[collision_facets]") {
    const Facet f = makeFacet({0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    CHECK(facesMotion(f, {0, 0, -1}));
    CHECK_FALSE(facesMotion(f, {0, 0, 1}));
    // Along the face is not into it: the client's limit is -1e-5.
    CHECK_FALSE(facesMotion(f, {1, 0, 0}));
    CHECK_FALSE(facesMotion(f, glm::normalize(glm::vec3(1, 0, -0.5e-5f))));
    CHECK(facesMotion(f, glm::normalize(glm::vec3(1, 0, -2e-5f))));
}

TEST_CASE("a triangle without area stands up", "[collision_facets]") {
    const glm::vec3 n = windingNormal({0, 0, 0}, {1, 1, 1}, {2, 2, 2});
    CHECK(n == glm::vec3(0, 0, 1));
}

TEST_CASE("a triangle is dropped only when all of it is beyond one side", "[collision_facets]") {
    const glm::vec3 lo(0.0f), hi(1.0f);
    // All three corners outside, on different sides, but the face crosses the
    // box: kept.
    CHECK_FALSE(outsideBox({-5, 0.5f, 0.5f}, {5, -5, 0.5f}, {5, 5, 0.5f}, lo, hi));
    // All beyond +x: dropped.
    CHECK(outsideBox({2, 0, 0}, {3, 1, 1}, {2, 5, -5}, lo, hi));
    // One corner inside: kept.
    CHECK_FALSE(outsideBox({0.5f, 0.5f, 0.5f}, {3, 1, 1}, {2, 5, -5}, lo, hi));
}

TEST_CASE("terrain quads face up whatever their slope", "[collision_facets]") {
    std::vector<Facet> out;
    const glm::vec3 c(0.5f, 0.5f, 3.0f);
    const glm::vec3 p[4] = {{0, 0, 0}, {1, 0, 1}, {1, 1, 7}, {0, 1, 2}};
    appendTerrainQuad(c, p, kBigLo, kBigHi, out);
    REQUIRE(out.size() == 4);
    for (const auto& f : out) {
        CHECK(f.n.z > 0.0f);
        CHECK(glm::length(f.n) == Approx(1.0f));
    }
    // Corners given the other way round come out facing up all the same,
    // and wound to match.
    out.clear();
    const glm::vec3 q[4] = {p[3], p[2], p[1], p[0]};
    appendTerrainQuad(c, q, kBigLo, kBigHi, out);
    REQUIRE(out.size() == 4);
    for (const auto& f : out) {
        CHECK(f.n.z > 0.0f);
        CHECK(glm::dot(glm::cross(f.v[1] - f.v[0], f.v[2] - f.v[0]), f.n) > 0.0f);
    }
    // A box that takes one corner's triangles only.
    out.clear();
    appendTerrainQuad(c, p, glm::vec3(-0.1f, -0.1f, -1.0f), glm::vec3(0.05f, 0.05f, 0.5f), out);
    CHECK(out.size() == 2);
}

TEST_CASE("groups and faces a player's sweep passes over", "[collision_facets]") {
    CHECK(groupCollides(0x0));
    CHECK(groupCollides(0x2000 | 0x8));
    CHECK_FALSE(groupCollides(0x80));
    // Detail faces only; the collision flag 0x08 is not consulted, and a
    // rendered face with no collision flag still collides.
    CHECK(buildingFaceCollides(0x20));
    CHECK(buildingFaceCollides(0x08));
    CHECK(buildingFaceCollides(0x00));
    CHECK_FALSE(buildingFaceCollides(0x04));
    CHECK_FALSE(buildingFaceCollides(0x24));
}

TEST_CASE("a doodad's normals turn with it and ignore its scale", "[collision_facets]") {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(10, 20, 30));
    m = glm::rotate(m, glm::radians(90.0f), glm::vec3(0, 0, 1));
    m = glm::scale(m, glm::vec3(2.5f));
    const glm::mat3 axes = unitAxes(m);
    std::vector<Facet> out;
    const glm::vec3 fileN(1, 0, 0);
    appendDoodadTriangle(m, axes, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, &fileN, glm::vec3(-1000.0f),
                         glm::vec3(1000.0f), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].n.x == Approx(0.0f).margin(1e-6f));
    CHECK(out[0].n.y == Approx(1.0f));
    CHECK(glm::length(out[0].n) == Approx(1.0f));
    CHECK(out[0].v[1].x == Approx(10.0f - 2.5f));
    CHECK(glm::dot(out[0].n, out[0].v[2]) + out[0].d == Approx(0.0f).margin(1e-4f));
    // Without a file normal the winding stands in.
    out.clear();
    appendDoodadTriangle(m, axes, {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, nullptr, glm::vec3(-1000.0f),
                         glm::vec3(1000.0f), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].n.y == Approx(1.0f));
}

TEST_CASE("a box stands in for a doodad not yet loaded, facing out", "[collision_facets]") {
    std::vector<Facet> out;
    const glm::vec3 lo(-1, -2, 0), hi(1, 2, 3);
    appendBoxFacets(lo, hi, out);
    REQUIRE(out.size() == 12);
    const glm::vec3 centre = (lo + hi) * 0.5f;
    for (const auto& f : out) {
        CHECK(glm::length(f.n) == Approx(1.0f));
        // The centre is behind every face.
        CHECK(glm::dot(f.n, centre) + f.d < 0.0f);
        for (const auto& v : f.v) CHECK(glm::dot(f.n, v) + f.d == Approx(0.0f).margin(1e-5f));
    }
}

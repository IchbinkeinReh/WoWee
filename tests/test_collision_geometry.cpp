// The primitives every collision query is built from.
//
// The doodad and building renderers each carried their own copy. A difference
// between them does not crash or log: it moves where the world is solid, and
// the report reads as "I fell through the floor in one building".
//
// The expectations below are computed independently - a brute-force scan of
// the box surface for the distance test, and a plane-intersection solved by
// hand for the ray test - rather than by calling the functions a second way.
#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>
#include <vector>
#include <limits>

#include <glm/glm.hpp>

#include "rendering/collision_geometry.hpp"

using wowee::rendering::CollisionFocus;
using wowee::rendering::holdCylinderOffWallTriangle;
using wowee::rendering::pointAABBDistanceSq;
using wowee::rendering::rayTriangleIntersect;

namespace {

/// Nearest distance from a point to a box, found by sampling the box surface.
/// Slow and obviously correct, which is what an oracle has to be.
///
/// Each face is swept with two independent parameters. A first version of this
/// drove two of the three coordinates from the same one, so it only ever
/// sampled a diagonal of each face and reported a point 36.99 away from a box
/// whose nearest surface is exactly 36. The function under test was right and
/// the oracle was not, which is the failure an oracle is supposed to make
/// visible rather than the one it is supposed to cause.
float bruteForceDistanceSq(const glm::vec3& p, const glm::vec3& bmin,
                           const glm::vec3& bmax) {
    float best = std::numeric_limits<float>::max();
    const int steps = 60;
    const glm::vec3 span = bmax - bmin;
    for (int i = 0; i <= steps; ++i) {
        for (int j = 0; j <= steps; ++j) {
            const float u = static_cast<float>(i) / steps;
            const float v = static_cast<float>(j) / steps;
            const glm::vec3 faces[6] = {
                {bmin.x, bmin.y + span.y * u, bmin.z + span.z * v},
                {bmax.x, bmin.y + span.y * u, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmin.y, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmax.y, bmin.z + span.z * v},
                {bmin.x + span.x * u, bmin.y + span.y * v, bmin.z},
                {bmin.x + span.x * u, bmin.y + span.y * v, bmax.z},
            };
            for (const glm::vec3& q : faces) {
                const glm::vec3 d = p - q;
                best = std::min(best, glm::dot(d, d));
            }
        }
    }
    return best;
}

}  // namespace

TEST_CASE("distance to a box is zero inside it", "[collision]") {
    // Every caller relies on this: an instance the query point is inside must
    // never sort further away than one it is outside.
    const glm::vec3 bmin(-5.0f, -5.0f, -5.0f), bmax(5.0f, 5.0f, 5.0f);
    CHECK(pointAABBDistanceSq({0, 0, 0}, bmin, bmax) == Catch::Approx(0.0f));
    CHECK(pointAABBDistanceSq({4.9f, -4.9f, 0}, bmin, bmax) == Catch::Approx(0.0f));
    CHECK(pointAABBDistanceSq(bmax, bmin, bmax) == Catch::Approx(0.0f));
}

TEST_CASE("distance to a box matches a brute-force search", "[collision]") {
    const glm::vec3 bmin(-3.0f, 1.0f, -2.0f), bmax(4.0f, 6.0f, 9.0f);
    const glm::vec3 outside[] = {
        {10.0f, 3.0f, 0.0f},     // past one face
        {-8.0f, -4.0f, 0.0f},    // past an edge
        {12.0f, 20.0f, 30.0f},   // past a corner
        {0.0f, 0.0f, -6.0f},
    };
    for (const glm::vec3& p : outside) {
        INFO("point " << p.x << "," << p.y << "," << p.z);
        // The sampled surface is a lower bound on accuracy, so allow the grid.
        CHECK(pointAABBDistanceSq(p, bmin, bmax) ==
              Catch::Approx(bruteForceDistanceSq(p, bmin, bmax)).epsilon(0.02));
    }
}

TEST_CASE("a ray hits a triangle at the distance the plane says", "[collision]") {
    // Triangle in the z = 4 plane; a ray straight down the +z axis from the
    // origin must therefore report exactly 4.
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);
    const float t = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v1, v2);
    CHECK(t == Catch::Approx(4.0f));
}

TEST_CASE("a floor is solid from underneath", "[collision]") {
    // Two-sided on purpose. A version that culled backfaces would make every
    // floor one-way, and a character under a bridge or inside a building
    // would fall through the surface holding them up.
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);
    const float front = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v1, v2);
    // Same triangle, opposite winding: the same ray must still hit it.
    const float back = rayTriangleIntersect({0, 0, 0}, {0, 0, 1}, v0, v2, v1);
    CHECK(front == Catch::Approx(back));
    CHECK(back > 0.0f);
}

TEST_CASE("winding never changes the answer", "[collision]") {
    // Five call sites tested each triangle twice, the second time with the
    // vertices reversed, in case the first order missed. It cannot: the
    // barycentric bounds are symmetric in u and v, so reversing the winding
    // flips the sign of the determinant and swaps the two coordinates, and
    // both the hit test and the distance come out the same.
    //
    // Swept rather than argued: a deterministic spread of triangles and rays,
    // hits and misses alike.
    uint32_t seed = 12345;
    const auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1 << 24) * 20.0f - 10.0f;
    };
    int hits = 0;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 v0(next(), next(), next());
        const glm::vec3 v1(next(), next(), next());
        const glm::vec3 v2(next(), next(), next());
        const glm::vec3 origin(next(), next(), next());
        const glm::vec3 dir = glm::normalize(glm::vec3(next(), next(), next()) + glm::vec3(1e-3f));

        const float forward = rayTriangleIntersect(origin, dir, v0, v1, v2);
        const float reversed = rayTriangleIntersect(origin, dir, v0, v2, v1);
        if (forward > 0.0f) ++hits;
        // Both miss, or both hit at the same distance.
        REQUIRE((forward > 0.0f) == (reversed > 0.0f));
        if (forward > 0.0f) {
            REQUIRE(forward == Catch::Approx(reversed).epsilon(1e-4));
        }
    }
    // A sweep that never hits anything would prove nothing.
    CHECK(hits > 20);
}

TEST_CASE("a ray misses what it does not point at", "[collision]") {
    const glm::vec3 v0(-1.0f, -1.0f, 4.0f), v1(3.0f, -1.0f, 4.0f), v2(-1.0f, 3.0f, 4.0f);

    SECTION("outside the triangle but on its plane") {
        CHECK(rayTriangleIntersect({10, 10, 0}, {0, 0, 1}, v0, v1, v2) < 0.0f);
    }
    SECTION("parallel to the plane") {
        CHECK(rayTriangleIntersect({0, 0, 0}, {1, 0, 0}, v0, v1, v2) < 0.0f);
    }
    SECTION("pointing away from it") {
        // Behind the origin is a miss, not a negative-distance hit: a floor
        // above your head is not one you are standing on.
        CHECK(rayTriangleIntersect({0, 0, 0}, {0, 0, -1}, v0, v1, v2) < 0.0f);
    }
    SECTION("just past the far edge") {
        // u + v > 1 - the diagonal, which is the edge a barycentric test gets
        // wrong first if its bound is written as u > 1 || v > 1.
        CHECK(rayTriangleIntersect({2.5f, 2.5f, 0}, {0, 0, 1}, v0, v1, v2) < 0.0f);
    }
}

TEST_CASE("an unset focus excludes nothing", "[collision]") {
    // Radius zero means no restriction. Reading it as an empty sphere would
    // make the whole world non-solid rather than merely slow, which is the
    // failure worth being explicit about.
    CollisionFocus focus;
    CHECK_FALSE(focus.excludes({-1, -1, -1}, {1, 1, 1}));
    CHECK_FALSE(focus.excludes({1000, 1000, 1000}, {1001, 1001, 1001}));

    SECTION("and setting it to zero radius clears it again") {
        focus.set({0, 0, 0}, 5.0f);
        CHECK(focus.enabled);
        focus.set({0, 0, 0}, 0.0f);
        CHECK_FALSE(focus.enabled);
        CHECK_FALSE(focus.excludes({1000, 1000, 1000}, {1001, 1001, 1001}));
    }
}

TEST_CASE("a focus measures to the box, not to its centre", "[collision]") {
    // A long building whose origin is far away is still under the player's
    // feet at one end. Measuring to the centre would drop it out of the query.
    CollisionFocus focus;
    focus.set({0, 0, 0}, 10.0f);

    const glm::vec3 longMin(-5.0f, -1.0f, -1.0f), longMax(500.0f, 1.0f, 1.0f);
    CHECK_FALSE(focus.excludes(longMin, longMax));

    SECTION("and something wholly beyond the radius is excluded") {
        CHECK(focus.excludes({100, 100, 100}, {101, 101, 101}));
    }
    SECTION("the boundary is the radius itself, inclusive") {
        // Exactly at the radius is kept: the test is a strict greater-than,
        // and an instance exactly at the edge is one the player can touch.
        CHECK_FALSE(focus.excludes({10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}));
        CHECK(focus.excludes({10.001f, 0.0f, 0.0f}, {10.001f, 0.0f, 0.0f}));
    }
}

namespace {

/// A plate as the collision mesh has it: an upright quad in the plane x = `x`,
/// from y -2..2 and z 0..4, once facing each way - a door's two faces, back to
/// back with nothing between.
struct Plate {
    float x;
    std::vector<std::array<glm::vec3, 3>> tris() const {
        const glm::vec3 a(x, -2, 0), b(x, 2, 0), c(x, 2, 4), d(x, -2, 4);
        return {{a, b, c}, {a, c, d}, {a, c, b}, {a, d, c}};
    }
};

/// Walk a character in steps the size of a running frame, every face holding
/// it off as the renderer does, and answer where it ends.
glm::vec3 walk(const std::vector<std::array<glm::vec3, 3>>& tris, glm::vec3 pos,
               const glm::vec3& step, int frames) {
    for (int f = 0; f < frames; ++f) {
        glm::vec3 to = pos + step;
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& t : tris) {
                holdCylinderOffWallTriangle(pos, to, 0.5f, 2.0f, t[0], t[1], t[2]);
            }
        }
        pos = to;
    }
    return pos;
}

}  // namespace

TEST_CASE("a plate holds a running character on the side it came from", "[collision]") {
    // Seven yards a second at sixty frames, at a door with nothing behind its
    // face: the step ends past the middle of the plate, nearest the far face.
    const auto tris = Plate{0.0f}.tris();
    const glm::vec3 end = walk(tris, {-3.0f, 0.0f, 0.0f}, {0.117f, 0.0f, 0.0f}, 120);
    CHECK(end.x < -0.49f);
    CHECK(end.x > -0.52f);

    SECTION("from the other side as well") {
        const glm::vec3 back = walk(tris, {3.0f, 0.0f, 0.0f}, {-0.117f, 0.0f, 0.0f}, 120);
        CHECK(back.x > 0.49f);
    }
    SECTION("however long the step") {
        // A hitch of a quarter second: the whole step is past the plate.
        const glm::vec3 far = walk(tris, {-1.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, 1);
        CHECK(far.x < -0.49f);
    }
}

TEST_CASE("a character walking into a plate at an angle slides along it", "[collision]") {
    const auto tris = Plate{0.0f}.tris();
    const glm::vec3 end = walk(tris, {-1.0f, -1.5f, 0.0f}, {0.08f, 0.08f, 0.0f}, 10);
    // Held off the face, and the part of each step along it kept.
    CHECK(end.x < -0.49f);
    CHECK(end.y == Catch::Approx(-1.5f + 0.8f).margin(1e-3));
}

TEST_CASE("a plate does not reach past its edge or over its top", "[collision]") {
    const auto tris = Plate{0.0f}.tris();
    SECTION("a step that passes a radius beyond the end goes on") {
        const glm::vec3 end = walk(tris, {-3.0f, 2.6f, 0.0f}, {0.117f, 0.0f, 0.0f}, 60);
        CHECK(end.x > 3.0f);
    }
    SECTION("a plate held up out of the way - an open door - is walked under") {
        const auto raised = [] {
            auto t = Plate{0.0f}.tris();
            for (auto& tri : t) for (auto& v : tri) v.z += 2.5f;
            return t;
        }();
        const glm::vec3 end = walk(raised, {-3.0f, 0.0f, 0.0f}, {0.117f, 0.0f, 0.0f}, 60);
        CHECK(end.x > 3.0f);
    }
    SECTION("a character standing clear is not touched") {
        glm::vec3 to(-0.6f, 0.0f, 0.0f);
        const auto t = tris.front();
        CHECK_FALSE(holdCylinderOffWallTriangle({-0.7f, 0.0f, 0.0f}, to, 0.5f, 2.0f,
                                                t[0], t[1], t[2]));
        CHECK(to.x == -0.6f);
    }
}

TEST_CASE("a face holds back only what is in front of it", "[collision]") {
    // The client tests a face only when the step runs into its front. A face
    // toward +x, walked into from behind, lets the character through.
    const glm::vec3 a(0, -2, 0), b(0, 2, 0), c(0, 2, 4);   // `front` says which way it faces
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float front = n.x > 0.0f ? 1.0f : -1.0f;

    glm::vec3 to(front * 0.2f, 0.0f, 0.0f);
    CHECK(holdCylinderOffWallTriangle({front * 1.0f, 0.0f, 0.0f}, to, 0.5f, 2.0f, a, b, c));
    CHECK(to.x * front > 0.5f);

    glm::vec3 behind(-front * 0.2f, 0.0f, 0.0f);
    CHECK_FALSE(holdCylinderOffWallTriangle({-front * 1.0f, 0.0f, 0.0f}, behind, 0.5f, 2.0f,
                                            a, b, c));
    CHECK(behind.x == -front * 0.2f);
}

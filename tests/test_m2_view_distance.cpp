// How far an M2 instance is drawn: the client's doodad size classes
// (0x007bdb10, 0x0078f570, 0x00791cb0), the ground cover's own radius, and
// the view distance over all of them.
#include <catch_amalgamated.hpp>

#include <cmath>

#include "rendering/m2_view_distance.hpp"

using namespace wowee::rendering;

namespace {
float distanceOf(float distSq) { return std::sqrt(distSq); }
}

TEST_CASE("a doodad's size class is the first limit its largest side fits", "[m2][viewdist]") {
    CHECK(m2DoodadSizeClass(0.5f) == 0);
    CHECK(m2DoodadSizeClass(1.0f) == 0);   // at the limit stays in it
    CHECK(m2DoodadSizeClass(1.01f) == 1);
    CHECK(m2DoodadSizeClass(4.0f) == 1);
    CHECK(m2DoodadSizeClass(10.0f) == 2);
    CHECK(m2DoodadSizeClass(60.0f) == 3);
    CHECK(m2DoodadSizeClass(100.5f) == 4);
    CHECK(m2DoodadSizeClass(5000.0f) == 4);
}

TEST_CASE("each class has the client's distance, the middle three scaled by detail",
          "[m2][viewdist]") {
    CHECK(m2DoodadCullDistance(0, 1.0f) == Catch::Approx(30.0f));
    CHECK(m2DoodadCullDistance(1, 1.0f) == Catch::Approx(100.0f));
    CHECK(m2DoodadCullDistance(2, 1.0f) == Catch::Approx(200.0f));
    CHECK(m2DoodadCullDistance(3, 1.0f) == Catch::Approx(750.0f));
    CHECK(m2DoodadCullDistance(4, 1.0f) == Catch::Approx(1250.0f));
    // Environment Detail 0.5..1.5 (0x0078dc60) scales classes 1-3 only.
    CHECK(m2DoodadCullDistance(0, 1.5f) == Catch::Approx(30.0f));
    CHECK(m2DoodadCullDistance(3, 1.5f) == Catch::Approx(1125.0f));
    CHECK(m2DoodadCullDistance(4, 0.5f) == Catch::Approx(1250.0f));
    CHECK(m2DoodadCullDistance(2, 0.1f) == Catch::Approx(100.0f));  // clamped to 0.5
}

TEST_CASE("a doodad fades over its class's band and stops below 0.01", "[m2][viewdist]") {
    // Class 2: drawn to 200, fading over the last 15.
    CHECK(m2DoodadFade(100.0f, 2, 1.0f) == 1.0f);
    CHECK(m2DoodadFade(185.0f, 2, 1.0f) == 1.0f);
    CHECK(m2DoodadFade(192.5f, 2, 1.0f) == Catch::Approx(0.5f));
    CHECK(m2DoodadFade(199.95f, 2, 1.0f) == 0.0f);
    CHECK(m2DoodadFade(250.0f, 2, 1.0f) == 0.0f);
    // Above 0.99 is whole.
    CHECK(m2DoodadFade(185.1f, 2, 1.0f) == 1.0f);
}

TEST_CASE("a doodad stops at its class's distance, never past the view", "[m2][viewdist]") {
    CHECK(distanceOf(m2InstanceMaxDistSq(3, 1.0f, false, 2400.0f)) == Catch::Approx(750.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(4, 1.0f, false, 1000.0f)) == Catch::Approx(1000.0f));
}

TEST_CASE("a game object is not a doodad: drawn as far as the world is", "[m2][viewdist]") {
    CHECK(distanceOf(m2InstanceMaxDistSq(0, 1.0f, true, 1200.0f)) == Catch::Approx(1200.0f));
}

TEST_CASE("ground cover stops at its own radius, not its class's", "[m2][viewdist]") {
    CHECK(distanceOf(m2InstanceMaxDistSq(0, 1.0f, false, 2400.0f, true, 70.0f))
          == Catch::Approx(70.0f));
    // A radius past the view is held by the view.
    CHECK(distanceOf(m2InstanceMaxDistSq(0, 1.0f, false, 400.0f, true, 500.0f))
          == Catch::Approx(400.0f));
    // With none set it is a doodad like the rest.
    CHECK(distanceOf(m2InstanceMaxDistSq(1, 1.0f, false, 2400.0f, true, 0.0f))
          == Catch::Approx(100.0f));
}

TEST_CASE("the size class is taken from the header's vertex box in the world", "[m2][viewdist]") {
    // 0x007bdb10: the +0xa0 box carried by the placement, its largest side.
    const glm::vec3 lo(-0.5f, -0.5f, 0.0f), hi(0.5f, 0.5f, 3.0f);  // 3 yards tall
    CHECK(m2DoodadSizeClassOfBox(glm::mat4(1.0f), lo, hi) == 1);
    // Scaled up twice, it is 6 yards tall: the next class.
    glm::mat4 twice(1.0f);
    twice[0][0] = twice[1][1] = twice[2][2] = 2.0f;
    CHECK(m2DoodadSizeClassOfBox(twice, lo, hi) == 2);
    // Turned on its side, a 3-yard pole is still 3 yards long.
    glm::mat4 onSide(0.0f);
    onSide[0][2] = 1.0f; onSide[1][1] = 1.0f; onSide[2][0] = -1.0f; onSide[3][3] = 1.0f;
    CHECK(m2DoodadSizeClassOfBox(onSide, lo, hi) == 1);
    // A box inverted on every axis is none: the doodad's position, class 0.
    CHECK(m2DoodadSizeClassOfBox(twice, glm::vec3(1.0f), glm::vec3(-1.0f)) == 0);
}

TEST_CASE("a chunk draws the classes its depth is short of", "[m2][viewdist]") {
    // 0x0078fb60 against the distances 0x0078f570 squares.
    CHECK(m2ChunkMinSizeClass(-5.0f, 1.0f) == 0);   // behind the camera
    CHECK(m2ChunkMinSizeClass(29.0f, 1.0f) == 0);
    CHECK(m2ChunkMinSizeClass(30.0f, 1.0f) == 1);   // at 30 the smallest are gone
    CHECK(m2ChunkMinSizeClass(150.0f, 1.0f) == 2);
    CHECK(m2ChunkMinSizeClass(500.0f, 1.0f) == 3);
    CHECK(m2ChunkMinSizeClass(800.0f, 1.0f) == 4);
    CHECK(m2ChunkMinSizeClass(5000.0f, 1.0f) == 4); // the largest are never dropped here
    // Environment Detail scales the middle three.
    CHECK(m2ChunkMinSizeClass(140.0f, 1.5f) == 1);
}

TEST_CASE("a chunk's depth is to its corner nearest along the view", "[m2][viewdist]") {
    // 0x00790650 picks the corner, 0x007c3e70 the plane through the camera.
    const glm::vec3 lo(10.0f, -5.0f, 0.0f), hi(20.0f, 5.0f, 2.0f);
    CHECK(m2ChunkViewDepth(lo, hi, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == Catch::Approx(10.0f));
    CHECK(m2ChunkViewDepth(lo, hi, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f)) == Catch::Approx(-20.0f));
    // Widened to the 33.3-yard chunk grid.
    glm::vec3 bmin, bmax;
    m2ChunkBoxAround(glm::vec3(40.0f, -1.0f, 3.0f), glm::vec3(41.0f, 1.0f, 9.0f), bmin, bmax);
    CHECK(bmin.x == Catch::Approx(33.33333f));
    CHECK(bmax.x == Catch::Approx(66.66666f));
    CHECK(bmin.y == Catch::Approx(-33.33333f));
    CHECK(bmax.y == Catch::Approx(33.33333f));
    CHECK(bmin.z == 3.0f);
    CHECK(bmax.z == 9.0f);
}

TEST_CASE("a doodad with MDDF flag 0x1 is not held to its class distance", "[m2][viewdist]") {
    // 0x007becd0 gives it flag 0x800, which 0x00791cb0 passes over.
    CHECK(distanceOf(m2InstanceMaxDistSq(0, 1.0f, false, 1000.0f, false, 0.0f, true)) == Catch::Approx(1000.0f));
    CHECK(distanceOf(m2InstanceMaxDistSq(0, 1.0f, false, 1000.0f, false, 0.0f, false)) == Catch::Approx(30.0f));
}

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

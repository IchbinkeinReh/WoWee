// How far the ray traced point lights are shown (U18): a share of the view
// distance, never beyond the 120 yards they were always gathered to, with a
// fade over the last stretch below that.
#include <catch_amalgamated.hpp>

#include "rendering/rt_light_reach.hpp"

using namespace wowee::rendering;

TEST_CASE("the distance is the share of the view, held to the old 120 yards", "[rt_light_reach]") {
    CHECK(rtLightReach(100, 1277.0f) == Catch::Approx(120.0f));
    CHECK(rtLightReach(100, 177.0f) == Catch::Approx(120.0f));
    CHECK(rtLightReach(kRtLightReachDefaultPercent, 791.0f) == Catch::Approx(kRtLightReachCap));
    CHECK(rtLightReach(10, 791.0f) == Catch::Approx(79.1f));
    CHECK(rtLightReach(1, 1000.0f) == Catch::Approx(10.0f));
    CHECK(rtLightReach(0, 1000.0f) == Catch::Approx(10.0f));    // held to 1 %
    CHECK(rtLightReach(250, 1000.0f) == Catch::Approx(120.0f)); // held to 100 %
}

TEST_CASE("at the cap every light is shown as before", "[rt_light_reach]") {
    const float reach = rtLightReach(100, 1277.0f);
    CHECK(rtLightReachShare(0.0f, reach) == 1.0f);
    CHECK(rtLightReachShare(119.0f, reach) == 1.0f);
    // The old gathering keeps a light whose reach comes within 120 yards.
    CHECK(rtLightReachShare(130.0f, reach) == 1.0f);
}

TEST_CASE("below the cap a light fades out to none at the distance", "[rt_light_reach]") {
    const float reach = rtLightReach(10, 800.0f);  // 80 yards
    REQUIRE(reach == Catch::Approx(80.0f));
    CHECK(rtLightReachShare(10.0f, reach) == 1.0f);
    CHECK(rtLightReachShare(68.0f, reach) == 1.0f);           // 0.85 x 80: the fade starts
    const float mid = rtLightReachShare(74.0f, reach);          // half way through the fade
    CHECK(mid == Catch::Approx(0.5f));
    CHECK(rtLightReachShare(72.0f, reach) > mid);
    CHECK(rtLightReachShare(76.0f, reach) < mid);
    CHECK(rtLightReachShare(80.0f, reach) == 0.0f);
    CHECK(rtLightReachShare(95.0f, reach) == 0.0f);
}

#include <catch_amalgamated.hpp>

#include "rendering/rt_min_brightness.hpp"

using namespace wowee::rendering;

TEST_CASE("Minimum brightness maps its percent to a floor of 0 to 0.5", "[rt][settings]") {
    CHECK(rtMinBrightnessFactor(0) == 0.0f);
    CHECK(rtMinBrightnessFactor(15) == Catch::Approx(0.15f));
    CHECK(rtMinBrightnessFactor(50) == Catch::Approx(0.5f));
    // Held to the slider's range.
    CHECK(rtMinBrightnessFactor(-10) == 0.0f);
    CHECK(rtMinBrightnessFactor(80) == Catch::Approx(0.5f));
}

TEST_CASE("Minimum brightness lifts only what the trace takes near black", "[rt][settings]") {
    const float floor = rtMinBrightnessFactor(20);
    // A lit surface, and one the trace brightens: as they were.
    CHECK(rtFlooredScale(1.0f, floor) == 1.0f);
    CHECK(rtFlooredScale(1.4f, floor) == 1.4f);
    CHECK(rtFlooredScale(0.6f, floor) == 0.6f);
    // A corner the occlusion, the sun's shade and the baked shade together
    // take to nothing: held at the floor.
    CHECK(rtFlooredScale(0.0f, floor) == Catch::Approx(0.2f));
    CHECK(rtFlooredScale(-0.1f, floor) == Catch::Approx(0.2f));
    // 0: the tracing as it always was.
    CHECK(rtFlooredScale(0.05f, rtMinBrightnessFactor(0)) == 0.05f);
}

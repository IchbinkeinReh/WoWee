// The eye's adaptation under the ray traced lighting (eye_adaptation.hpp).
// Daylight must come out at an exposure of exactly one - the traced world
// in the open is meant to look as it does without it - a room only part of
// the way brighter and never past the limits, and the eye quicker into the
// light than out of it.
#include <catch_amalgamated.hpp>

#include "rendering/eye_adaptation.hpp"

#include <cmath>
#include <vector>

using namespace wowee::rendering::eye_adaptation;

TEST_CASE("the neutral band is left at an exposure of one", "[eye]") {
    const Params p;
    CHECK(exposureFor(p.neutralLow) == 1.0f);
    CHECK(exposureFor(p.neutralHigh) == 1.0f);
    CHECK(exposureFor(0.5f * (p.neutralLow + p.neutralHigh)) == 1.0f);
}

TEST_CASE("darker than the band brightens, brighter darkens, part of the way and within limits",
          "[eye]") {
    const Params p;
    // Half a stop under: a quarter stop up; half over: a quarter down.
    CHECK(exposureFor(p.neutralLow - 0.5f) == Catch::Approx(std::exp2(0.25f)));
    CHECK(exposureFor(p.neutralHigh + 0.5f) == Catch::Approx(std::exp2(-0.25f)));
    // Far out either way: no further than the limits.
    CHECK(exposureFor(p.neutralLow - 20.0f) == Catch::Approx(p.maxExposure));
    CHECK(exposureFor(p.neutralHigh + 20.0f) == Catch::Approx(p.minExposure));
    // Monotonic: a darker scene never gets less exposure.
    float last = exposureFor(p.neutralHigh + 5.0f);
    for (float l = p.neutralHigh + 5.0f; l >= p.neutralLow - 5.0f; l -= 0.25f) {
        const float e = exposureFor(l);
        CHECK(e >= last);
        last = e;
    }
}

TEST_CASE("the eye follows into the light faster than into the dark", "[eye]") {
    const Params p;
    // A second after a two-stop change either way.
    float up = -4.0f, down = -2.0f;
    for (int i = 0; i < 60; ++i) {
        up = adapt(up, -2.0f, 1.0f / 60.0f);
        down = adapt(down, -4.0f, 1.0f / 60.0f);
    }
    const float upShare = (up - -4.0f) / 2.0f;
    const float downShare = (-2.0f - down) / 2.0f;
    CHECK(upShare > downShare);
    // Into the light: most of the way within a second. Into the dark: well
    // under half, but on its way.
    CHECK(upShare > 0.8f);
    CHECK(downShare > 0.2f);
    CHECK(downShare < 0.5f);
    // It never overshoots, however long the step.
    CHECK(adapt(-4.0f, -2.0f, 100.0f) <= -2.0f);
    CHECK(adapt(-4.0f, -2.0f, 100.0f) == Catch::Approx(-2.0f));
    // No time, no change.
    CHECK(adapt(-4.0f, -2.0f, 0.0f) == -4.0f);
    // Frame-rate independent: two half steps are one whole one.
    const float whole = adapt(-4.0f, -2.0f, 0.2f);
    const float halves = adapt(adapt(-4.0f, -2.0f, 0.1f), -2.0f, 0.1f);
    CHECK(halves == Catch::Approx(whole));
}

TEST_CASE("an eye that starts looking starts at an exposure of one", "[eye]") {
    const Params p;
    for (float measured : {-12.0f, -5.0f, -3.0f, -1.0f, 0.0f}) {
        CHECK(exposureFor(initialAdaptation(measured)) == 1.0f);
    }
}

TEST_CASE("the picture's luminance is the mean of its logs, in either byte order", "[eye]") {
    // Uniform mid grey: one value whatever the order.
    std::vector<uint8_t> grey(16 * 8 * 4, 128);
    const float g = measureLog2(grey.data(), 16, 8, false);
    CHECK(g == Catch::Approx(std::log2(std::pow(128.0f / 255.0f, 2.2f))).margin(1e-4));
    CHECK(measureLog2(grey.data(), 16, 8, true) == Catch::Approx(g));

    // Pure red read as RGBA, and the same bytes read as BGRA are pure blue:
    // red weighs three times what blue does.
    std::vector<uint8_t> red(4 * 4, 0);
    for (size_t i = 0; i < red.size(); i += 4) red[i] = 255;
    CHECK(measureLog2(red.data(), 2, 2, false) == Catch::Approx(std::log2(0.2126f)));
    CHECK(measureLog2(red.data(), 2, 2, true) == Catch::Approx(std::log2(0.0722f)));

    // Half white, half black: the geometric mean, which a single bright
    // patch cannot drag up the way an arithmetic one would.
    std::vector<uint8_t> half(4 * 4, 0);
    for (size_t i = 0; i < 8; ++i) half[i] = 255;
    const float h = measureLog2(half.data(), 2, 2, false);
    CHECK(h == Catch::Approx(0.5f * (0.0f + std::log2(1.0f / 1024.0f))).margin(1e-3));

    CHECK(measureLog2(nullptr, 2, 2, false) == 0.0f);
}

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

TEST_CASE("the settings' defaults are the eye as it has always been", "[eye]") {
    const Params p = paramsFor(kDefaultBrighteningPercent, kDefaultSeconds);
    const Params d;
    CHECK(p.maxExposure == d.maxExposure);
    CHECK(p.minExposure == d.minExposure);
    CHECK(p.toBrighterSeconds == d.toBrighterSeconds);
    CHECK(p.toDarkerSeconds == d.toDarkerSeconds);
    CHECK(d.maxExposure == Catch::Approx(1.3f));
    CHECK(d.toDarkerSeconds == Catch::Approx(2.5f));
}

TEST_CASE("brightening 0 leaves every view at an exposure of one", "[eye]") {
    const Params p = paramsFor(0, kDefaultSeconds);
    CHECK(p.maxExposure == 1.0f);
    CHECK(p.minExposure == 1.0f);
    for (float adapted : {-14.0f, -8.0f, -5.0f, -2.0f, 0.0f}) {
        CHECK(exposureFor(adapted, p) == 1.0f);
    }
}

TEST_CASE("brightening is the most a dark view is lifted, darkening going with it", "[eye]") {
    CHECK(paramsFor(50, kDefaultSeconds).maxExposure == Catch::Approx(1.5f));
    const Params most = paramsFor(100, kDefaultSeconds);
    CHECK(most.maxExposure == Catch::Approx(2.0f));
    CHECK(most.minExposure == 0.5f);  // held at half
    CHECK(exposureFor(-20.0f, most) == Catch::Approx(2.0f));
    // Out of range is held to it.
    CHECK(paramsFor(250, kDefaultSeconds).maxExposure == Catch::Approx(2.0f));
    CHECK(paramsFor(-5, kDefaultSeconds).maxExposure == 1.0f);
    // More brightening never darkens less.
    float lastMin = 1.0f;
    for (int percent = 0; percent <= 100; percent += 5) {
        const Params p = paramsFor(percent, kDefaultSeconds);
        CHECK(p.maxExposure >= 1.0f);
        CHECK(p.minExposure <= lastMin);
        lastMin = p.minExposure;
    }
}

TEST_CASE("the time scales both ways, the light five times quicker", "[eye]") {
    const Params slow = paramsFor(kDefaultBrighteningPercent, 10.0f);
    CHECK(slow.toDarkerSeconds == Catch::Approx(10.0f));
    CHECK(slow.toBrighterSeconds == Catch::Approx(2.0f));
    CHECK(paramsFor(30, 100.0f).toDarkerSeconds == Catch::Approx(kMaxSeconds));
    CHECK(paramsFor(30, 0.0f).toDarkerSeconds == Catch::Approx(kMinSeconds));
    CHECK(paramsFor(30, std::nanf("")).toDarkerSeconds == Catch::Approx(kDefaultSeconds));
    // A long time is a slow fade: a second into a dark room the eye has
    // come a quarter as far as it does at the default.
    const float fast = adapt(-4.0f, -9.0f, 1.0f, paramsFor(30, kDefaultSeconds));
    const float fade = adapt(-4.0f, -9.0f, 1.0f, slow);
    CHECK(-4.0f - fade < 0.3f * (-4.0f - fast));
}

TEST_CASE("sensitivity 0 is the band as it has always been", "[eye]") {
    const Params d;
    const Params p = paramsFor(kDefaultBrighteningPercent, kDefaultSeconds, 0);
    CHECK(p.neutralLow == d.neutralLow);
    CHECK(p.neutralHigh == d.neutralHigh);
    CHECK(d.neutralLow == Catch::Approx(-6.3f));
    CHECK(d.neutralHigh == Catch::Approx(-3.6f));
    // The two-argument form is the same.
    CHECK(paramsFor(kDefaultBrighteningPercent, kDefaultSeconds).neutralLow == d.neutralLow);
}

TEST_CASE("sensitivity narrows the band about its middle", "[eye]") {
    const Params d;
    const float middle = 0.5f * (d.neutralLow + d.neutralHigh);
    const Params half = paramsFor(kDefaultBrighteningPercent, kDefaultSeconds, 50);
    CHECK(half.neutralHigh - half.neutralLow == Catch::Approx(0.5f * (d.neutralHigh - d.neutralLow)));
    CHECK(0.5f * (half.neutralLow + half.neutralHigh) == Catch::Approx(middle));
    const Params none = paramsFor(kDefaultBrighteningPercent, kDefaultSeconds, 100);
    CHECK(none.neutralLow == Catch::Approx(middle));
    CHECK(none.neutralHigh == Catch::Approx(middle));
    // Out of range is held to it.
    CHECK(paramsFor(30, 2.5f, 250).neutralLow == Catch::Approx(middle));
    CHECK(paramsFor(30, 2.5f, -10).neutralLow == d.neutralLow);
    // It touches nothing else.
    CHECK(half.maxExposure == d.maxExposure);
    CHECK(half.toDarkerSeconds == d.toDarkerSeconds);
}

TEST_CASE("a higher sensitivity adjusts a view the default leaves alone", "[eye]") {
    // A shaded street, -5.9: inside the default band, outside a narrowed one.
    const float street = -5.9f;
    CHECK(exposureFor(street, paramsFor(30, 2.5f, 0)) == 1.0f);
    CHECK(exposureFor(street, paramsFor(30, 2.5f, 60)) > 1.0f);
    // A bright plain, -4.0, the other way.
    CHECK(exposureFor(-4.0f, paramsFor(30, 2.5f, 0)) == 1.0f);
    CHECK(exposureFor(-4.0f, paramsFor(30, 2.5f, 60)) < 1.0f);
    // At the middle nothing moves, however sensitive.
    const Params d;
    CHECK(exposureFor(0.5f * (d.neutralLow + d.neutralHigh), paramsFor(30, 2.5f, 100)) == 1.0f);
}

namespace {
// A sunset-like test frame, 320 x 180: a sky gradient, a burnt-out white disc
// (the clipped glare round the sun) centred at (160, 60), radius 25, a pale
// yellow cloud at (60, 50), a sunlit yellow field below y 120.
struct Frame {
    static constexpr uint32_t W = 320, H = 180;
    std::vector<float> px = std::vector<float>(size_t(W) * H * 3);
    float* at(uint32_t x, uint32_t y) { return px.data() + (size_t(y) * W + x) * 3; }
    const float* at(uint32_t x, uint32_t y) const { return px.data() + (size_t(y) * W + x) * 3; }
};

Frame sunset(bool withSun) {
    Frame f;
    for (uint32_t y = 0; y < Frame::H; ++y) {
        for (uint32_t x = 0; x < Frame::W; ++x) {
            float* p = f.at(x, y);
            if (y >= 120) { p[0] = 1.0f; p[1] = 0.9f; p[2] = 0.25f; continue; }   // field
            const float t = float(y) / 120.0f;
            p[0] = 0.45f + 0.4f * t; p[1] = 0.6f + 0.25f * t; p[2] = 0.85f;        // sky
            const float dc = std::hypot(float(x) - 60.0f, float(y) - 50.0f);
            if (dc < 15.0f) { p[0] = 1.0f; p[1] = 0.88f; p[2] = 0.72f; }           // pale cloud
            if (withSun) {
                const float d = std::hypot(float(x) - 160.0f, float(y) - 60.0f);
                if (d < 25.0f) { p[0] = p[1] = p[2] = 1.0f; }                     // clipped
                else if (d < 60.0f) {                                              // glow, not white
                    const float g = 0.995f * (1.0f - (d - 25.0f) / 35.0f);
                    for (int c = 0; c < 3; ++c) p[c] = std::max(p[c], g);
                }
            }
        }
    }
    return f;
}
}  // namespace

TEST_CASE("away from clipped white the exposure is exactly the multiply", "[eye]") {
    // A frame with nothing clipped white: a noon field, a blue sky, a pale
    // cloud - every pixel times the exposure, as it always was.
    Frame f = sunset(false);
    const Frame orig = f;
    exposeFrame(f.px.data(), Frame::W, Frame::H, 0.6f);
    for (size_t i = 0; i < f.px.size(); ++i) CHECK(f.px[i] == Catch::Approx(orig.px[i] * 0.6f).margin(1e-6));
    // At an exposure of one, or above it, nothing but the multiply anywhere.
    Frame g = sunset(true);
    const Frame gOrig = g;
    exposeFrame(g.px.data(), Frame::W, Frame::H, 1.0f);
    for (size_t i = 0; i < g.px.size(); ++i) CHECK(g.px[i] == Catch::Approx(gOrig.px[i]).margin(1e-6));
    Frame h = sunset(true);
    exposeFrame(h.px.data(), Frame::W, Frame::H, 1.5f);
    CHECK(h.at(10, 10)[0] == Catch::Approx(std::min(1.0f, gOrig.px[(10 * Frame::W + 10) * 3] * 1.5f)));
    CHECK(h.at(160, 60)[0] == 1.0f);
}

TEST_CASE("clipped white stays white and the glare round it falls off smoothly", "[eye]") {
    const float e = 0.6f;
    Frame f = sunset(true);
    const Frame orig = f;
    exposeFrame(f.px.data(), Frame::W, Frame::H, e);
    // The burnt-out core: white, not 0.6 grey.
    CHECK(f.at(160, 60)[0] > 0.97f);
    CHECK(f.at(160, 60)[2] > 0.97f);
    // Along a row out from the sun: never brighter further out (the image
    // only gets darker there), and no step between neighbours bigger than a
    // gentle ramp's - no edge.
    float last = f.at(160, 60)[1];
    for (uint32_t x = 160; x < 300; ++x) {
        const float v = f.at(x, 60)[1];
        CHECK(v <= last + 1e-4f);
        CHECK(last - v < 0.06f);
        last = std::min(last, v);
    }
    // Far from it the multiply again.
    CHECK(f.at(310, 10)[1] == Catch::Approx(orig.at(310, 10)[1] * e).margin(0.01));
    // The field below keeps the multiply - bright in two channels, not white.
    CHECK(f.at(20, 170)[0] == Catch::Approx(e).margin(0.01));
}

TEST_CASE("a pale coloured cloud keeps its hue", "[eye]") {
    const float e = 0.6f;
    Frame f = sunset(true);
    const Frame orig = f;
    exposeFrame(f.px.data(), Frame::W, Frame::H, e);
    // Its own pixel ratios are kept: only ever scaled.
    const float* c = f.at(60, 50);
    const float* o = orig.at(60, 50);
    CHECK(c[1] / c[0] == Catch::Approx(o[1] / o[0]).epsilon(1e-4));
    CHECK(c[2] / c[0] == Catch::Approx(o[2] / o[0]).epsilon(1e-4));
    // And far from the sun it is not lifted at all.
    CHECK(c[0] == Catch::Approx(o[0] * e).margin(0.01));
    // Only white in all three channels counts as clipped.
    CHECK(clipShare(1.0f, 1.0f, 1.0f) == 1.0f);
    CHECK(clipShare(1.0f, 0.88f, 0.72f) == 0.0f);
    CHECK(clipShare(1.0f, 1.0f, 0.25f) == 0.0f);
    CHECK(glareScale(e, 0.0f) == e);
    CHECK(glareScale(e, 1.0f) == 1.0f);
    CHECK(glareScale(1.4f, 1.0f) == 1.4f);
}

TEST_CASE("a small clipped disc on its own is brought up with a soft halo", "[eye]") {
    // The sun's disc low over a ridge: a few pixels clipped white in a
    // plain sky, too small for the wide glare.
    const float e = 0.65f;
    Frame f;
    for (uint32_t y = 0; y < Frame::H; ++y) {
        for (uint32_t x = 0; x < Frame::W; ++x) {
            float* p = f.at(x, y);
            p[0] = 0.8f; p[1] = 0.65f; p[2] = 0.5f;
            if (std::hypot(float(x) - 150.0f, float(y) - 90.0f) < 2.5f) p[0] = p[1] = p[2] = 1.0f;
        }
    }
    const Frame orig = f;
    exposeFrame(f.px.data(), Frame::W, Frame::H, e);
    CHECK(f.at(150, 90)[1] > e + 0.15f);
    // Far away, the multiply; in between, never a step.
    CHECK(f.at(20, 20)[1] == Catch::Approx(orig.at(20, 20)[1] * e).margin(1e-4));
    for (uint32_t x = 154; x < 300; ++x) CHECK(f.at(x - 1, 90)[1] - f.at(x, 90)[1] < 0.05f);
}

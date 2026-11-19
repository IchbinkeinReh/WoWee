// The tessellated water's curve and the interaction's heights
// (rendering/water_tessellation.hpp, the twin of water_tess.glsli and
// water_interaction.glsli).
#include <catch_amalgamated.hpp>

#include "rendering/swim_wave.hpp"
#include "rendering/water_tessellation.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace wt = wowee::rendering::water_tess;
namespace swim_wave = wowee::rendering::swim_wave;

namespace {

// How far a height's slope along the radius is from the slope the fragment
// stage shades with, over a sweep: the root mean square of the difference
// over that of the slope.
template <typename Height, typename Slope>
double slopeMismatch(Height height, Slope slope, double from, double to, int steps) {
    double diff = 0.0, ref = 0.0;
    const double h = 1e-3;
    for (int i = 0; i <= steps; ++i) {
        const double x = from + (to - from) * i / steps;
        const double numeric = (height(x + h) - height(x - h)) / (2.0 * h);
        const double s = slope(x);
        diff += (numeric - s) * (numeric - s);
        ref += s * s;
    }
    return std::sqrt(diff / ref);
}

}  // namespace

TEST_CASE("the grid is cut finest near the camera and whole far off", "[water_tess]") {
    // A side of the grid's cell near the camera: pieces of 0.3 yards.
    CHECK(wt::level(wt::kGridEdge, 5.0f) == Catch::Approx(wt::kGridEdge / wt::kTargetEdge));
    CHECK(wt::level(wt::kGridEdge, wt::kNear) == Catch::Approx(wt::kGridEdge / wt::kTargetEdge));
    // A diagonal would want more pieces than the cap.
    CHECK(wt::level(wt::kGridEdge * std::sqrt(2.0f), 0.0f) == Catch::Approx(wt::kMaxLevel));
    // From kFar out, whole, as the untessellated water.
    CHECK(wt::level(wt::kGridEdge, wt::kFar) == Catch::Approx(1.0f));
    CHECK(wt::level(wt::kGridEdge, 500.0f) == Catch::Approx(1.0f));
    // Never fewer pieces nearer the camera, never under one or over the cap.
    float last = wt::level(wt::kGridEdge * 2.0f, 0.0f);
    for (float d = 0.0f; d <= 120.0f; d += 0.5f) {
        const float l = wt::level(wt::kGridEdge * 2.0f, d);
        CHECK(l <= last + 1e-5f);
        CHECK(l >= 1.0f);
        CHECK(l <= wt::kMaxLevel);
        last = l;
    }
}

TEST_CASE("the shortest moved wave follows the cut grid's pieces", "[water_tess]") {
    // Where the grid is whole: water.vert's 9 yards.
    CHECK(wt::minWavelength(wt::kFar) == Catch::Approx(wt::kGridWavelength));
    CHECK(wt::minWavelength(300.0f) == Catch::Approx(wt::kGridWavelength));
    // Near: about twice a piece, never under the floor.
    const float piece = wt::kGridEdge / wt::level(wt::kGridEdge, 0.0f);
    CHECK(wt::minWavelength(0.0f) == Catch::Approx(2.0f * piece).epsilon(0.15));
    for (float d = 0.0f; d <= 120.0f; d += 1.0f) CHECK(wt::minWavelength(d) >= wt::kMinWavelength);
}

TEST_CASE("the interaction's height is raised only where the grid is cut", "[water_tess]") {
    CHECK(wt::interaction(0.0f) == Catch::Approx(1.0f));
    CHECK(wt::interaction(wt::kNear) == Catch::Approx(1.0f));
    CHECK(wt::interaction(wt::kFar) == Catch::Approx(0.0f));
    float last = 1.0f;
    for (float d = 0.0f; d <= 120.0f; d += 0.5f) {
        CHECK(wt::interaction(d) <= last + 1e-5f);
        last = wt::interaction(d);
    }
}

TEST_CASE("an impulse's rings stand at the height whose slope the shading uses", "[water_tess]") {
    // A hand's stroke (0.25) and a trail's point (0.45), early and late in
    // the rings' life.
    for (float sigma : {0.25f, 0.45f}) {
        for (float t : {0.3f, 0.5f, 1.0f, 1.5f, 2.0f}) {
            INFO("sigma " << sigma << " t " << t);
            const double mismatch = slopeMismatch(
                [&](double r) { return wt::impulseRingHeight(static_cast<float>(r), t, sigma, 1.0f); },
                [&](double r) { return wt::impulseRingSlope(static_cast<float>(r), t, sigma, 1.0f); }, 0.25, 9.0,
                600);
            CHECK(mismatch < 0.12);
        }
    }
    // Still water ahead of the rings: no hump where nothing has arrived.
    CHECK(std::abs(wt::impulseRingHeight(40.0f, 0.5f, 0.45f, 1.0f)) < 1e-3f);
    // Before the impulse, and once its rings have faded, nothing.
    CHECK(wt::impulseRingHeight(1.0f, 0.0f, 0.45f, 1.0f) == 0.0f);
    CHECK(wt::impulseRingHeight(1.0f, swim_wave::kLife, 0.25f, wt::swimRingFade(swim_wave::kLife)) ==
          Catch::Approx(0.0f).margin(1e-6));
    CHECK(wt::impulseRingHeight(1.0f, wt::kWakeRingLife, 0.45f, wt::wakeRingFade(wt::kWakeRingLife)) ==
          Catch::Approx(0.0f).margin(1e-6));
}

namespace {
// A run through the shallows with the physics waves (WaterRenderer::emitWake,
// water.tese's interactionHeight): a point every 0.6 yards (the wader's 0.4,
// half again for the physics waves), a quarter yard ahead of the feet, at a
// wader's full strength, for `runFor` seconds at 7 yards a second; then the
// most the water stands off its rest anywhere within 3 yards of the path,
// `at` seconds after the start.
float wakeHeave(float runFor, float at, float strengthScale, float life, float (*fade)(float), float spacing) {
    std::vector<std::pair<float, float>> points;  // x, laid at
    float last = -1e9f;
    for (float t = 0.0f; t < runFor; t += 1.0f / 60.0f) {
        const float x = 7.0f * t;
        if (x - last >= spacing) {
            points.emplace_back(x + 0.25f, t);
            last = x;
        }
    }
    float most = 0.0f;
    for (float gx = -3.0f; gx <= 3.0f + 7.0f * runFor; gx += 0.25f) {
        for (float gy = 0.0f; gy <= 3.0f; gy += 0.25f) {
            float h = 0.0f;
            for (const auto& [px, laid] : points) {
                const float age = at - laid;
                if (age <= 0.0f || age >= life) continue;
                h += wt::impulseRingHeight(std::hypot(gx - px, gy), age, 0.45f, strengthScale * fade(age));
            }
            most = std::max(most, std::abs(h));
        }
    }
    return most;
}
float oldFade(float t) { return 1.0f - wt::smoothstep(1.2f, 2.2f, t); }
}  // namespace

TEST_CASE("a run through the shallows heaves the water a little, and it settles over seconds",
          "[water_tess][wake]") {
    // The old wake, for scale: half the point's strength, gone with the
    // point at 1.5 s - three quarters of a yard of heave at 1.5 s, nothing
    // at 2.5.
    const float oldPeak = wakeHeave(0.75f, 1.5f, 0.5f, 1.5f, oldFade, 0.4f);
    CHECK(oldPeak > 0.5f);
    CHECK(wakeHeave(0.75f, 2.5f, 0.5f, 1.5f, oldFade, 0.4f) == 0.0f);
    // Now: a tenth of a yard or so at most, and the rings still there,
    // smaller, seconds after the runner stopped.
    float peak = 0.0f;
    for (float at : {0.5f, 1.0f, 1.5f, 2.0f}) {
        peak = std::max(peak, wakeHeave(0.75f, at, wt::kWakeRingStrength, wt::kWakeRingLife, wt::wakeRingFade, 0.6f));
    }
    CHECK(peak < 0.15f);
    CHECK(peak < oldPeak / 5.0f);
    const float later = wakeHeave(0.75f, 3.0f, wt::kWakeRingStrength, wt::kWakeRingLife, wt::wakeRingFade, 0.6f);
    CHECK(later > 0.01f);
    CHECK(later < peak);
    CHECK(wakeHeave(0.75f, wt::kWakeRingLife + 0.8f, wt::kWakeRingStrength, wt::kWakeRingLife, wt::wakeRingFade,
                    0.6f) == 0.0f);
    // The fade never rises.
    float lastFade = 2.0f;
    for (float t = 0.0f; t <= wt::kWakeRingLife; t += 0.05f) {
        CHECK(wt::wakeRingFade(t) <= lastFade);
        lastFade = wt::wakeRingFade(t);
    }
}

namespace {
// A wader's run through the shallows and then standing still, with the
// physics waves (WaterRenderer::emitWake, the Renderer's swim_wave limbs,
// water.tese's interactionHeight): for `runFor` seconds at 7 yards a second
// a trail point every 0.6 yards a quarter yard ahead of the feet, at full
// strength, and with the hands at the surface a stroke's ring from each
// every swim_wave::kCooldown, at full strength; each kept as long as the
// renderer keeps it (swim_wave::kLife, kWakeRingLife).
struct Stir {
    float x, laid, sigma, strength;
    bool ring;
};
std::vector<Stir> wadeAndStop(float runFor) {
    std::vector<Stir> out;
    float last = -1e9f, nextRing = 0.0f;
    for (float t = 0.0f; t < runFor; t += 1.0f / 60.0f) {
        const float x = 7.0f * t;
        if (x - last >= 0.6f) {
            out.push_back({x + 0.25f, t, 0.45f, wt::kWakeRingStrength, false});
            last = x;
        }
        if (t >= nextRing) {
            out.push_back({x, t, 0.25f, 1.0f, true});
            out.push_back({x + 0.1f, t, 0.25f, 1.0f, true});
            nextRing = t + swim_wave::kCooldown;
        }
    }
    return out;
}
// The most the water stands off its rest within a few yards of the path,
// `at` seconds after the start.
float heaveAt(const std::vector<Stir>& stirs, float runFor, float at) {
    float most = 0.0f;
    for (float gx = -2.0f; gx <= 7.0f * runFor + 3.0f; gx += 0.35f) {
        for (float gy = 0.25f; gy <= 3.0f; gy += 0.35f) {
            float h = 0.0f;
            for (const Stir& s : stirs) {
                const float age = at - s.laid;
                const float life = s.ring ? swim_wave::kLife : wt::kWakeRingLife;
                if (age <= 0.0f || age >= life) continue;
                const float fade = s.ring ? wt::swimRingFade(age) : wt::wakeRingFade(age);
                h += wt::impulseRingHeight(std::hypot(gx - s.x, gy), age, s.sigma, s.strength * fade);
            }
            most = std::max(most, std::abs(h));
        }
    }
    return most;
}
}  // namespace

TEST_CASE("the water settles over seconds after a wader stops, with no snap", "[water_tess][wake][settle]") {
    // Every fade ends at nothing, without a step: what the renderer drops at
    // the end of a ring's or a point's life has already gone.
    struct Fade {
        float (*f)(float);
        float life;
    };
    for (const Fade& fade : {Fade{wt::swimRingFade, swim_wave::kLife}, Fade{wt::wakeRingFade, wt::kWakeRingLife}}) {
        INFO("life " << fade.life);
        CHECK(fade.f(fade.life) == Catch::Approx(0.0f).margin(1e-6f));
        CHECK(fade.f(fade.life - 1.0f / 60.0f) < 1e-3f);
        float last = fade.f(0.0f);
        float steepest = 0.0f;
        for (float t = 1.0f / 60.0f; t <= fade.life; t += 1.0f / 60.0f) {
            const float now = fade.f(t);
            CHECK(now <= last + 1e-6f);
            steepest = std::max(steepest, last - now);
            last = now;
        }
        // Never more than a hundredth or so of the whole in a frame.
        CHECK(steepest < 0.015f);
    }
    // A run of three quarters of a second, then standing: the heave left
    // falls away gently - never by a fifth from one frame (30 Hz) to the
    // next while it is more than a few thousandths of a yard - and is still
    // there, smaller, seconds later. The stroke's rings were cut off at
    // 1.6 s two-thirds high: the water went flat in a frame.
    const float runFor = 0.75f;
    const auto stirs = wadeAndStop(runFor);
    float peak = 0.0f;
    float prev = heaveAt(stirs, runFor, runFor);
    float worstDrop = 1.0f;
    float at3 = 0.0f;
    for (float t = runFor + 1.0f / 30.0f; t <= runFor + 9.0f; t += 1.0f / 30.0f) {
        const float now = heaveAt(stirs, runFor, t);
        peak = std::max(peak, now);
        if (prev > 0.003f) worstDrop = std::min(worstDrop, now / prev);
        if (std::abs(t - (runFor + 3.0f)) < 0.5f / 30.0f) at3 = now;
        prev = now;
    }
    INFO("peak " << peak << " worst frame-to-frame share " << worstDrop << " at 3 s " << at3);
    CHECK(worstDrop > 0.8f);
    CHECK(at3 > 0.04f * peak);
    CHECK(at3 < 0.6f * peak);
    CHECK(heaveAt(stirs, runFor, runFor + wt::kWakeRingLife + 0.5f) == 0.0f);
}

TEST_CASE("the classic wake and its bow settle away too", "[water_tess][wake][settle]") {
    CHECK(wt::wakeClassicFade(0.0f) == Catch::Approx(1.0f));
    CHECK(wt::wakeClassicFade(wt::kWakeClassicLife) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(wt::wakeClassicFade(wt::kWakeClassicLife - 1.0f / 60.0f) < 1e-3f);
    // The froth outlasts no point (the renderer keeps a point for its rings'
    // life, longer than the froth's).
    CHECK(wt::kWakeFrothLife < wt::kWakeClassicLife);
    CHECK(wt::kWakeFrothLife < wt::kWakeRingLife);
    // The bow: whole while the trail goes on (a new point every tenth of a
    // second or so), gone within a few seconds of its stopping.
    CHECK(wt::bowFade(0.1f) > 0.8f);
    CHECK(wt::bowFade(3.0f) < 0.01f);
}

TEST_CASE("the shaders settle the rings as the C++ does", "[water_tess][wake][settle]") {
    const std::string glsl = wowee::test::slurp("assets/shaders/water_interaction.glsli");
    const std::string frag = wowee::test::slurp("assets/shaders/water.frag.glsl");
    const std::string tese = wowee::test::slurp("assets/shaders/water.tese.glsl");
    REQUIRE(glsl.size() > 1000);
    auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
    CHECK(has(glsl, "return exp(-t / decay) * (1.0 - smoothstep(0.4 * life, life, t));"));
    CHECK(has(glsl, "const float SWIM_RING_DECAY = 2.0;"));
    CHECK(has(glsl, "const float SWIM_RING_LIFE = 6.0;"));
    CHECK(has(glsl, "const float WAKE_RING_DECAY = 4.0;"));
    CHECK(has(glsl, "const float WAKE_RING_LIFE = 10.0;"));
    CHECK(has(glsl, "const float WAKE_CLASSIC_DECAY = 1.2;"));
    CHECK(has(glsl, "const float WAKE_CLASSIC_LIFE = 4.0;"));
    CHECK(has(glsl, "const float WAKE_CLASSIC_SPREAD = 0.5;"));
    CHECK(has(glsl, "float bowFade(float headAge) { return exp(-headAge / 0.6); }"));
    CHECK(wt::kSwimRingDecay == 2.0f);
    CHECK(wt::kSwimRingLife == 6.0f);
    CHECK(wt::kWakeRingDecay == 4.0f);
    CHECK(wt::kWakeRingLife == 10.0f);
    CHECK(wt::kWakeClassicDecay == 1.2f);
    CHECK(wt::kWakeClassicLife == 4.0f);
    CHECK(wt::kWakeClassicSpread == 0.5f);
    CHECK(has(frag, "const float WAKE_FROTH_LIFE = 2.5;"));
    CHECK(wt::kWakeFrothLife == 2.5f);
    // No fade by the old linear shares or cut-offs left in either stage.
    for (const std::string* stage : {&frag, &tese}) {
        CHECK_FALSE(has(*stage, "1.0 - w.z / 1.6"));
        CHECK_FALSE(has(*stage, "(1.0 - wp.z)"));
        CHECK(has(*stage, "bowFade(headAge)"));
        CHECK(has(*stage, "WAKE_CLASSIC_SPREAD * headAge"));
    }
}

TEST_CASE("a classic ring's crest stands at the height whose slope the shading uses", "[water_tess]") {
    // The swimmer's ring (b 4, width 0.28) and the wake's and the bow's (3.2).
    for (float b : {4.0f, 3.2f}) {
        for (float width : {0.22f, 0.28f, 0.32f}) {
            INFO("b " << b << " width " << width);
            // Along the radius d = radius + a * width, so dh/dd = dh/da / width.
            const double mismatch = slopeMismatch(
                [&](double d) { return wt::ringCrestHeight(static_cast<float>(d / width), b, width, 1.0f); },
                [&](double d) { return wt::ringCrestSlope(static_cast<float>(d / width), b, 1.0f); },
                -2.9 * width, 2.9 * width, 600);
            CHECK(mismatch < 0.05);
        }
        // Flat on both sides of the crest.
        CHECK(std::abs(wt::ringCrestHeight(-3.5f, b, 0.3f, 1.0f)) < 1e-4f);
        CHECK(std::abs(wt::ringCrestHeight(3.5f, b, 0.3f, 1.0f)) < 1e-4f);
        CHECK(std::abs(wt::ringCrestHeight(2.999f, b, 0.3f, 1.0f)) < 1e-3f);
    }
}

TEST_CASE("no tessellated piece is cut narrower than twenty pixels", "[water_tess]") {
    // 1080 rows over a 70 degree field of view.
    const float yardsPerPixel = 2.0f * std::tan(0.5f * 70.0f * 3.14159265f / 180.0f) / 1080.0f;
    for (float d = 1.0f; d <= 120.0f; d += 1.0f) {
        const float piece = wt::kGridEdge / wt::level(wt::kGridEdge, d, yardsPerPixel);
        INFO("distance " << d);
        CHECK(piece / (d * yardsPerPixel) >= wt::kMinPixels - 1e-3f);
        // Never finer than without the floor.
        CHECK(wt::level(wt::kGridEdge, d, yardsPerPixel) <= wt::level(wt::kGridEdge, d) + 1e-5f);
    }
    // Near the camera, as before.
    CHECK(wt::level(wt::kGridEdge, 5.0f, yardsPerPixel) == Catch::Approx(wt::level(wt::kGridEdge, 5.0f)));
}

// The "Water tessellation" slider (watertessdensity): 100 % is the cut the
// water had before the slider, bit for bit; the rest scale the pieces.
namespace {
// The cut as it was before the slider (git d5f694ebc's level).
float levelBeforeSlider(float edgeLength, float dist, float yardsPerPixel) {
    const float target = std::max(wt::kTargetEdge, dist * yardsPerPixel * wt::kMinPixels);
    const float full = std::clamp(edgeLength / target, 1.0f, wt::kMaxLevel);
    return 1.0f + (full - 1.0f) * (1.0f - wt::smoothstep(wt::kNear, wt::kFar, dist));
}
}  // namespace

TEST_CASE("the tessellation slider's default is the cut as it was", "[water_tess]") {
    CHECK(wt::kDensityDefaultPercent == 100);
    CHECK(wt::densityFromPercent(wt::kDensityDefaultPercent) == 1.0f);
    CHECK(wt::maxLevel(1.0f) == wt::kMaxLevel);
    const float yardsPerPixel = 2.0f * std::tan(0.5f * 70.0f * 3.14159265f / 180.0f) / 1001.0f;
    for (float edge : {wt::kGridEdge, wt::kGridEdge * std::sqrt(2.0f), 0.5f * wt::kGridEdge}) {
        for (float d = 0.0f; d <= 120.0f; d += 0.25f) {
            INFO("edge " << edge << " distance " << d);
            // Exactly, not approximately: the default must draw the same pixels.
            CHECK(wt::level(edge, d, yardsPerPixel, wt::densityFromPercent(100)) ==
                  levelBeforeSlider(edge, d, yardsPerPixel));
            CHECK(wt::level(edge, d, 0.0f, 1.0f) == levelBeforeSlider(edge, d, 0.0f));
        }
    }
}

TEST_CASE("the tessellation slider maps percent to density and clamps", "[water_tess]") {
    CHECK(wt::densityFromPercent(25) == Catch::Approx(0.25f));
    CHECK(wt::densityFromPercent(50) == Catch::Approx(0.5f));
    CHECK(wt::densityFromPercent(150) == Catch::Approx(1.5f));
    CHECK(wt::densityFromPercent(200) == Catch::Approx(2.0f));
    // Out of range (an edited settings.cfg) is held to the slider's ends.
    CHECK(wt::densityFromPercent(0) == Catch::Approx(0.25f));
    CHECK(wt::densityFromPercent(-40) == Catch::Approx(0.25f));
    CHECK(wt::densityFromPercent(1000) == Catch::Approx(2.0f));
    CHECK(wt::maxLevel(2.0f) == Catch::Approx(2.0f * wt::kMaxLevel));
    CHECK(wt::maxLevel(0.5f) == Catch::Approx(wt::kMaxLevel));
    // Every device allows 64 pieces an edge.
    CHECK(wt::maxLevel(wt::densityFromPercent(wt::kDensityMaxPercent)) <= 64.0f);
}

TEST_CASE("the tessellation slider scales the pieces in proportion", "[water_tess]") {
    const float yardsPerPixel = 2.0f * std::tan(0.5f * 70.0f * 3.14159265f / 180.0f) / 1001.0f;
    // Near the camera, uncapped and unfaded: pieces 0.3 / density yards.
    for (int pct : {25, 50, 100, 150, 200}) {
        const float density = wt::densityFromPercent(pct);
        INFO("percent " << pct);
        CHECK(wt::level(wt::kGridEdge, 5.0f, yardsPerPixel, density) ==
              Catch::Approx(std::max(1.0f, wt::kGridEdge * density / wt::kTargetEdge)));
    }
    // Past the pixel floor's reach the pieces are 20 / density pixels wide.
    for (int pct : {50, 100, 200}) {
        const float density = wt::densityFromPercent(pct);
        for (float d = 15.0f; d <= 19.0f; d += 1.0f) {
            const float piece = wt::kGridEdge / wt::level(wt::kGridEdge, d, yardsPerPixel, density);
            INFO("percent " << pct << " distance " << d);
            CHECK(piece / (d * yardsPerPixel) == Catch::Approx(wt::kMinPixels / density).epsilon(0.02));
        }
    }
    // More pieces at every distance as the slider goes up, never fewer than one,
    // still the grid whole from kFar.
    for (float d = 0.0f; d <= 120.0f; d += 0.5f) {
        float last = 0.0f;
        for (int pct = wt::kDensityMinPercent; pct <= wt::kDensityMaxPercent; pct += 5) {
            const float l = wt::level(wt::kGridEdge, d, yardsPerPixel, wt::densityFromPercent(pct));
            CHECK(l >= last - 1e-5f);
            CHECK(l >= 1.0f);
            last = l;
        }
    }
    CHECK(wt::level(wt::kGridEdge, wt::kFar, yardsPerPixel, 2.0f) == Catch::Approx(1.0f));
}

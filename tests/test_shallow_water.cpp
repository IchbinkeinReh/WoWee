// Waves in shallow water (rendering/shallow_water.hpp, the twin of
// water_waves.glsli's shore waves), against the textbook figures.
#include <catch_amalgamated.hpp>

#include "rendering/shallow_water.hpp"

#include <cmath>
#include <limits>

namespace sw = wowee::rendering::shallow_water;
namespace ws = wowee::rendering::wave_spectrum;

TEST_CASE("the dispersion relation holds at every depth", "[shallow_water]") {
    for (float omega : {0.3f, 0.8f, 1.5f, 4.0f}) {
        for (float h : {0.05f, 0.5f, 3.0f, 20.0f, 300.0f}) {
            const float k = sw::wavenumberExact(omega, h);
            CHECK(ws::kGravity * k * std::tanh(k * h) == Catch::Approx(omega * omega).epsilon(1e-4));
            // The shader's explicit approximation and its one Newton step.
            CHECK(sw::wavenumber(omega, h) == Catch::Approx(k).epsilon(2e-3));
        }
    }
}

TEST_CASE("deep water gives k = omega^2 / g, shallow water c = sqrt(g h)", "[shallow_water]") {
    const float omega = 2.0f;
    // k h well over pi: deep.
    const float deep = sw::wavenumberExact(omega, 500.0f);
    CHECK(deep == Catch::Approx(ws::wavenumber(omega)).epsilon(1e-4));
    CHECK(sw::wavenumber(omega, 500.0f) == Catch::Approx(ws::wavenumber(omega)).epsilon(1e-4));
    // k h small: the long-wave speed, the same for every frequency.
    for (float w : {0.2f, 0.5f}) {
        const float h = 0.05f;
        const float k = sw::wavenumberExact(w, h);
        CHECK(sw::phaseSpeed(w, k) == Catch::Approx(std::sqrt(ws::kGravity * h)).epsilon(5e-3));
        CHECK(sw::groupSpeed(w, k, h) == Catch::Approx(std::sqrt(ws::kGravity * h)).epsilon(5e-3));
    }
    // In between the waves slow and shorten as the water shallows.
    float lastK = 0.0f;
    for (float h : {100.0f, 10.0f, 3.0f, 1.0f, 0.3f}) {
        const float k = sw::wavenumberExact(0.8f, h);
        CHECK(k > lastK);
        lastK = k;
    }
}

TEST_CASE("deep water's group runs at half the phase speed", "[shallow_water]") {
    const float omega = 3.0f;
    const float k = sw::wavenumberExact(omega, 1000.0f);
    CHECK(sw::groupSpeed(omega, k, 1000.0f) == Catch::Approx(0.5f * sw::phaseSpeed(omega, k)).epsilon(1e-4));
}

TEST_CASE("shoaling is 1 in deep water and grows as the water shallows", "[shallow_water]") {
    const float omega = 0.8f;
    CHECK(sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, 2000.0f), 2000.0f) ==
          Catch::Approx(1.0f).epsilon(1e-3));
    // The textbook dip: at k0 h about 0.8 (h / L0 about 0.13) the group runs
    // faster than in deep water and Ks is at its least, about 0.91.
    const float k0 = ws::wavenumber(omega);
    const float hDip = 0.8f / k0;
    const float dip = sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, hDip), hDip);
    CHECK(dip == Catch::Approx(0.913f).margin(0.01f));
    // Then up, without a break, to the cap.
    float last = dip;
    for (float f : {0.4f, 0.2f, 0.1f, 0.05f, 0.02f}) {
        const float h = f / k0;
        const float ks = sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, h), h);
        CHECK(ks > last);
        last = ks;
    }
    // Green's law in the shallows: Ks goes as h^(-1/4).
    const float h1 = 0.02f / k0, h2 = 0.01f / k0;
    const float r = sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, h2), h2) /
                    sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, h1), h1);
    CHECK(r == Catch::Approx(std::pow(2.0f, 0.25f)).epsilon(0.02));
    CHECK(sw::shoalingCoefficient(omega, sw::wavenumberExact(omega, 1e-4f), 1e-4f) == sw::kMaxShoaling);
    // Where the shader stops working the depth out (kDeepKh), the waves are
    // as in deep water: no step where it does.
    const float hDeep = sw::kDeepKh / k0;
    const float kDeep = sw::wavenumber(omega, hDeep);
    CHECK(sw::shoalingCoefficient(omega, kDeep, hDeep) == Catch::Approx(1.0f).margin(1e-3));
    CHECK(kDeep == Catch::Approx(k0).epsilon(1e-3));
    CHECK(sw::refractionBlend(kDeep * hDeep) == 0.0f);
}

TEST_CASE("waves break at McCowan's limit, and are held under it smoothly", "[shallow_water]") {
    CHECK(sw::depthLimitedHeight(2.0f) == Catch::Approx(1.56f));
    CHECK(sw::breakerIndex(0.78f, 1.0f) == Catch::Approx(1.0f));
    CHECK(sw::breakerIndex(0.39f, 1.0f) == Catch::Approx(0.5f));
    // Well under the limit the height is kept; over it, held near the limit.
    CHECK(sw::limitScale(0.3f) == Catch::Approx(1.0f).epsilon(1e-3));
    for (float b : {0.5f, 1.0f, 2.0f, 5.0f, 50.0f}) {
        const float kept = b * sw::limitScale(b);
        CHECK(kept <= 1.0f);
        CHECK(kept <= b);
    }
    CHECK(5.0f * sw::limitScale(5.0f) == Catch::Approx(1.0f).epsilon(1e-3));
    // Never decreasing: a higher wave never comes out lower.
    float last = 0.0f;
    for (float b = 0.0f; b < 4.0f; b += 0.05f) {
        const float kept = b * sw::limitScale(b);
        CHECK(kept >= last);
        last = kept;
    }
}

TEST_CASE("the waves die away smoothly at the waterline", "[shallow_water]") {
    CHECK(sw::shallowFade(0.0f) == 0.0f);
    CHECK(sw::shallowFade(-1.0f) == 0.0f);
    CHECK(sw::shallowFade(sw::kFadeDepth) == 1.0f);
    CHECK(sw::shallowFade(10.0f) == 1.0f);
    // No step: a small change of depth makes a small change of height.
    for (float h = 0.0f; h < sw::kFadeDepth; h += 0.01f) {
        CHECK(std::abs(sw::shallowFade(h + 0.01f) - sw::shallowFade(h)) < 0.05f);
    }
    // And the depth's limit goes to nothing with it.
    CHECK(sw::depthLimitedHeight(0.0f) == 0.0f);
}

TEST_CASE("the waves turn toward the shore as Snell's law has it", "[shallow_water]") {
    // Deep: no turn. Shallow: nearly all of it.
    CHECK(sw::refractionBlend(sw::kShoreReachKh) == Catch::Approx(0.0f).margin(1e-6));
    CHECK(sw::refractionBlend(0.01f) == Catch::Approx(0.99f).margin(0.01f));
    // In between, 1 - c / c0 = 1 - tanh(k h).
    CHECK(sw::refractionBlend(0.5f) == Catch::Approx(1.0f - std::tanh(0.5f)));
    float last = 2.0f;
    for (float kh = 0.0f; kh <= sw::kShoreReachKh; kh += 0.1f) {
        const float r = sw::refractionBlend(kh);
        CHECK(r <= last);
        last = r;
    }
}

TEST_CASE("the shore phase is whole and runs at the long-wave wavenumber", "[shallow_water]") {
    const float omega = 1.0f;
    // Its gradient along a bed of the reference slope is omega / sqrt(g h).
    for (float h : {0.2f, 1.0f, 4.0f}) {
        const float dh = 1e-3f;
        const float dPhase = (sw::shorePhase(omega, h + dh) - sw::shorePhase(omega, h - dh)) / (2.0f * dh);
        CHECK(dPhase * sw::kReferenceSlope == Catch::Approx(sw::shoreWavenumber(omega, h, sw::kReferenceSlope)).epsilon(1e-2));
        CHECK(sw::shoreWavenumber(omega, h, sw::kReferenceSlope) ==
              Catch::Approx(omega / std::sqrt(ws::kGravity * (h + sw::kShorePhaseDepth))));
    }
    // Continuous in the depth, and still past its reach.
    const float reach = sw::shoreReachDepth(omega);
    CHECK(sw::shorePhase(omega, reach * 2.0f) == Catch::Approx(sw::shorePhase(omega, reach)));
    for (float h = 0.0f; h < reach; h += 0.25f) {
        CHECK(std::abs(sw::shorePhase(omega, h + 0.01f) - sw::shorePhase(omega, h)) < 1.0f);
    }
    // A steeper bed: shorter shore waves for the same frequency.
    CHECK(sw::shoreWavenumber(omega, 1.0f, 0.3f) > sw::shoreWavenumber(omega, 1.0f, 0.1f));
}

TEST_CASE("steepening toward the break never folds the surface", "[shallow_water]") {
    for (float kaSum : {0.2f, 0.75f, 1.5f, 4.0f}) {
        for (float b : {0.0f, 0.6f, 0.9f, 1.5f}) {
            const sw::Steepness s = sw::steepness(kaSum, b);
            CHECK(s.q <= 1.0f);
            CHECK(kaSum * std::sqrt(s.q * s.q + s.lean * s.lean) <= sw::kBreakingSteepness + 1e-5f);
        }
    }
    // Not breaking: deep water's shared steepness, and no lean.
    const sw::Steepness calm = sw::steepness(1.5f, 0.0f);
    CHECK(calm.q == Catch::Approx(ws::kTotalSteepness / 1.5f));
    CHECK(calm.lean == 0.0f);
    // Breaking: the crests lean shoreward.
    CHECK(sw::steepness(1.5f, 1.2f).lean > 0.0f);
}

TEST_CASE("the shore field points up the bed, smoothed", "[shallow_water]") {
    // A plane rising 0.1 yd a yard along +gx, a grid 4 yards apart, with a gap.
    const int n = 9;
    const float cell = 4.0f;
    std::vector<float> z(n * n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) z[y * n + x] = 0.1f * cell * x;
    }
    z[4 * n + 4] = std::numeric_limits<float>::quiet_NaN();
    const auto field = sw::uphillField(z, n, cell);
    for (int i : {0, 3 * n + 4, 4 * n + 4, n * n - 1}) {
        CHECK(field[i].gx == Catch::Approx(0.1f).epsilon(1e-4));
        CHECK(field[i].gy == Catch::Approx(0.0f).margin(1e-6));
    }
    CHECK(sw::slopeWeight(0.0f) == 0.0f);
    CHECK(sw::slopeWeight(0.1f) == 1.0f);
}

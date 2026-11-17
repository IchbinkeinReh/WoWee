// The physics waves' spectrum (rendering/wave_spectrum.hpp), against the
// textbook figures it is built from.
#include <catch_amalgamated.hpp>

#include "rendering/wave_spectrum.hpp"

#include <cmath>

namespace ws = wowee::rendering::wave_spectrum;

TEST_CASE("each wave runs at the speed deep water gives its length", "[wave_spectrum]") {
    const auto waves = ws::sample(7.0f, ws::kOceanFetch, 0.0f);
    for (const auto& w : waves) {
        // omega^2 = g k, so the phase speed is sqrt(g / k).
        CHECK(w.omega * w.omega == Catch::Approx(ws::kGravity * w.k).epsilon(1e-4));
        CHECK(ws::phaseSpeed(w.omega) == Catch::Approx(std::sqrt(ws::kGravity / w.k)).epsilon(1e-4));
    }
    // Longer waves outrun shorter ones.
    CHECK(ws::phaseSpeed(waves.front().omega) > ws::phaseSpeed(waves.back().omega));
}

TEST_CASE("a fully developed sea peaks where Pierson-Moskowitz puts it", "[wave_spectrum]") {
    // omega_p = 0.855 g / U for an unlimited fetch.
    const float u = 10.0f * ws::kYardsPerMetre;
    CHECK(ws::peakOmega(u, 1.0e9f) == Catch::Approx(0.855f * ws::kGravity / u).epsilon(1e-4));
    // A short fetch peaks at a higher frequency: shorter, younger waves.
    CHECK(ws::peakOmega(u, ws::kInlandFetch) > ws::peakOmega(u, ws::kOceanFetch));
}

TEST_CASE("the significant wave height grows with the wind and the fetch", "[wave_spectrum]") {
    const float calm = ws::significantHeight(ws::sample(3.0f, ws::kOceanFetch, 0.0f));
    const float moderate = ws::significantHeight(ws::sample(7.0f, ws::kOceanFetch, 0.0f));
    const float rough = ws::significantHeight(ws::sample(15.0f, ws::kOceanFetch, 0.0f));
    CHECK(calm < moderate);
    CHECK(moderate < rough);
    // A lake under the same wind stays far lower than the sea.
    const float lake = ws::significantHeight(ws::sample(7.0f, ws::kInlandFetch, 0.0f));
    CHECK(lake < 0.25f * moderate);
    // JONSWAP's fetch-limited height, Hs = 0.0016 U sqrt(F / g) (metres):
    // about 0.11 m for 7 m/s over a kilometre. The 12 samples catch most of it.
    const float fetchMetres = ws::kInlandFetch / ws::kYardsPerMetre;
    const float expected = 0.0016f * 7.0f * std::sqrt(fetchMetres / 9.80665f) * ws::kYardsPerMetre;
    CHECK(lake == Catch::Approx(expected).epsilon(0.35));
}

TEST_CASE("the waves never fold over", "[wave_spectrum]") {
    for (float wind : {1.0f, 7.0f, 20.0f}) {
        for (float fetch : {ws::kInlandFetch, ws::kOceanFetch}) {
            const auto waves = ws::sample(wind, fetch, 0.3f);
            float sum = 0.0f;
            for (const auto& w : waves) sum += w.steepness * w.k * w.amplitude;
            CHECK(sum <= ws::kTotalSteepness + 1e-4f);
        }
    }
}

TEST_CASE("the amplitude scale scales every wave", "[wave_spectrum]") {
    const auto one = ws::sample(7.0f, ws::kOceanFetch, 0.0f, 1.0f);
    const auto half = ws::sample(7.0f, ws::kOceanFetch, 0.0f, 0.5f);
    for (std::size_t i = 0; i < one.size(); ++i) CHECK(half[i].amplitude == Catch::Approx(0.5f * one[i].amplitude));
    CHECK(ws::significantHeight(ws::sample(7.0f, ws::kOceanFetch, 0.0f, 0.0f)) == 0.0f);
}

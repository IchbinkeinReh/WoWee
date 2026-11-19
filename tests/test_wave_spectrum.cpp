// The physics waves' spectrum (rendering/wave_spectrum.hpp), against the
// textbook figures it is built from.
#include <catch_amalgamated.hpp>

#include "rendering/wave_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

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

TEST_CASE("a disturbance's rings: each length where its group velocity has carried it", "[wave_spectrum]") {
    for (float t : {0.3f, 1.0f, 1.9f}) {
        for (float r : {0.5f, 2.0f, 6.0f}) {
            const float k = ws::impulseWavenumber(r, t);
            CHECK(ws::groupSpeed(k) * t == Catch::Approx(r).epsilon(1e-4));
            CHECK(ws::impulsePhase(r, t) == Catch::Approx(k * r).epsilon(1e-4));
        }
    }
    // Long waves lead: farther out, the waves are longer (smaller k).
    CHECK(ws::impulseWavenumber(6.0f, 1.0f) < ws::impulseWavenumber(2.0f, 1.0f));
}

TEST_CASE("rain roughens the water a little, a storm a lot", "[wave_spectrum]") {
    const auto calm = ws::weatherWaves(0.0f, 0.0f, 1.0f);
    CHECK(calm.windScale == 1.0f);
    CHECK(calm.amplitudeScale == 1.0f);
    CHECK(calm.speedScale == 1.0f);
    const auto rain = ws::weatherWaves(1.0f, 0.0f, 1.0f);
    const auto storm = ws::weatherWaves(1.0f, 1.0f, 1.0f);
    CHECK(rain.windScale > 1.0f);
    CHECK(rain.amplitudeScale > 1.0f);
    CHECK(storm.windScale - 1.0f > 3.0f * (rain.windScale - 1.0f));
    CHECK(storm.amplitudeScale - 1.0f > 3.0f * (rain.amplitudeScale - 1.0f));
    CHECK(storm.speedScale > 1.0f);
    // The slider: 0 none, 2 twice the default's effect.
    CHECK(ws::weatherWaves(1.0f, 1.0f, 0.0f).windScale == 1.0f);
    CHECK(ws::weatherWaves(1.0f, 1.0f, 2.0f).windScale - 1.0f ==
          Catch::Approx(2.0f * (storm.windScale - 1.0f)));
    // 7 m/s under a full thunderstorm at the default: a rough sea.
    CHECK(7.0f * storm.windScale > 15.0f);
}

// Physics waves on, a clear night at Stonebull Lake: the default 7 m/s drew the
// lake as a busy field of glints with a regular pattern. Calm weather calms
// them; a storm leaves them as they were.
TEST_CASE("calm weather calms the physics waves, a storm leaves them be", "[wave_spectrum]") {
    const auto calm = ws::weatherWaves(0.0f, 0.0f, 1.0f);
    CHECK(calm.physicsAmplitudeScale == Catch::Approx(1.0f - ws::kCalmAmplitude));
    CHECK(calm.calmMinWavelength == Catch::Approx(ws::kCalmMinWavelength));
    CHECK(calm.detailScale == Catch::Approx(1.0f - ws::kCalmDetail));
    CHECK(calm.physicsAmplitudeScale < 0.75f);
    CHECK(calm.detailScale < 0.5f);
    // A lake's significant height at 7 m/s, calm: well under what the wind's
    // full spectrum gives.
    const float full = ws::significantHeight(ws::sample(7.0f, ws::kInlandFetch, ws::kWindDirection));
    const float calmed =
        ws::significantHeight(ws::sample(7.0f, ws::kInlandFetch, ws::kWindDirection, calm.physicsAmplitudeScale));
    CHECK(calmed < 0.7f * full);
    // Rain or a storm, all of it back.
    for (const auto& w : {ws::weatherWaves(1.0f, 0.0f, 1.0f), ws::weatherWaves(1.0f, 1.0f, 1.0f),
                          ws::weatherWaves(0.0f, 1.0f, 1.0f)}) {
        CHECK(w.physicsAmplitudeScale == 1.0f);
        CHECK(w.calmMinWavelength == 0.0f);
        CHECK(w.detailScale == 1.0f);
    }
    // Half the rain, half the calm.
    const auto light = ws::weatherWaves(0.5f, 0.0f, 1.0f);
    CHECK(light.physicsAmplitudeScale == Catch::Approx(1.0f - 0.5f * ws::kCalmAmplitude));
    // The slider at 0: the weather does nothing, calm included.
    const auto off = ws::weatherWaves(0.0f, 0.0f, 0.0f);
    CHECK(off.physicsAmplitudeScale == 1.0f);
    CHECK(off.calmMinWavelength == 0.0f);
    CHECK(off.detailScale == 1.0f);
}

TEST_CASE("how stormy the weather is", "[wave_spectrum]") {
    CHECK(ws::storminess(false, 0, 1.0f) == 0.0f);
    CHECK(ws::storminess(true, 3, 0.4f) == 0.0f);          // light rain: none
    CHECK(ws::storminess(true, 5, 1.0f) == Catch::Approx(0.5f));  // a downpour: half
    CHECK(ws::storminess(true, ws::kThunderstormWeather, 0.8f) == Catch::Approx(0.8f));
}

namespace {
// The physics waves' strength (their significant height) at a lake for a
// place on the ladder and the eased weather, as the renderer hands them on
// (Renderer: waterWindRung_, waterRain_, waterStorm_; wave_surface::paramsFor).
float waveStrength(float rungPos, float rain, float storm) {
    const auto rungs = ws::windRungs(7.0f, ws::windScaleAtRung(rungPos));
    const float amp = ws::weatherWaves(rain, storm, 1.0f).physicsAmplitudeScale;
    const auto sp = ws::sampleBlend(rungs.lower, ws::kInlandFetch, ws::kWindDirection, amp, rungs.upperShare,
                                    rungs.upper, rungs.lowerRung);
    float variance = 0.0f;
    for (std::size_t i = 0; i < sp.count; ++i) variance += 0.5f * sp.waves[i].amplitude * sp.waves[i].amplitude;
    return 4.0f * std::sqrt(variance);
}

struct WeatherRun {
    float blendSeconds = 0.0f;     // frames with two rungs summed
    float lastBlendAt = 0.0f;      // when the last of them was
    float maxRungStep = 0.0f;      // the most the place moved in a frame
    bool monotonic = true;         // the place and the strength one way only
    float maxStrengthStep = 0.0f;  // the most the strength moved in a frame, of the whole change
    float strengthLo = 1e9f, strengthHi = -1e9f;
    float startStrength = 0.0f, endStrength = 0.0f;
};

// From `from` to `to` (thunderstorm intensity, Weather.dbc row 86), the
// weather's intensity eased over `rampSeconds` (0 abrupt), 90 s at 60 fps.
// `oldEase`: the place as the weather's exponential ease had it before.
WeatherRun runWeather(float from, float to, float rampSeconds, bool oldEase) {
    const float dt = 1.0f / 60.0f;
    auto target = [](float i) {
        return std::pair<float, float>(i, ws::storminess(true, ws::kThunderstormWeather, i));
    };
    auto [rain, storm] = target(from);
    float place = ws::rungPosition(ws::weatherWaves(rain, storm, 1.0f).windScale);
    WeatherRun run;
    run.startStrength = waveStrength(place, rain, storm);
    const float rising = to > from ? 1.0f : -1.0f;
    float lastPlace = place, lastStrength = run.startStrength;
    std::vector<float> strengths;
    for (int frame = 1; frame <= 90 * 60; ++frame) {
        const float t = frame * dt;
        const float intensity =
            rampSeconds > 0.0f ? from + (to - from) * std::min(t / rampSeconds, 1.0f) : to;
        const auto [r, s] = target(intensity);
        const float ease = 1.0f - std::exp(-dt / 6.0f);
        rain += (r - rain) * ease;
        storm += (s - storm) * ease;
        if (oldEase) {
            place = ws::rungPosition(ws::weatherWaves(rain, storm, 1.0f).windScale);
        } else {
            place = ws::easeRung(place, ws::rungPosition(ws::weatherWaves(r, s, 1.0f).windScale), dt);
        }
        const auto rungs = ws::windRungs(7.0f, ws::windScaleAtRung(place));
        if (rungs.upperShare > 0.0f) {
            run.blendSeconds += dt;
            run.lastBlendAt = t;
        }
        const float strength = waveStrength(place, rain, storm);
        run.maxRungStep = std::max(run.maxRungStep, std::abs(place - lastPlace));
        if ((place - lastPlace) * rising < 0.0f) run.monotonic = false;
        if ((strength - lastStrength) * rising < -1e-6f * std::max(strength, 1e-3f)) run.monotonic = false;
        strengths.push_back(strength);
        run.strengthLo = std::min(run.strengthLo, strength);
        run.strengthHi = std::max(run.strengthHi, strength);
        lastPlace = place;
        lastStrength = strength;
    }
    run.endStrength = lastStrength;
    float prev = run.startStrength;
    const float whole = std::max(std::abs(run.endStrength - run.startStrength), 1e-6f);
    for (float s : strengths) {
        run.maxStrengthStep = std::max(run.maxStrengthStep, std::abs(s - prev) / whole);
        prev = s;
    }
    return run;
}
}  // namespace

TEST_CASE("the ladder's place eases toward the weather's and arrives", "[wave_spectrum][weather]") {
    CHECK(ws::rungPosition(1.0f) == 0.0f);
    CHECK(ws::rungPosition(ws::kWindRung * ws::kWindRung) == Catch::Approx(2.0f));
    CHECK(ws::windScaleAtRung(ws::rungPosition(1.9f)) == Catch::Approx(1.9f));
    CHECK(ws::windScaleAtRung(0.0f) == 1.0f);
    // On the target: stays. Never past it, either way.
    CHECK(ws::easeRung(1.0f, 1.0f, 0.016f) == 1.0f);
    CHECK(ws::easeRung(0.0f, 2.0f, 100.0f) == 2.0f);
    CHECK(ws::easeRung(2.0f, 0.0f, 100.0f) == 0.0f);
    // Far off: the weather's exponential (time constant kRungEaseTime).
    CHECK(ws::easeRung(0.0f, 2.0f, 0.1f) == Catch::Approx(2.0f * (1.0f - std::exp(-0.1f / ws::kRungEaseTime))));
    // Near: never slower than the floor's pace.
    CHECK(ws::easeRung(1.9f, 2.0f, 0.1f) == Catch::Approx(1.9f + ws::kRungMinPace * 0.1f));
    CHECK(ws::easeRung(0.1f, 0.0f, 0.1f) == Catch::Approx(0.1f - ws::kRungMinPace * 0.1f));
    // No time, no move.
    CHECK(ws::easeRung(0.5f, 2.0f, 0.0f) == 0.5f);
    CHECK(ws::easeRung(0.5f, 2.0f, -1.0f) == 0.5f);
}

TEST_CASE("a weather change sums two rungs' waves for seconds, not a minute", "[wave_spectrum][weather]") {
    // Clear to a full thunderstorm and back, abrupt and as the client eases
    // the intensity (ten seconds for the whole range), and to half a storm
    // (between two rungs to stay).
    struct Case { float from, to, ramp; };
    for (const Case c : {Case{0.0f, 1.0f, 0.0f}, Case{1.0f, 0.0f, 0.0f}, Case{0.0f, 1.0f, 10.0f},
                         Case{1.0f, 0.0f, 10.0f}, Case{0.0f, 0.5f, 5.0f}, Case{0.5f, 1.0f, 5.0f}}) {
        CAPTURE(c.from, c.to, c.ramp);
        const WeatherRun now = runWeather(c.from, c.to, c.ramp, false);
        const WeatherRun before = runWeather(c.from, c.to, c.ramp, true);
        // Arrives: the last frame with two rungs summed is soon after the
        // weather's own ramp (none past it when it ends between two).
        const bool endsOnRung = c.to == 0.0f || c.to == 1.0f;
        if (endsOnRung) {
            CHECK(now.lastBlendAt <= c.ramp + 8.0f);
            CHECK(now.blendSeconds <= c.ramp + 8.0f);
            // Before: the exponential's tail, most of a minute.
            CHECK(before.lastBlendAt > 40.0f);
        }
        // Smooth: the place moves at most the exponential's first pace a
        // frame, and the strength in small steps, one way only.
        CHECK(now.maxRungStep <= 2.0f / ws::kRungEaseTime / 60.0f * 1.01f + 1e-6f);
        CHECK(now.monotonic);
        CHECK(now.maxStrengthStep < 0.02f);
        // Never rougher (or calmer) than either weather: no "enormous wind".
        const float lo = std::min(now.startStrength, now.endStrength);
        const float hi = std::max(now.startStrength, now.endStrength);
        CHECK(now.strengthLo >= lo * 0.9999f);
        CHECK(now.strengthHi <= hi * 1.0001f);
        // And it ends where the old ease ended.
        CHECK(now.endStrength == Catch::Approx(before.endStrength).epsilon(1e-3));
    }
}

// The traced water's waves on the CPU (rendering/wave_surface.hpp), which
// swimmers, boats and the camera ride: the surface found over a point is
// where the waves carried the flat surface's point to, and there are none
// where the traced water has none.
#include <catch_amalgamated.hpp>

#include "rendering/wave_surface.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace wsf = wowee::rendering::wave_surface;
namespace ws = wowee::rendering::wave_spectrum;

namespace {

// A beach rising toward +X: 12 yards deep at x = 0, dry past x = 80, the way
// to the shallows +X, as loadFromTerrain gives the bed.
glm::vec4 beachBed(glm::vec2 at) {
    const float slope = 0.15f;
    return glm::vec4(12.0f - slope * at.x, 1.0f, 0.0f, slope);
}
glm::vec4 deepBed(glm::vec2) { return glm::vec4(500.0f, 0.0f, 0.0f, 0.0f); }

wsf::WaveSettings settings(bool physics, bool shore, float wind = 7.0f) {
    wsf::WaveSettings s;
    s.physicsWaves = physics;
    s.shoreWaves = shore;
    s.windSpeed = wind;
    return s;
}

// The reference: move a point of the flat surface by the waves, then ask
// for the surface over where it landed. The two must agree.
template <class BedAt>
void checkInverse(const wsf::WaveParams& p, BedAt bedAt, float heightTol) {
    int checked = 0;
    float worst = 0.0f;
    for (float t : {0.0f, 3.7f, 41.25f, 1234.5f}) {
        for (float x = -37.0f; x < 60.0f; x += 7.3f) {
            for (float y = -40.0f; y < 40.0f; y += 9.1f) {
                const glm::vec2 base(x, y);
                const wsf::Waves fwd = wsf::evaluate(p, base, t, bedAt(base));
                const glm::vec2 landed = base + glm::vec2(fwd.displacement.x, fwd.displacement.y);
                const wsf::SurfaceSample s = wsf::sample(p, landed, t, bedAt);
                worst = std::max(worst, std::abs(s.height - fwd.displacement.z));
                CHECK(s.height == Catch::Approx(fwd.displacement.z).margin(heightTol));
                CHECK(s.normal.z > 0.0f);
                CHECK(glm::length(s.normal) == Catch::Approx(1.0f).epsilon(1e-4));
                ++checked;
            }
        }
    }
    CHECK(checked > 100);
    INFO("worst height error " << worst);
}

}  // namespace

TEST_CASE("the surface over a point is where the waves carried the flat one", "[wave_surface]") {
    SECTION("the hand-made waves, a lake and the sea") {
        for (uint16_t liquid : {1, 2}) {
            const auto p = wsf::paramsFor(liquid, false, settings(false, false));
            REQUIRE(p);
            checkInverse(*p, deepBed, 0.001f);
        }
    }
    SECTION("the hand-made waves at the shore") {
        const auto p = wsf::paramsFor(2, false, settings(false, true));
        REQUIRE(p);
        REQUIRE(p->shore);
        checkInverse(*p, beachBed, 0.001f);
    }
    SECTION("the physics waves, a moderate and a rough sea") {
        for (float wind : {7.0f, 15.0f}) {
            const auto p = wsf::paramsFor(2, false, settings(true, false, wind));
            REQUIRE(p);
            REQUIRE(p->wind > 0.0f);
            checkInverse(*p, deepBed, 0.005f);
        }
    }
    SECTION("the physics waves coming ashore") {
        const auto p = wsf::paramsFor(2, false, settings(true, true, 10.0f));
        REQUIRE(p);
        checkInverse(*p, beachBed, 0.005f);
    }
}

TEST_CASE("the surface rises and falls with the waves", "[wave_surface]") {
    const auto p = wsf::paramsFor(2, false, settings(true, false, 12.0f));
    REQUIRE(p);
    float lo = 1e9f, hi = -1e9f;
    for (float t = 0.0f; t < 60.0f; t += 0.25f) {
        const float h = wsf::sample(*p, glm::vec2(10.0f, -3.0f), t, deepBed).height;
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    // A 12 m/s sea is well over a yard from trough to crest.
    CHECK(hi - lo > 1.0f);
    CHECK(hi > 0.0f);
    CHECK(lo < 0.0f);
}

TEST_CASE("only the waves the grid moves are ridden", "[wave_surface]") {
    // Every wave under the vertex stage's cut-off moves nothing: a cut-off
    // longer than the whole spectrum leaves the surface flat.
    const auto p = wsf::paramsFor(2, false, settings(true, false, 7.0f));
    REQUIRE(p);
    const auto w = wsf::evaluate(*p, glm::vec2(3.0f, 4.0f), 10.0f, deepBed({}), 1.0e6f);
    CHECK(w.displacement.z == 0.0f);
    CHECK(w.displacement.x == 0.0f);
    // And the default is water.vert's 9 yards.
    CHECK(wsf::kVertexMinWavelength == 9.0f);
}

TEST_CASE("deep water ashore takes the deep waves as they are", "[wave_surface]") {
    const auto deep = wsf::paramsFor(2, false, settings(true, false, 9.0f));
    const auto shore = wsf::paramsFor(2, false, settings(true, true, 9.0f));
    REQUIRE(deep);
    REQUIRE(shore);
    for (float x : {0.0f, 13.0f, -77.0f}) {
        const auto a = wsf::evaluate(*deep, glm::vec2(x, 5.0f), 17.0f, deepBed({}));
        const auto b = wsf::evaluate(*shore, glm::vec2(x, 5.0f), 17.0f, deepBed({}));
        CHECK(a.displacement.z == Catch::Approx(b.displacement.z).margin(1e-6));
    }
}

TEST_CASE("there are no waves to ride where the traced water has none", "[wave_surface]") {
    const auto on = settings(true, true, 15.0f);
    // A building's water: still.
    CHECK_FALSE(wsf::rideParams(true, true, 1, true, on));
    const auto wmo = wsf::paramsFor(13, true, on);
    REQUIRE(wmo);
    CHECK(wmo->amp == 0.0f);
    CHECK(wmo->wind == 0.0f);
    CHECK_FALSE(wmo->shore);
    for (float x : {0.0f, 4.0f, 33.0f}) {
        const auto s = wsf::sample(*wmo, glm::vec2(x, -x), 12.0f, deepBed);
        CHECK(s.height == 0.0f);
        CHECK(s.normal.z == Catch::Approx(1.0f));
    }
    // Magma and slime are the client's own, flat.
    CHECK_FALSE(wsf::paramsFor(3, false, on));
    CHECK_FALSE(wsf::paramsFor(4, false, on));
    CHECK_FALSE(wsf::rideParams(true, true, 3, false, on));
    // The setting off, or the traced water not drawn.
    CHECK_FALSE(wsf::rideParams(false, true, 2, false, on));
    CHECK_FALSE(wsf::rideParams(true, false, 2, false, on));
    CHECK(wsf::rideParams(true, true, 2, false, on));
    // Dry land past the waterline: the shore waves die away to nothing.
    const auto p = wsf::paramsFor(2, false, settings(true, true, 15.0f));
    REQUIRE(p);
    const auto dry = wsf::evaluate(*p, glm::vec2(100.0f, 0.0f), 3.0f, beachBed(glm::vec2(100.0f, 0.0f)));
    CHECK(dry.displacement.z == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("the push constants are the ones the water was drawn with", "[wave_surface]") {
    wsf::WaveSettings s;
    s.waveAmp = 1.5f;
    s.waveFreq = 0.8f;
    s.weather = ws::weatherWaves(0.5f, 0.25f, 1.0f);
    // An ocean, hand-made waves.
    auto p = wsf::paramsFor(2, false, s);
    REQUIRE(p);
    CHECK(p->basicType == 1.0f);
    CHECK(p->amp == Catch::Approx(0.35f * wowee::rendering::kRtWaveAmpScale * 1.5f * s.weather.amplitudeScale));
    CHECK(p->freq == Catch::Approx(0.20f * 0.8f));
    CHECK(p->speed == Catch::Approx(1.20f * s.weather.speedScale));
    CHECK(p->wind == 0.0f);
    // A river (liquid type 5) runs as a canal.
    p = wsf::paramsFor(5, false, s);
    REQUIRE(p);
    CHECK(p->basicType == 0.0f);
    CHECK(p->freq == Catch::Approx(0.35f * 0.8f));
    // Physics waves: the slider, the fetch, the wind's direction and the wind.
    s.physicsWaves = true;
    s.windSpeed = 8.0f;
    p = wsf::paramsFor(1, false, s);
    REQUIRE(p);
    CHECK(p->amp == Catch::Approx(1.5f * s.weather.physicsAmplitudeScale));
    CHECK(p->freq == ws::kInlandFetch);
    CHECK(p->speed == ws::kWindDirection);
    // The weather's wind as two rungs of the ladder (wave_spectrum::windRungs).
    CHECK(p->wind * std::pow(ws::kWindRung, p->windUpperShare) == Catch::Approx(8.0f * s.weather.windScale));
    CHECK(p->windUpper == Catch::Approx(p->windUpperShare > 0.0f ? p->wind * ws::kWindRung : 0.0f));
    CHECK(p->shore);
    CHECK(p->surf == Catch::Approx(s.weather.amplitudeScale));
    // The weather's calm for the normal (push.shore.z, w).
    CHECK(p->normalMinWavelength == Catch::Approx(s.weather.calmMinWavelength));
    CHECK(p->detail == Catch::Approx(s.weather.detailScale));
    // A clear sky calms them; the hand-made waves keep their detail.
    s.weather = ws::weatherWaves(0.0f, 0.0f, 1.0f);
    p = wsf::paramsFor(1, false, s);
    REQUIRE(p);
    CHECK(p->amp < 1.5f);
    CHECK(p->normalMinWavelength > 0.0f);
    CHECK(p->detail < 1.0f);
    s.physicsWaves = false;
    p = wsf::paramsFor(1, false, s);
    REQUIRE(p);
    CHECK(p->normalMinWavelength == 0.0f);
    CHECK(p->detail == 1.0f);
}

TEST_CASE("the riders follow smoothly and settle where they are put", "[wave_surface]") {
    wsf::CriticalSpring spring;
    float most = 0.0f;
    // A step: no overshoot, and there within a second and a half.
    for (int i = 0; i < 90; ++i) {
        spring.step(1.0f, 1.0f / 60.0f, wsf::kRideTau);
        most = std::max(most, spring.value);
    }
    CHECK(most <= 1.0f + 1e-5f);
    CHECK(spring.value == Catch::Approx(1.0f).margin(2e-3));
    // The same whatever the frame time.
    wsf::CriticalSpring coarse;
    for (int i = 0; i < 6; ++i) coarse.step(1.0f, 0.25f, wsf::kRideTau);
    CHECK(coarse.value == Catch::Approx(spring.value).margin(1e-2));
    // A long hitch does not throw it.
    wsf::CriticalSpring hitch;
    hitch.step(2.0f, 5.0f, wsf::kRideTau);
    CHECK(hitch.value == Catch::Approx(2.0f).margin(1e-3));
}

TEST_CASE("a rider leans with the surface, no more than the limit", "[wave_surface]") {
    const glm::vec3 gentle = glm::normalize(glm::vec3(-0.1f, 0.0f, 1.0f));
    const glm::vec2 s = wsf::slopesOf(gentle);
    CHECK(s.x == Catch::Approx(0.1f));
    CHECK(s.y == Catch::Approx(0.0f).margin(1e-6));
    const glm::vec2 steep = wsf::slopesOf(glm::normalize(glm::vec3(0.0f, -2.0f, 1.0f)));
    CHECK(glm::length(steep) == Catch::Approx(std::tan(wsf::kMaxTilt)));
    // The rotation stands the world's up on that slope.
    const glm::vec3 up = wsf::tiltOf(s) * glm::vec3(0.0f, 0.0f, 1.0f);
    CHECK(up.x == Catch::Approx(gentle.x).margin(1e-5));
    CHECK(up.z == Catch::Approx(gentle.z).margin(1e-5));
    // Raised and leant about where it stands: that point only rises.
    const glm::vec3 pivot(10.0f, -4.0f, 2.0f);
    const glm::mat4 m = wsf::rideMatrix(pivot, 0.75f, wsf::tiltOf(s));
    const glm::vec3 moved(m * glm::vec4(pivot, 1.0f));
    CHECK(moved.x == Catch::Approx(pivot.x));
    CHECK(moved.y == Catch::Approx(pivot.y));
    CHECK(moved.z == Catch::Approx(pivot.z + 0.75f));
    // Swimmers near the surface ride all of it, divers none.
    CHECK(wsf::swimmerRideWeight(1.5f) == 1.0f);
    CHECK(wsf::swimmerRideWeight(10.0f) == 0.0f);
    CHECK(wsf::swimmerRideWeight(-0.5f) == 0.0f);
}

TEST_CASE("a hull takes the mean of the waves along it and leans with them", "[wave_surface]") {
    // A plane rising 0.1 along +X and falling 0.05 along +Y, 0.5 up at the origin.
    auto plane = [](glm::vec2 at) { return 0.5f + 0.1f * at.x - 0.05f * at.y; };
    const auto b = wsf::bodyOnWaves(glm::vec2(0.0f), glm::vec2(0.0f, 3.0f), 20.0f, 6.0f, plane);
    CHECK(b.heave == Catch::Approx(0.5f));
    CHECK(b.slopes.x == Catch::Approx(0.1f));
    CHECK(b.slopes.y == Catch::Approx(-0.05f));
    // A wave as long as the hull mostly cancels: the heave is far less than its height.
    auto hullWave = [](glm::vec2 at) { return std::sin(at.x * 2.0f * 3.14159265f / 40.0f); };
    const auto c = wsf::bodyOnWaves(glm::vec2(10.0f, 0.0f), glm::vec2(1.0f, 0.0f), 20.0f, 6.0f, hullWave);
    CHECK(std::abs(c.heave) < 0.5f);
}

namespace {

// A weather change as the renderer has it: rain and storm eased toward the
// new weather's with a six-second time constant, a frame every 1/60 s, the
// session's clock already `t0` seconds on. Returns the fastest the surface
// rises or falls at `at` (yards a second), over 0.25 s steps so that it is
// the waves' own motion, while the weather goes from `from` to `to`.
struct Weather {
    float rain = 0.0f;
    float storm = 0.0f;
};
float fastestRise(Weather from, Weather to, bool physics, bool resampleEachFrame, float t0, float seconds,
                  glm::vec2 at) {
    float rain = from.rain, storm = from.storm;
    // The hand-made waves' summed pace (Renderer::waterWaveClock_), at the
    // first weather's pace up to t0.
    double clock = static_cast<double>(t0) * ws::weatherWaves(rain, storm, 1.0f).speedScale;
    const float dt = 1.0f / 60.0f;
    float t = t0;
    float last = 0.0f, lastT = t0;
    bool have = false;
    float fastest = 0.0f;
    for (int frame = 0; frame < static_cast<int>(seconds / dt); ++frame) {
        t += dt;
        const float ease = 1.0f - std::exp(-dt / 6.0f);
        rain += (to.rain - rain) * ease;
        storm += (to.storm - storm) * ease;
        wsf::WaveSettings s = settings(physics, false, 7.0f);
        s.weather = ws::weatherWaves(rain, storm, 1.0f);
        clock += static_cast<double>(dt) * s.weather.speedScale;
        if (!resampleEachFrame) s.weather.speedScale = ws::paceOverClock(clock, t);
        auto p = wsf::paramsFor(2, false, s);
        REQUIRE(p);
        if (resampleEachFrame && physics) {
            // The wind of the moment, one spectrum: what the water did.
            p->wind = 7.0f * s.weather.windScale;
            p->windUpper = 0.0f;
            p->windUpperShare = 0.0f;
        }
        if (frame % 15 != 0) continue;
        const float h = wsf::evaluate(*p, at, t, deepBed(at)).displacement.z;
        if (have) fastest = std::max(fastest, std::abs(h - last) / (t - lastT));
        last = h;
        lastT = t;
        have = true;
    }
    return fastest;
}

std::string slurpShader(const char* name) {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/assets/shaders/" + name);
    std::stringstream src;
    src << in.rdbuf();
    return src.str();
}

}  // namespace

// The user: switching the weather, the water looked as in an enormous wind
// while it blended. The waves' phase is k x - omega t on a clock that runs
// from the start of the session; with the spectrum resampled for the easing
// wind (and the hand-made waves' pace times that clock) the crests raced.
TEST_CASE("the water does not race while the weather changes", "[wave_surface][weather]") {
    const glm::vec2 at(-2179.8f, -732.2f);  // Stonebull Lake, far from the origin
    const float t0 = 1500.0f;
    const Weather clear{0.0f, 0.0f}, storm{1.0f, 1.0f};
    for (bool physics : {true, false}) {
        INFO((physics ? "physics waves" : "hand-made waves"));
        // The steady seas either side: the most the surface moves in each.
        const float calmRate = fastestRise(clear, clear, physics, false, t0, 40.0f, at);
        const float stormRate = fastestRise(storm, storm, physics, false, t0, 40.0f, at);
        const float steady = std::max(calmRate, stormRate);
        REQUIRE(steady > 0.0f);
        // Coming in and going: never faster than the faster of the two, but
        // for the sampling's margin.
        CHECK(fastestRise(clear, storm, physics, false, t0, 40.0f, at) < 1.25f * steady);
        CHECK(fastestRise(storm, clear, physics, false, t0, 40.0f, at) < 1.25f * steady);
        // What it did: the wind of the moment resampled (the pace times the
        // clock), several times the faster sea's motion.
        CHECK(fastestRise(clear, storm, physics, true, t0, 40.0f, at) > 3.0f * steady);
    }
}

TEST_CASE("the wind's rungs and the blend between them", "[wave_spectrum][weather]") {
    // On a rung: the slider's wind, and a full thunderstorm's at the default.
    auto r = ws::windRungs(7.0f, 1.0f);
    CHECK(r.lower == 7.0f);
    CHECK(r.upperShare == 0.0f);
    r = ws::windRungs(7.0f, ws::weatherWaves(1.0f, 1.0f, 1.0f).windScale);
    CHECK(r.lower == Catch::Approx(7.0f * (1.0f + ws::kRainWind + ws::kStormWind)));
    CHECK(r.upperShare == 0.0f);
    CHECK(r.lowerRung == 2.0f);
    CHECK(ws::kWindRung * ws::kWindRung == Catch::Approx(1.0f + ws::kRainWind + ws::kStormWind));
    // Between: the geometric place, the sea's height monotonic in the wind,
    // never folding.
    float lastHs = 0.0f;
    for (float scale = 1.0f; scale <= 2.6f; scale += 0.05f) {
        r = ws::windRungs(7.0f, scale);
        CHECK(r.lower * std::pow(ws::kWindRung, r.upperShare) == Catch::Approx(7.0f * scale).epsilon(1e-3));
        const auto sp =
            ws::sampleBlend(r.lower, ws::kInlandFetch, ws::kWindDirection, 1.0f, r.upperShare, r.upper, r.lowerRung);
        float variance = 0.0f, qka = 0.0f;
        for (size_t i = 0; i < sp.count; ++i) {
            variance += 0.5f * sp.waves[i].amplitude * sp.waves[i].amplitude;
            qka += sp.waves[i].steepness * sp.waves[i].k * sp.waves[i].amplitude;
        }
        const float hs = 4.0f * std::sqrt(variance);
        CHECK(hs >= lastHs - 1e-5f);
        CHECK(qka <= ws::kTotalSteepness + 1e-4f);
        lastHs = hs;
    }
    // Each end of a blend is that rung's sea.
    const float upperWind = 7.0f * ws::kWindRung;
    const auto lowerOnly = ws::sampleBlend(7.0f, ws::kInlandFetch, ws::kWindDirection, 1.0f, 0.0f, upperWind);
    const auto upperAll = ws::sampleBlend(7.0f, ws::kInlandFetch, ws::kWindDirection, 1.0f, 1.0f, upperWind);
    const auto upperRung = ws::sample(upperWind, ws::kInlandFetch, ws::kWindDirection);
    // The upper rung seen from below, all of it, is that rung seen as the
    // lower one: the same waves, in the same phases, as the wind crosses it.
    const auto onRung = ws::sampleBlend(upperWind, ws::kInlandFetch, ws::kWindDirection, 1.0f, 0.0f, 0.0f, 1.0f);
    for (size_t i = 0; i < ws::kWaveCount; ++i) {
        CHECK(std::remainder(upperAll.waves[ws::kWaveCount + i].phase - onRung.waves[i].phase, 6.2831853f) ==
              Catch::Approx(0.0f).margin(1e-3));
    }
    CHECK(lowerOnly.count == ws::kWaveCount);
    REQUIRE(upperAll.count == 2 * ws::kWaveCount);
    for (size_t i = 0; i < ws::kWaveCount; ++i) {
        CHECK(upperAll.waves[i].amplitude == Catch::Approx(0.0f).margin(1e-6));
        CHECK(upperAll.waves[ws::kWaveCount + i].amplitude == Catch::Approx(upperRung[i].amplitude));
        CHECK(upperAll.waves[ws::kWaveCount + i].omega == Catch::Approx(upperRung[i].omega));
    }
}

TEST_CASE("the shader blends the rungs as the CPU does", "[wave_surface][weather]") {
    CHECK(slurpShader("water_waves.glsli")
              .find("pwSampleSet(sp, PW_COUNT, ampScale * sqrt(f), fetch, pwWindBlend.y, kaSum);") !=
          std::string::npos);
    for (const char* stage : {"water.vert.glsl", "water.tese.glsl", "water.frag.glsl"}) {
        INFO(stage);
        const std::string src = slurpShader(stage);
        CHECK(src.find("pwWindBlend = push.windBlend.xyz;") != std::string::npos);
        CHECK(src.find("vec4 windBlend;") != std::string::npos);
    }
}

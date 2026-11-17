// The traced water's waves on the CPU (rendering/wave_surface.hpp), which
// swimmers, boats and the camera ride: the surface found over a point is
// where the waves carried the flat surface's point to, and there are none
// where the traced water has none.
#include <catch_amalgamated.hpp>

#include "rendering/wave_surface.hpp"

#include <cmath>
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
    CHECK(p->amp == 1.5f);
    CHECK(p->freq == ws::kInlandFetch);
    CHECK(p->speed == ws::kWindDirection);
    CHECK(p->wind == Catch::Approx(8.0f * s.weather.windScale));
    CHECK(p->shore);
    CHECK(p->surf == Catch::Approx(s.weather.amplitudeScale));
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

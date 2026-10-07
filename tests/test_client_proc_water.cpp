// Procedural water as wow.exe 3.3.5a draws it (rendering/client_proc_water.hpp):
// the wave manager, the ripple constants and the program constants. Every
// case is a number the exe states.

#include <catch_amalgamated.hpp>

#include "rendering/client_proc_water.hpp"

#include <cmath>

namespace pw = wowee::rendering::client_proc_water;

TEST_CASE("procedural water binds the LiquidType textures 0x008a48f0 names", "[liquid]") {
    // Units 0-5 take Textures 0, 1, 4, 5, 2, 3 (render states 0x15-0x1a).
    CHECK(pw::kUnitTexture == std::array<int, 6>{0, 1, 4, 5, 2, 3});
    // Textures 0, 1 and 4 over 0x4e2 ms; 2, 3 and 5 over Int[1], [2], [3].
    CHECK(pw::kTexturePeriodInt == std::array<int, 6>{-1, -1, 1, 2, -1, 3});
}

TEST_CASE("the CRT rand the wave manager draws from", "[liquid]") {
    pw::CrtRand r;
    // MSVC's rand() from seed 1.
    CHECK(r() == 41);
    CHECK(r() == 18467);
    CHECK(r() == 6334);
}

TEST_CASE("a wave's phase is 1024ths of a turn, 13 bits of them", "[liquid]") {
    // 0x008a3620: (round(t x speed) & 0x1fff) / 1024 x 2 pi.
    CHECK(pw::wavePhase(0, 0.03f) == 0.0f);
    CHECK(pw::wavePhase(1000, 0.512f) == Catch::Approx(3.14159265f).margin(1e-5));
    CHECK(pw::wavePhase(16384, 1.0f) == 0.0f);  // 0x4000 & 0x1fff
    CHECK(pw::wavePhase(8193, 1.0f) == Catch::Approx(0.0061359233f).margin(1e-6));
}

TEST_CASE("the wave manager spawns plane waves ahead of the camera", "[liquid]") {
    pw::WaveManager m;
    pw::CrtRand r;
    const glm::vec3 eye(1000.0f, 2000.0f, 50.0f);
    const glm::vec3 target(1001.0f, 2000.0f, 50.0f);  // looking along +x
    m.update(10000, eye, target, r);
    for (int i = 0; i < 3; ++i) {
        const auto& w = m.waves()[i];
        // 150-199 along the heading, -50..49 across.
        CHECK(w.center.x >= eye.x + 150.0f - 50.0f);
        CHECK(w.center.x <= eye.x + 199.0f + 49.0f);
        CHECK(std::abs(w.center.y - eye.y) <= 50.0f);
        CHECK(glm::length(w.dir) == Catch::Approx(1.0f).margin(1e-5));
        // Back toward the camera, within 45 degrees.
        const glm::vec2 back = glm::normalize(glm::vec2(eye) - w.center);
        CHECK(glm::dot(back, w.dir) >= std::cos(0.7853982f) - 1e-4f);
        CHECK(w.wavelength >= 10.0f);
        CHECK(w.wavelength < 50.0f);
        CHECK(w.falloff >= 200.0f);
        CHECK(w.falloff < 400.0f);
        CHECK(w.speed >= 0.02f);
        CHECK(w.speed < 0.05f);
        // A new wave starts flat and fades in over 5 s.
        CHECK(w.amplitude == 0.0f);
        CHECK(m.lifetime(i) >= 20000u);
        CHECK(m.lifetime(i) < 40000u);
    }
    m.update(12500, eye, target, r);
    for (int i = 0; i < 3; ++i) {
        CHECK(m.waves()[i].amplitude >= 0.25f);
        CHECK(m.waves()[i].amplitude < 0.5f);
    }
    // Circular waves are never set.
    for (const auto& c : m.circularWaves()) CHECK(c.amplitude == 0.0f);
}

TEST_CASE("ripple constants are the reciprocals 0x008a3620 and 0x008a3710 store", "[liquid]") {
    std::array<pw::Wave, 3> planes{};
    planes[0].center = {1.0f, 2.0f};
    planes[0].dir = {0.6f, 0.8f};
    planes[0].wavelength = 20.0f;
    planes[0].falloff = 250.0f;
    planes[0].amplitude = 0.75f;
    const auto r = pw::rippleConstants(0, {}, planes);
    CHECK(r.plane[0] == glm::vec4(1.0f, 2.0f, 0.6f, 0.8f));
    CHECK(r.planeFrequency[0] == Catch::Approx(0.05f));
    CHECK(r.planeFalloff[0] == Catch::Approx(0.004f));
    CHECK(r.planeAmplitude[0] == 0.75f);
    // An unset wave: 1 / 0.001 for its scales, 0x4479ffff for a circular falloff.
    CHECK(r.planeFrequency[1] == Catch::Approx(1000.0f));
    CHECK(r.circleFalloff[0] == Catch::Approx(999.99994f));
    CHECK(r.circle[0].z == Catch::Approx(1000.0f));
    CHECK(r.circle[0].w == 0.0f);
}

TEST_CASE("the pixel constants come from Floats 11 to 17", "[liquid]") {
    std::array<float, 18> f{};
    for (int i = 0; i < 18; ++i) f[i] = static_cast<float>(i);
    const auto p = pw::pixelConstants(f);
    CHECK(p.c10 == glm::vec4(1.0f / 11.0f, 12.0f, 13.0f, 14.0f));
    CHECK(p.c11 == glm::vec4(17.0f, 15.0f, 16.0f, 0.0f));
    f[11] = 0.0f;
    CHECK(pw::pixelConstants(f).c10.x == Catch::Approx(1000.0f));
    CHECK(pw::kSpecularPower == 50.0f);
}

TEST_CASE("the normal maps' matrices take Float i and Float i + 4", "[liquid]") {
    std::array<float, 18> f{};
    f[0] = 2.0f;  // scale
    f[4] = 0.0f;  // rotation
    f[3] = 0.5f;
    f[7] = 1.5707964f / 57.29578f;  // a quarter turn once times 57.29578
    const auto m = pw::normalMatrices(f);
    CHECK(m[0] * glm::vec2(1.0f, 0.0f) == glm::vec2(2.0f, 0.0f));
    const glm::vec2 q = m[3] * glm::vec2(1.0f, 0.0f);
    CHECK(q.x == Catch::Approx(0.0f).margin(1e-5));
    CHECK(q.y == Catch::Approx(0.5f));
}

// The full-screen effects' CPU side (rendering/screen_effect_state.hpp):
// 0x004f88b0, 0x004f7290, 0x004f8770, 0x008c2920, 0x008c2350, 0x00464580,
// 0x007e9b10, 0x009852a0, 0x007e8e40, 0x007e9670.
#include <catch_amalgamated.hpp>

#include "rendering/screen_effect_state.hpp"
#include "audio/screen_effect_audio.hpp"

#include <cmath>
#include <set>

namespace se = wowee::rendering::screen_effect;

namespace {
se::AuraEffects aura(uint32_t spell, uint32_t type0, uint32_t misc0, uint32_t type1 = 0, uint32_t misc1 = 0) {
    se::AuraEffects a;
    a.spellId = spell;
    a.auraType[0] = type0;
    a.miscValue[0] = misc0;
    a.auraType[1] = type1;
    a.miscValue[1] = misc1;
    return a;
}
}  // namespace

TEST_CASE("the row: an aura first, the last slot first", "[screen_effect]") {
    std::vector<se::AuraEffects> slots = {aura(10, se::kAuraScreenEffect, 7), aura(11, 3, 0),
                                          aura(12, 4, 0, se::kAuraScreenEffect, 9)};
    CHECK(se::chooseRow(slots, true, false, 0x40000000u) == 9);
    // An empty slot's leftovers are not read.
    slots[2].spellId = 0;
    CHECK(se::chooseRow(slots, true, false, 0) == 7);
}

TEST_CASE("the row: a ghost, then the invisibility glow", "[screen_effect]") {
    const std::vector<se::AuraEffects> none;
    CHECK(se::chooseRow(none, true, false, 0) == se::kRowDeath);
    // In an arena a ghost sees the world as it is - unless invisible.
    CHECK(se::chooseRow(none, true, true, 0) == 0);
    CHECK(se::chooseRow(none, true, true, 0x40000000u) == se::kRowInvisibility);
    CHECK(se::chooseRow(none, false, false, 0x40000000u) == 0x51);
    CHECK(se::chooseRow(none, false, false, 0x20000000u) == 0);
    CHECK(se::chooseRow(none, false, false, 0) == 0);
}

TEST_CASE("drunk: the more of the two, all of it at 100", "[screen_effect]") {
    CHECK(se::drunkAmount(0, 0) == 0.0f);
    CHECK(se::drunkAmount(50u << 8, 0) == Catch::Approx(0.5f));
    CHECK(se::drunkAmount(20u << 8, 60) == Catch::Approx(0.6f));
    CHECK(se::drunkAmount(100u << 8, 0) == 1.0f);
    CHECK(se::drunkAmount(0, 250) == 1.0f);
    // Only byte 1 of PLAYER_BYTES_3 is the drunkenness.
    CHECK(se::drunkAmount(0xFF0000FFu, 0) == 0.0f);
}

TEST_CASE("the glow's bytes", "[screen_effect]") {
    CHECK(se::clientByte(0.0f) == 0);
    CHECK(se::clientByte(1.0f) == 255);
    CHECK(se::clientByte(0.5f) == 127);
    // No player: neither the blend nor the wave.
    auto g = se::glowBlend(0.25f, false, 1.0f, true);
    CHECK(g.glow == 63);
    CHECK(g.blend == 0);
    CHECK_FALSE(g.wave);
    // In liquid: the wave and a third of the blur, drunker than that more.
    g = se::glowBlend(0.25f, true, 0.0f, true);
    CHECK(g.wave);
    CHECK(g.blend == 0x54);
    g = se::glowBlend(0.25f, true, 0.5f, true);
    CHECK(g.wave);
    CHECK(g.blend == 127);
    g = se::glowBlend(0.25f, true, 0.2f, false);
    CHECK_FALSE(g.wave);
    CHECK(g.blend == 51);
}

TEST_CASE("the wave: a sine round 128 texels, as signed bytes", "[screen_effect]") {
    CHECK(se::waveValue(0) == 0);
    CHECK(se::waveValue(32) == 127);
    CHECK(se::waveValue(96) == -128);
    CHECK(std::abs(static_cast<int>(se::waveValue(64))) <= 1);
    int sign = 0;
    for (uint32_t i = 1; i < 64; ++i) sign += se::waveValue(i) > 0 ? 1 : 0;
    CHECK(sign == 63);
    const auto tex = se::waveTexture();
    REQUIRE(tex.size() == 128u * 128u * 4u);
    CHECK(static_cast<int8_t>(tex[(5 * 128 + 32) * 4 + 0]) == 127);
    CHECK(static_cast<int8_t>(tex[(32 * 128 + 5) * 4 + 1]) == 127);
}

TEST_CASE("the wave's texture matrix", "[screen_effect]") {
    const auto w = se::waveTransform(3174u + 1587u, 1280, 1024);
    CHECK(w.tx == Catch::Approx(0.5f));
    CHECK(w.ty == Catch::Approx(static_cast<float>((3174u + 1587u) % 2805u) / 2805.0f));
    // Ten texels across the screen, turned ten degrees.
    CHECK(std::hypot(w.m00, w.m10 * 10.0f / (1024.0f * 0.88f / 128.0f)) == Catch::Approx(10.0f).margin(0.01));
    CHECK(std::atan2(w.m10 / (1024.0f * 0.88f / 128.0f), w.m00 / 10.0f) == Catch::Approx(0.17453292f));
}

TEST_CASE("the nether field: values in (-1, 1), easing, renewed", "[screen_effect]") {
    se::NetherField f;
    std::set<int> seen;
    for (int step = 0; step < 200; ++step) {
        f.advance(1.0f / 30.0f);
        for (int k = 0; k < se::NetherField::kPoints; ++k) {
            const float v = f.pointValue(k);
            REQUIRE(v >= -1.0f);
            REQUIRE(v <= 1.0f);
            seen.insert(static_cast<int>(std::lround(v * 100.0f)));
        }
        REQUIRE(f.phase() >= 0.0f);
        REQUIRE(f.phase() < 6.2831855f);
    }
    CHECK(seen.size() > 50u);
    // Small steps move the values by little: they ease, not jump.
    se::NetherField g;
    g.advance(0.01f);
    const float before = g.pointValue(0);
    g.advance(0.01f);
    CHECK(std::abs(g.pointValue(0) - before) < 0.1f);
}

TEST_CASE("the nether angle follows the world's x axis on screen", "[screen_effect]") {
    se::NetherField f;  // phase 0: half a radian added
    CHECK(f.angle(1.0f, 0.0f, 1.0f) == Catch::Approx(0.5f));
    CHECK(f.angle(0.0f, 1.0f, 1.0f) == Catch::Approx(1.5707964f + 0.5f));
    CHECK(f.angle(0.0f, -1.0f, 1.0f) == Catch::Approx(4.712389f + 0.5f));
}

TEST_CASE("the random walk is the client's", "[screen_effect]") {
    se::Random a(0xabcdef01u), b(0xabcdef01u), c(1u);
    for (int i = 0; i < 100; ++i) {
        const uint32_t x = a.next();
        CHECK(x == b.next());
        (void)c.next();
    }
    CHECK(a.next() != c.next());
    for (int i = 0; i < 1000; ++i) {
        const float v = a.nextSigned();
        REQUIRE(v > -1.0f);
        REQUIRE(v < 1.0f);
    }
}

TEST_CASE("the fog's noise", "[screen_effect]") {
    // The smoothed lattice stays in [-1, 1]; five octaves under 2.
    for (int y = -5; y < 5; ++y) {
        for (int x = -5; x < 5; ++x) {
            const float n = se::latticeNoise(x, y);
            REQUIRE(n >= -1.0f);
            REQUIRE(n <= 1.0f);
        }
    }
    CHECK(se::smoothNoise(3.0f, 4.0f) == Catch::Approx(se::latticeNoise(3, 4)));
    CHECK(se::smoothNoise(3.5f, 4.0f) ==
          Catch::Approx(0.5f * (se::latticeNoise(3, 4) + se::latticeNoise(4, 4))));
    CHECK(se::fractalNoise(1.25f, 2.5f, 1) == Catch::Approx(se::smoothNoise(1.25f, 2.5f)));
    const auto tex = se::fogNoiseTexture();
    REQUIRE(tex.size() == 256u * 256u * 4u);
    CHECK(tex[0] == 0xFF);
    CHECK(tex[1] == 0xFF);
    CHECK(tex[2] == 0xFF);
    std::set<uint8_t> alphas;
    for (size_t i = 3; i < tex.size(); i += 4) alphas.insert(tex[i]);
    CHECK(alphas.size() > 20u);
}

TEST_CASE("the state: fades and ramps start when their effect comes up", "[screen_effect]") {
    se::State s;
    se::Row nether{.id = 0x51, .kind = se::Kind::NetherWorld, .params = {0, 0, 0, 0}, .lightOverride = 0xFFFFFFFFu};
    s.select(&nether);
    s.advance(0.5f);
    CHECK(s.netherFade() == Catch::Approx(0.5f));
    s.advance(0.5f);
    CHECK(s.netherFade() == Catch::Approx(0.75f));
    // Gone and back: from nothing again.
    s.select(nullptr);
    CHECK(s.kind() == se::Kind::Glow);
    s.select(&nether);
    s.advance(0.1f);
    CHECK(s.netherFade() == Catch::Approx(0.1f));

    se::Row fog{.id = 99, .kind = se::Kind::Special, .params = {0x00FF8000, 51, 60, 0}, .lightOverride = 0xFFFFFFFFu};
    s.select(&fog);
    // The ramp is read before it counts down: nothing the first frame, half
    // way 1.5 s in.
    s.advance(1.5f);
    CHECK(s.fogDesaturate() == 0.0f);
    s.advance(1.5f);
    CHECK(s.fogDesaturate() == Catch::Approx(0.6f * 0.5f));
    CHECK(s.fogBrighten() == Catch::Approx(0.05f));
    CHECK(s.fogDecay() == Catch::Approx(0.2f));
    CHECK(s.fogColour() == 0x00FF8000u);
    s.advance(0.1f);
    CHECK(s.fogDesaturate() == Catch::Approx(0.6f));
    CHECK(s.fogBrighten() == Catch::Approx(0.1f));
    // A noise row a frame.
    const uint32_t r = s.fogNoiseRow();
    s.advance(0.01f);
    CHECK(s.fogNoiseRow() == (r + 1) % se::kFogNoiseSize);
}

TEST_CASE("a screen effect's sounds follow the client's sound day", "[screen_effect][sound]") {
    namespace sea = wowee::audio::screen_effect_audio;
    // 5:30 to 21:00 is day (0x004c9850).
    CHECK_FALSE(sea::isSoundDaytime(5.25f));
    CHECK(sea::isSoundDaytime(5.5f));
    CHECK(sea::isSoundDaytime(20.9f));
    CHECK_FALSE(sea::isSoundDaytime(21.0f));
    CHECK(sea::dayOrNight(11, 22, true) == 11);
    CHECK(sea::dayOrNight(11, 22, false) == 22);
}

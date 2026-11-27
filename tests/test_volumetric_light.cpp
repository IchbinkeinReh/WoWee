// The volumetric light: its arithmetic (volumetric_light_math.hpp, which the
// march shader mirrors) and the option's plumbing - the schema row, its
// German text, the settings file's key, the presets.
//
// The option is the user's request of 2026-11-27 and off by default: the
// original client has nothing like it, so a player who never chooses it must
// get today's picture, and no preset of the client's own levels may choose
// it for them.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/volumetric_light_math.hpp"
#include "test_support.hpp"
#include "ui/graphics_presets.hpp"
#include "ui/settings_i18n.hpp"
#include "ui/settings_schema.hpp"

namespace vl = wowee::rendering::volumetric_light;

namespace {

const wowee::ui::SettingDesc* row(const std::string& key) {
    std::size_t count = 0;
    const auto* schema = wowee::ui::clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (key == schema[i].key) return &schema[i];
    }
    return nullptr;
}

std::size_t characters(const char* utf8) {
    std::size_t n = 0;
    for (const char* p = utf8; *p; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xC0u) != 0x80u) ++n;
    }
    return n;
}

struct Lamp {
    glm::vec3 position{0.0f};
    float attenuationEnd = 0.0f;
};

}  // namespace

TEST_CASE("volumetric light: the option is a row of its own, off by default", "[volumetric_light][settings]") {
    const auto* r = row("volumetriclight");
    REQUIRE(r != nullptr);
    CHECK(r->kind == wowee::ui::SettingKind::Enum);
    CHECK(std::string(r->choices) == "Off|Low|High");
    CHECK(r->defaultValue == 0.0f);
    CHECK(r->minValue == 0.0f);
    CHECK(r->maxValue == 2.0f);
    // A graphics page, so the presets and "Reset to default" cover it.
    CHECK(std::string(r->category) == "Detail");
    // Its index is the quality the renderer takes.
    CHECK(vl::qualityFromIndex(0) == vl::Quality::Off);
    CHECK(vl::qualityFromIndex(1) == vl::Quality::Low);
    CHECK(vl::qualityFromIndex(2) == vl::Quality::High);
    CHECK(vl::qualityFromIndex(7) == vl::Quality::High);
    CHECK(vl::qualityFromIndex(-1) == vl::Quality::Off);
}

TEST_CASE("volumetric light: German label, tooltip and choices", "[volumetric_light][settings][i18n]") {
    const auto* de = wowee::ui::germanSettingText("volumetriclight");
    REQUIRE(de != nullptr);
    REQUIRE(de->label != nullptr);
    CHECK(std::string(de->label) == "Volumetrisches Licht");
    CHECK(characters(de->label) <= 24);
    REQUIRE(de->tooltip != nullptr);
    CHECK(std::string(de->tooltip).size() > 40);
    REQUIRE(de->choices != nullptr);
    CHECK(std::string(de->choices) == "Aus|Niedrig|Hoch");
}

TEST_CASE("volumetric light: the settings file saves and loads it", "[volumetric_light][settings]") {
    // GameScreen::saveSettings writes it, loadSettings reads it back clamped
    // to the row's range, and the panel applies it at start-up and on change.
    const std::string io = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    REQUIRE(io.size() > 1000);
    CHECK(io.find("out << \"volumetric_light=\" << settingsPanel_.pendingVolumetricLight") != std::string::npos);
    CHECK(io.find("key == \"volumetric_light\") settingsPanel_.pendingVolumetricLight = std::clamp(std::stoi(val), 0, 2)") !=
          std::string::npos);
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    CHECK(panel.find("{.key = \"volumetriclight\", .asInt  = &SettingsPanel::pendingVolumetricLight}") != std::string::npos);
    CHECK(panel.find("renderer->setVolumetricLightQuality(pendingVolumetricLight)") != std::string::npos);
    CHECK(panel.find("\"sunshafts\", \"volumetriclight\",") != std::string::npos);
}

TEST_CASE("volumetric light: no preset turns it on", "[volumetric_light][settings]") {
    // Not a preset column: every preset puts the row back to its default,
    // off. Super-Ultra too, being the user's own settings file of 2026-11-14,
    // which predates the option (tests/data/super_ultra_user_settings.cfg
    // has it at 0) - whether it should turn it on is the user's to say.
    for (const char* key : wowee::ui::kGraphicsPresetKeys) CHECK(std::string(key) != "volumetriclight");
    for (const bool superUltra : {false, true}) {
        bool seen = false;
        for (const auto& r : wowee::ui::presetRowValues(superUltra)) {
            if (std::string(r.row->key) != "volumetriclight") continue;
            seen = true;
            CHECK(r.value == 0.0f);
        }
        CHECK(seen);
    }
}

TEST_CASE("volumetric light: each quality has a bounded cost", "[volumetric_light]") {
    const vl::Budget off = vl::budgetFor(vl::Quality::Off);
    CHECK(off.divisor == 0);
    const vl::Budget low = vl::budgetFor(vl::Quality::Low);
    const vl::Budget high = vl::budgetFor(vl::Quality::High);
    // Never the full screen, never more lamps than the shader's array holds.
    CHECK(low.divisor >= 2);
    CHECK(high.divisor >= 2);
    CHECK(low.maxLights <= vl::kMaxLights);
    CHECK(high.maxLights <= vl::kMaxLights);
    CHECK(high.maxLights == vl::kMaxLights);
    // Low is cheaper in every part.
    CHECK(low.divisor > high.divisor);
    CHECK(low.sunSteps < high.sunSteps);
    CHECK(low.lightSteps < high.lightSteps);
    CHECK(low.maxLights < high.maxLights);
    CHECK(low.sunSteps >= 8);
    CHECK(high.sunSteps <= 32);
    CHECK(high.lightSteps <= 8);
}

TEST_CASE("volumetric light: a lamp's glow fades from its core to nothing at its reach", "[volumetric_light]") {
    const float end = 8.0f;
    const float core = 1.0f;  // an eighth of the reach
    CHECK(vl::glowCore(end) == Catch::Approx(core));
    CHECK(vl::glowFalloff(core, end) == Catch::Approx(2.0f * std::pow(1.0f - std::pow(core / end, 4.0f), 2.0f) / 2.0f));
    CHECK(vl::glowFalloff(0.0f, end) == Catch::Approx(2.0f));
    CHECK(vl::glowFalloff(end, end) == 0.0f);
    CHECK(vl::glowFalloff(end + 1.0f, end) == 0.0f);
    CHECK(vl::glowFalloff(1.0f, 0.0f) == 0.0f);
    float previous = vl::glowFalloff(0.0f, end);
    for (float d = 0.25f; d < end; d += 0.25f) {
        const float f = vl::glowFalloff(d, end);
        INFO("at " << d << " yards");
        CHECK(f <= previous);
        CHECK(f >= 0.0f);
        previous = f;
    }
    // The core is held between half a yard and a yard and a half: the
    // flame and the air right round it, not a ball the size of a room (T436).
    CHECK(vl::glowCore(1.0f) == Catch::Approx(0.5f));
    CHECK(vl::glowCore(40.0f) == Catch::Approx(1.5f));
    CHECK(vl::glowFalloff(0.5f, 1.0f) == Catch::Approx(2.0f * std::pow(1.0f - std::pow(0.5f, 4.0f), 2.0f) / 2.0f));
    CHECK(vl::glowFalloff(1.5f, 40.0f) == Catch::Approx(2.0f * std::pow(1.0f - std::pow(1.5f / 40.0f, 4.0f), 2.0f) / 2.0f));
    // Soft to its end: no step anywhere, and its slope nothing at the reach,
    // so the glow has no rim.
    for (float e : {3.0f, 8.0f, 12.0f}) {
        float last = vl::glowFalloff(0.0f, e);
        for (float d = 0.01f; d <= e + 0.5f; d += 0.01f) {
            const float f = vl::glowFalloff(d, e);
            INFO("end " << e << " at " << d);
            CHECK(std::abs(f - last) < 0.03f);
            last = f;
        }
        CHECK(vl::glowFalloff(e * 0.99f, e) < 1e-3f);
    }
}

TEST_CASE("volumetric light: a lamp's samples follow its glow, smooth with any count", "[volumetric_light]") {
    // The unwindowed glow 2 / (1 + (d / core)^2) along a ray passing `across`
    // from the lamp: each sample stands for exactly the whole, whatever its
    // place - no grain for any number of samples.
    const float core = 1.0f;
    for (float across : {0.0f, 0.3f, 1.0f, 4.0f}) {
        const float t0 = 2.0f, t1 = 18.0f, along = 10.0f;
        const float h = std::sqrt(across * across + core * core);
        const float exact = 2.0f * core * core / h *
                            (std::atan((t1 - along) / h) - std::atan((t0 - along) / h));
        for (float u : {0.05f, 0.3f, 0.5f, 0.77f, 0.99f}) {
            const vl::GlowSample s = vl::glowSample(along, across, t0, t1, core, u);
            const float x = s.t - along;
            const float d2 = across * across + x * x;
            const float f = 2.0f / (1.0f + d2 / (core * core));
            INFO("across " << across << " u " << u);
            CHECK(s.t >= t0 - 1e-4f);
            CHECK(s.t <= t1 + 1e-4f);
            CHECK(f * s.weight == Catch::Approx(exact).epsilon(1e-3));
        }
    }
    // With the window, four samples (Low) come within a few percent of the
    // glow along the ray, summed finely.
    const float end = 8.0f;
    const float c = vl::glowCore(end);
    for (float across : {0.0f, 0.5f, 2.0f, 5.0f}) {
        const float along = 20.0f;
        const float half = std::sqrt(end * end - across * across);
        const float t0 = along - half, t1 = along + half;
        double fine = 0.0;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            const float t = t0 + (t1 - t0) * (static_cast<float>(i) + 0.5f) / n;
            const float x = t - along;
            fine += vl::glowFalloff(std::sqrt(across * across + x * x), end) * (t1 - t0) / n;
        }
        double four = 0.0;
        for (int k = 0; k < 4; ++k) {
            const vl::GlowSample s = vl::glowSample(along, across, t0, t1, c, (static_cast<float>(k) + 0.5f) / 4.0f);
            const float x = s.t - along;
            four += vl::glowFalloff(std::sqrt(across * across + x * x), end) * s.weight / 4.0;
        }
        INFO("across " << across);
        CHECK(four == Catch::Approx(fine).epsilon(0.08));
    }
}

TEST_CASE("volumetric light: faint by default, Low fainter than High", "[volumetric_light]") {
    const vl::Scatter off = vl::scatterFor(vl::Quality::Off);
    const vl::Scatter low = vl::scatterFor(vl::Quality::Low);
    const vl::Scatter high = vl::scatterFor(vl::Quality::High);
    CHECK(off.body == 0.0f);
    CHECK(off.lamp == 0.0f);
    // Low visibly fainter, not the same at a lower size (T434).
    CHECK(low.body > 0.0f);
    CHECK(low.lamp > 0.0f);
    CHECK(low.body <= 0.7f * high.body);
    CHECK(low.lamp <= 0.7f * high.lamp);
    CHECK(low.body >= 0.4f * high.body);
    // A fifth of V572's sun and of its lamps at most.
    CHECK(high.body <= 0.001f / 5.0f);
    CHECK(high.lamp <= 0.03f / 5.0f);
    // Open air lit end to end adds nothing, whichever way one looks (T434).
    CHECK(vl::shaftWeight(1.0f) == 0.0f);
    // The worst a ray seen from the side can take, half in shade, in the
    // thickest air and a white sun: a few steps of 255 - no veil.
    float worst = 0.0f;
    for (float f = 0.0f; f <= 1.0f; f += 0.01f) worst = std::max(worst, f * vl::shaftWeight(f));
    const float side = high.body * vl::kMaxAirFactor * vl::kSunReach * vl::bodyPhase(0.0f) * worst;
    CHECK(side < 3.0f / 255.0f);
    // With the back to it, less still.
    CHECK(high.body * vl::kMaxAirFactor * vl::kSunReach * vl::bodyPhase(-1.0f) * worst < side);
    // A shaft toward the sun through a gap, twenty yards of lit air in shade
    // otherwise, clear air: seen, but not a white wall.
    const float shaft = high.body * 20.0f * vl::bodyPhase(1.0f) * vl::shaftWeight(20.0f / vl::kSunReach);
    CHECK(shaft > 4.0f / 255.0f);
    CHECK(shaft < 0.15f);
    // The moon less than the sun for the same light.
    CHECK(vl::kMoonScatterShare < 1.0f);
    CHECK(vl::kMoonScatterShare > 0.0f);
    // A lamp's glow through its middle at night, High, clear air: a soft
    // glow of a fraction of its colour.
    const float end = 9.0f;
    double through = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const float x = -end + 2.0f * end * (static_cast<float>(i) + 0.5f) / 2000.0f;
        through += vl::glowFalloff(std::abs(x), end) * (2.0f * end / 2000.0f);
    }
    CHECK(through * high.lamp < 0.12);
    CHECK(through * high.lamp > 0.02);
}

TEST_CASE("volumetric light: the sky body's light only beside shade", "[volumetric_light]") {
    CHECK(vl::shaftWeight(0.0f) == Catch::Approx(1.0f));
    CHECK(vl::shaftWeight(0.5f) == Catch::Approx(1.0f));
    CHECK(vl::shaftWeight(1.0f) == Catch::Approx(0.0f));
    float last = 1.0f;
    for (float f = 0.0f; f <= 1.0f; f += 0.005f) {
        const float w = vl::shaftWeight(f);
        INFO("lit share " << f);
        CHECK(w <= last + 1e-6f);
        CHECK(last - w < 0.02f);  // smooth: no edge where a ray's last sample turns lit
        last = w;
    }
}

TEST_CASE("volumetric light: a lamp's glow fainter by day", "[volumetric_light]") {
    CHECK(vl::lampDaylight(0.0f) == Catch::Approx(1.0f));
    CHECK(vl::lampDaylight(1.0f) == Catch::Approx(vl::kLampDaylightShare));
    CHECK(vl::lampDaylight(2.0f) == Catch::Approx(vl::kLampDaylightShare));
    CHECK(vl::lampDaylight(0.5f) < 1.0f);
    CHECK(vl::lampDaylight(0.5f) > vl::kLampDaylightShare);
    CHECK(vl::kLampDaylightShare > 0.0f);
}

TEST_CASE("volumetric light: a building's own light does not glow in the air", "[volumetric_light]") {
    enum class Src { Model, Fire, Lamp, Building, Unit, Emissive, Particle };
    CHECK_FALSE(vl::glowsInAir(Src::Building, Src::Building));
    for (Src s : {Src::Model, Src::Fire, Src::Lamp, Src::Unit, Src::Emissive, Src::Particle}) {
        CHECK(vl::glowsInAir(s, Src::Building));
    }
}

TEST_CASE("volumetric light: the shadow map's light fades out at its edge", "[volumetric_light]") {
    CHECK(vl::coverageFade(0.0f) == Catch::Approx(1.0f));
    CHECK(vl::coverageFade(0.8f) == Catch::Approx(1.0f));
    CHECK(vl::coverageFade(1.0f) == Catch::Approx(0.0f));
    CHECK(vl::coverageFade(1.5f) == Catch::Approx(0.0f));
    float last = 1.0f;
    for (float e = 0.0f; e <= 1.2f; e += 0.005f) {
        const float f = vl::coverageFade(e);
        INFO("at " << e);
        CHECK(f <= last + 1e-6f);
        CHECK(last - f < 0.04f);  // no step: no hard-edged cone (T434)
        last = f;
    }
}

TEST_CASE("volumetric light: the sky body's lobe has no flat top or rim", "[volumetric_light]") {
    // Rising all the way to the body, held under the cap, never a plateau:
    // the hard cap drew a ring round the moon (T434).
    float last = vl::bodyPhase(-1.0f);
    for (float c = -0.995f; c <= 1.0f; c += 0.005f) {
        const float p = vl::bodyPhase(c);
        INFO("cos " << c);
        CHECK(p > last);
        CHECK(p < vl::kBodyLobeCap);
        last = p;
    }
    // Sideways and behind small beside the forward lobe.
    CHECK(vl::bodyPhase(0.0f) < 0.3f);
    CHECK(vl::bodyPhase(1.0f) > 20.0f * vl::bodyPhase(0.0f));
}

TEST_CASE("volumetric light: the stretch of a ray inside a lamp's reach", "[volumetric_light]") {
    const glm::vec3 eye(0.0f);
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    // Straight through: from 5 to 15 yards.
    auto s = vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(10.0f, 0.0f, 0.0f), 5.0f);
    REQUIRE(s.has_value());
    CHECK(s->first == Catch::Approx(5.0f));
    CHECK(s->second == Catch::Approx(15.0f));
    // A wall at 8 yards ends it there.
    s = vl::segmentThroughSphere(eye, forward, 8.0f, glm::vec3(10.0f, 0.0f, 0.0f), 5.0f);
    REQUIRE(s.has_value());
    CHECK(s->second == Catch::Approx(8.0f));
    // A wall before the lamp's reach: nothing.
    CHECK_FALSE(vl::segmentThroughSphere(eye, forward, 4.0f, glm::vec3(10.0f, 0.0f, 0.0f), 5.0f).has_value());
    // Standing inside it: from the eye.
    s = vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(1.0f, 0.0f, 0.0f), 5.0f);
    REQUIRE(s.has_value());
    CHECK(s->first == Catch::Approx(0.0f));
    CHECK(s->second == Catch::Approx(6.0f));
    // Behind the eye, or beside the ray: nothing.
    CHECK_FALSE(vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(-10.0f, 0.0f, 0.0f), 5.0f).has_value());
    CHECK_FALSE(vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(10.0f, 6.0f, 0.0f), 5.0f).has_value());
}

TEST_CASE("volumetric light: the sky body's light only from a body that is up", "[volumetric_light]") {
    // Noon: the sun's, no moon.
    vl::BodyShares s = vl::bodyShares(0.8f, 0.5f);
    CHECK(s.sun == Catch::Approx(1.0f));
    CHECK(s.moon == Catch::Approx(0.0f));
    // Night with the moon up: hers alone.
    s = vl::bodyShares(-0.3f, 0.4f);
    CHECK(s.sun == 0.0f);
    CHECK(s.moon == Catch::Approx(1.0f));
    // Night with the moon down too: none.
    s = vl::bodyShares(-0.3f, -0.1f);
    CHECK(s.sun == 0.0f);
    CHECK(s.moon == 0.0f);
    // At the horizon the two hand over and never add past one.
    for (float h = -0.1f; h <= 0.12f; h += 0.01f) {
        s = vl::bodyShares(h, 0.5f);
        INFO("sun at " << h);
        CHECK(s.sun + s.moon <= 1.0f + 1e-5f);
        CHECK(s.sun >= 0.0f);
        CHECK(s.moon >= 0.0f);
    }
    // The forward lobe: whole while the disc is seen, reduced while hidden.
    CHECK(vl::lobeShare(1.0f) == Catch::Approx(1.0f));
    CHECK(vl::lobeShare(0.0f) == Catch::Approx(vl::kHiddenLobe));
    CHECK(vl::lobeShare(2.0f) == Catch::Approx(1.0f));
}

TEST_CASE("volumetric light: the phase function is the mist's", "[volumetric_light]") {
    // Even scattering is 1; forward many times that, sideways less.
    CHECK(vl::phaseHG(0.3f, 0.0f) == Catch::Approx(1.0f));
    CHECK(vl::phaseHG(1.0f, 0.7f) > 10.0f);
    CHECK(vl::phaseHG(0.0f, 0.7f) < 0.5f);
    CHECK(vl::phaseHG(-1.0f, 0.7f) < vl::phaseHG(0.0f, 0.7f));
}

TEST_CASE("volumetric light: thicker air in foggy zones, at dawn and in rain", "[volumetric_light]") {
    // A clear plain at noon.
    CHECK(vl::airFactor(900.0f, 12.0f, 0.0f, 0.0f) == Catch::Approx(1.0f));
    // Duskwood's fog, nearer.
    CHECK(vl::airFactor(525.0f, 12.0f, 0.0f, 0.0f) > 1.5f);
    // Held to a range either way.
    CHECK(vl::airFactor(10.0f, 12.0f, 0.0f, 0.0f) == Catch::Approx(2.0f));
    CHECK(vl::airFactor(99999.0f, 12.0f, 0.0f, 0.0f) == Catch::Approx(0.6f));
    // No fog known: as clear.
    CHECK(vl::airFactor(0.0f, 12.0f, 0.0f, 0.0f) == Catch::Approx(1.0f));
    // Half past six in the morning, and gone by nine.
    CHECK(vl::airFactor(900.0f, 6.5f, 0.0f, 0.0f) == Catch::Approx(1.8f));
    CHECK(vl::airFactor(900.0f, 9.0f, 0.0f, 0.0f) == Catch::Approx(1.0f));
    // Rain and snow.
    CHECK(vl::airFactor(900.0f, 12.0f, 1.0f, 0.0f) == Catch::Approx(2.2f));
    CHECK(vl::airFactor(900.0f, 12.0f, 0.0f, 1.0f) == Catch::Approx(1.8f));
    // All of them at once: held under a milky wall.
    CHECK(vl::airFactor(300.0f, 6.5f, 1.0f, 1.0f) == Catch::Approx(vl::kMaxAirFactor));
}

TEST_CASE("volumetric light: the most important lamps that reach the view", "[volumetric_light]") {
    std::vector<Lamp> lamps(5);
    lamps[0] = {{10.0f, 0.0f, 0.0f}, 5.0f};
    lamps[1] = {{200.0f, 0.0f, 0.0f}, 5.0f};  // too far
    lamps[2] = {{20.0f, 0.0f, 0.0f}, 0.0f};   // no reach
    lamps[3] = {{30.0f, 0.0f, 0.0f}, 8.0f};
    lamps[4] = {{40.0f, 0.0f, 0.0f}, 8.0f};
    const auto all = vl::pickLights(lamps, glm::vec3(0.0f), 80.0f, 32);
    CHECK(all == std::vector<size_t>{0, 3, 4});
    // Cut to the count, keeping the order.
    const auto two = vl::pickLights(lamps, glm::vec3(0.0f), 80.0f, 2);
    CHECK(two == std::vector<size_t>{0, 3});
    // A lamp just past the range whose reach comes inside it is kept.
    std::vector<Lamp> edge(1);
    edge[0] = {{85.0f, 0.0f, 0.0f}, 8.0f};
    CHECK(vl::pickLights(edge, glm::vec3(0.0f), 80.0f, 32).size() == 1);
}

TEST_CASE("volumetric light: three Debug Glow sliders, 100 % by default", "[volumetric_light][settings]") {
    // User request 2026-11-27 (VL3): the sun's, the moon's and the lamps'
    // part each as a percent of today's, live.
    struct Slider {
        const char* key;
        const char* fileKey;
        const char* field;
        const char* german;
    };
    const Slider sliders[] = {
        {"dbgvlsun", "dbg_vl_sun", "pendingDbgVlSun", "VL Sonne %"},
        {"dbgvlmoon", "dbg_vl_moon", "pendingDbgVlMoon", "VL Mond %"},
        {"dbgvllights", "dbg_vl_lights", "pendingDbgVlLights", "VL Lichter %"},
    };
    const std::string io = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    for (const Slider& s : sliders) {
        INFO(s.key);
        const auto* r = row(s.key);
        REQUIRE(r != nullptr);
        CHECK(r->kind == wowee::ui::SettingKind::Int);
        CHECK(r->minValue == 0.0f);
        CHECK(r->maxValue == 400.0f);
        CHECK(r->step == 5.0f);
        CHECK(r->defaultValue == 100.0f);
        CHECK(std::string(r->category) == "Debug Glow");
        CHECK(std::string(r->enabledWhen) == "volumetriclight");
        const auto* de = wowee::ui::germanSettingText(s.key);
        REQUIRE(de != nullptr);
        REQUIRE(de->label != nullptr);
        CHECK(std::string(de->label) == s.german);
        CHECK(characters(de->label) <= 24);
        REQUIRE(de->tooltip != nullptr);
        CHECK(std::string(de->tooltip).size() > 40);
        // Saved, loaded clamped to the row's range, bound, applied live.
        CHECK(io.find(std::string("out << \"") + s.fileKey + "=\" << settingsPanel_." + s.field) != std::string::npos);
        CHECK(io.find(std::string("key == \"") + s.fileKey + "\") settingsPanel_." + s.field +
                      " = std::clamp(std::stoi(val), 0, 400)") != std::string::npos);
        CHECK(panel.find(std::string("{.key = \"") + s.key + "\", .asInt = &SettingsPanel::" + s.field + "}") !=
              std::string::npos);
        CHECK(panel.find(std::string("key == \"") + s.key + "\"") != std::string::npos);
    }
    CHECK(panel.find("t.volumetricSun = pct(static_cast<float>(pendingDbgVlSun));") != std::string::npos);
    CHECK(panel.find("t.volumetricMoon = pct(static_cast<float>(pendingDbgVlMoon));") != std::string::npos);
    CHECK(panel.find("t.volumetricLights = pct(static_cast<float>(pendingDbgVlLights));") != std::string::npos);
}

TEST_CASE("volumetric light: 100 % leaves each term as it is, others scale it", "[volumetric_light]") {
    // The slider's percent over 100, as applyLightingTune makes it.
    const float hundred = 100.0f / 100.0f;
    for (const vl::Quality q : {vl::Quality::Low, vl::Quality::High}) {
        const vl::Scatter s = vl::scatterFor(q);
        CHECK(vl::tunedScatter(s.body, hundred) == s.body);
        CHECK(vl::tunedScatter(s.body * vl::kMoonScatterShare, hundred) == s.body * vl::kMoonScatterShare);
        CHECK(vl::tunedScatter(s.lamp, hundred) == s.lamp);
        CHECK(vl::tunedScatter(s.lamp, 0.0f) == 0.0f);
        CHECK(vl::tunedScatter(s.lamp, 4.0f) == Catch::Approx(4.0f * s.lamp));
        CHECK(vl::tunedScatter(s.body, 0.5f) == Catch::Approx(0.5f * s.body));
        CHECK(vl::tunedScatter(s.body, -1.0f) == 0.0f);
    }
    // The renderer scales each term by its own slider only.
    const std::string r = wowee::test::slurp("src/rendering/renderer.cpp");
    CHECK(r.find("tunedScatter(bodyScatter, lightingTune_.volumetricSun)") != std::string::npos);
    CHECK(r.find("tunedScatter(bodyScatter, lightingTune_.volumetricMoon)") != std::string::npos);
    CHECK(r.find("tunedScatter(scatter.lamp, lightingTune_.volumetricLights)") != std::string::npos);
}

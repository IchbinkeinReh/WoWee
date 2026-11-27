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
    // The lamps a fifth of V572's at most.
    CHECK(high.lamp <= 0.03f / 5.0f);
    // The sky body's light counts only lit air with shade beside it
    // (shaftStep): open air lit end to end adds nothing, whichever way one
    // looks (T434).
    CHECK(vl::shaftStep(1.0f, true, 0.0f) == 0.0f);
    // The worst a ray seen from the side can take - every yard of it lit
    // beside shade, the thickest air, a white sun - a faint haze, no wall;
    // with the back to the sun a few steps of 255.
    const float side = high.body * vl::kMaxAirFactor * vl::kSunReach * vl::bodyPhase(0.0f);
    CHECK(side < 0.1f);
    const float back = high.body * vl::kMaxAirFactor * vl::kSunReach * vl::bodyPhase(-1.0f);
    CHECK(back < 0.04f);
    // In clear air, a forest's sideways haze with half of it beside shade:
    // a few steps of 255.
    CHECK(high.body * vl::kSunReach * 0.5f * vl::bodyPhase(0.0f) < 6.0f / 255.0f);
    // U23: toward the sun through a crown, ten yards of lit air beside the
    // shade, clear air: plainly seen (VL2 gave about 1/100 even at 400 %),
    // not a white wall.
    const float shaft = high.body * 10.0f * vl::bodyPhase(1.0f);
    CHECK(shaft > 0.1f);
    CHECK(shaft < 0.4f);
    // VL2's figure for the same, to keep the reason on record: its scatter,
    // its lobe of 0.4 behind leaves and its per-ray weight at half shade.
    const float vl2 = 2.0e-4f * 10.0f * (8.0f * 12.9f / (8.0f + 12.9f)) * 0.4f;
    CHECK(vl2 < 0.01f);
    CHECK(shaft > 15.0f * vl2);
    // The moon's share: her light colour is the night's, already dim.
    CHECK(vl::kMoonScatterShare <= 1.0f);
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
    // U23: a lit step counts as much as the point beside it is shaded, and
    // only when that point is air the eye sees.
    CHECK(vl::shaftStep(1.0f, true, 1.0f) == 1.0f);
    CHECK(vl::shaftStep(1.0f, true, 0.0f) == 0.0f);
    CHECK(vl::shaftStep(0.0f, true, 1.0f) == 0.0f);   // in shade itself
    CHECK(vl::shaftStep(1.0f, false, 1.0f) == 0.0f);  // beside it: ground, wall, off screen
    CHECK(vl::shaftStep(0.5f, true, 1.0f) == Catch::Approx(0.5f));  // the map's fading edge
    CHECK(vl::shaftStep(1.0f, true, 3.0f) == 1.0f);
    // The point beside: air in front of what the screen shows, or sky.
    CHECK(vl::besideInAir(true, false, 10.0f, 20.0f));
    CHECK_FALSE(vl::besideInAir(true, false, 20.0f, 10.0f));  // inside the ground or a wall
    CHECK_FALSE(vl::besideInAir(true, false, 10.0f, 10.05f));  // on its face
    CHECK(vl::besideInAir(true, true, 500.0f, 0.0f));
    CHECK_FALSE(vl::besideInAir(false, true, 10.0f, 0.0f));  // off the screen
    CHECK_FALSE(vl::besideInAir(true, true, -1.0f, 0.0f));   // behind the eye
    // The span across the light: a tenth of the way, two to eight yards.
    CHECK(vl::shaftRadius(0.0f) == Catch::Approx(2.0f));
    CHECK(vl::shaftRadius(50.0f) == Catch::Approx(5.0f));
    CHECK(vl::shaftRadius(500.0f) == Catch::Approx(8.0f));
    float last = 0.0f;
    for (float t = 0.0f; t <= vl::kSunReach; t += 0.5f) {
        const float r = vl::shaftRadius(t);
        CHECK(r >= last);
        last = r;
    }
}

TEST_CASE("volumetric light: the history blends about five frames and starts afresh on a cut",
          "[volumetric_light]") {
    // U23 (user 2026-11-27): the light may be smoothed over the last ~5 frames.
    CHECK(vl::kHistoryWeight == Catch::Approx(0.2f));
    // A light that has stood still is the light.
    const glm::vec3 still(0.3f, 0.2f, 0.1f);
    glm::vec3 h = vl::historyBlend(still, still, true);
    CHECK(h.r == Catch::Approx(still.r));
    // A step from dark to lit: one frame takes a fifth of it, five about two
    // thirds, twenty nearly all.
    h = glm::vec3(0.0f);
    const glm::vec3 lit(1.0f);
    h = vl::historyBlend(h, lit, true);
    CHECK(h.r == Catch::Approx(0.2f));
    for (int i = 1; i < 5; ++i) h = vl::historyBlend(h, lit, true);
    CHECK(h.r == Catch::Approx(1.0f - std::pow(0.8f, 5.0f)));
    CHECK(h.r > 0.6f);
    CHECK(h.r < 0.75f);
    // Noise of a frame's grain (alternating 0 and 2 round 1) is cut to a
    // ninth or so of its swing once the history has settled.
    h = glm::vec3(1.0f);
    float lo = 10.0f, hi = -10.0f;
    for (int i = 0; i < 40; ++i) {
        h = vl::historyBlend(h, glm::vec3(i % 2 == 0 ? 0.0f : 2.0f), true);
        if (i >= 20) {
            lo = std::min(lo, h.r);
            hi = std::max(hi, h.r);
        }
    }
    CHECK(hi - lo < 2.0f * 0.25f);
    // Without a usable history the frame's own light, at once.
    h = vl::historyBlend(glm::vec3(5.0f), lit, false);
    CHECK(h.r == 1.0f);
    // When the history is usable: the last frame drew into the same targets
    // and the camera did not jump.
    CHECK(vl::historyUsable(true, true, 0.0f));
    CHECK(vl::historyUsable(true, true, 3.0f));             // flying fast
    CHECK(vl::historyUsable(true, true, vl::kHistoryCutDistance));
    CHECK_FALSE(vl::historyUsable(true, true, 40.0f));      // a teleport
    CHECK_FALSE(vl::historyUsable(false, true, 0.0f));      // the light was off, a loading screen
    CHECK_FALSE(vl::historyUsable(true, false, 0.0f));      // new size or quality
    // The kept distance: the same surface agrees, a unit walked in front
    // does not, the sky agrees only with the sky.
    CHECK(vl::historyDepthAgrees(20.0f, 20.0f));
    CHECK(vl::historyDepthAgrees(20.0f, 20.8f));
    CHECK_FALSE(vl::historyDepthAgrees(20.0f, 15.0f));
    CHECK(vl::historyDepthAgrees(0.5f, 0.58f));
    CHECK(vl::historyDepthAgrees(vl::kHistorySkyDistance, vl::kHistorySkyDistance));
    CHECK_FALSE(vl::historyDepthAgrees(vl::kHistorySkyDistance, 3000.0f));
    CHECK_FALSE(vl::historyDepthAgrees(20.0f, 0.0f));  // nothing kept
    // A half float holds the sky's distance.
    CHECK(vl::kHistorySkyDistance < 65504.0f);
}

TEST_CASE("volumetric light: the debug view shows faint light ten times as bright", "[volumetric_light]") {
    CHECK(vl::kDebugViewGain == Catch::Approx(10.0f));
    // Nothing is black.
    CHECK(vl::debugViewColor(glm::vec3(0.0f)) == glm::vec3(0.0f));
    // A hundredth of white shows as a tenth.
    CHECK(vl::debugViewColor(glm::vec3(0.01f)).r == Catch::Approx(0.1f));
    // Bright light comes under white without a flat top, its hue kept.
    const glm::vec3 warm(0.2f, 0.1f, 0.05f);
    const glm::vec3 shown = vl::debugViewColor(warm);
    CHECK(shown.r < 1.0f);
    CHECK(shown.r > 0.4f);
    CHECK(shown.g / shown.r == Catch::Approx(0.5f));
    float last = 0.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.002f) {
        const float y = vl::debugViewColor(glm::vec3(x)).r;
        CHECK(y >= last);
        CHECK(y < 1.0f);
        last = y;
    }
    // The composite's shoulder leaves light under its knee as it is.
    CHECK(vl::compositeShoulder(glm::vec3(0.3f)).r == Catch::Approx(0.3f));
}

TEST_CASE("volumetric light: the shaders say what the arithmetic says", "[volumetric_light]") {
    // Canaries: the GLSL mirrors of volumetric_light_math.hpp.
    const std::string march = wowee::test::slurp("assets/shaders/volumetric_light.comp.glsl");
    CHECK(march.find("const float cap = 16.0;") != std::string::npos);
    CHECK(march.find("float p = phaseHG(cosTheta, 0.8);") != std::string::npos);
    CHECK(march.find("return clamp(0.1 * t, 2.0, 8.0);") != std::string::npos);
    CHECK(march.find("return pointDepth < sceneDepth - 0.1;") != std::string::npos);
    CHECK(march.find("shown += v * shade;") != std::string::npos);
    // The march's pattern moves from frame to frame, so the history gathers.
    CHECK(march.find("0.61803399 * float(frameNo)") != std::string::npos);
    // VL2's per-ray weight is gone.
    CHECK(march.find("(share - 0.5) / 0.5") == std::string::npos);
    const float r = vl::shaftRadius(50.0f);
    CHECK(r == Catch::Approx(vl::kShaftRadiusShare * 50.0f));
    CHECK(vl::kShaftRadiusMin == 2.0f);
    CHECK(vl::kShaftRadiusMax == 8.0f);
    CHECK(vl::kBodyLobeG == 0.8f);
    CHECK(vl::kBodyLobeCap == 16.0f);
    CHECK(vl::kInAirMargin == 0.1f);

    const std::string temporal = wowee::test::slurp("assets/shaders/volumetric_light_temporal.comp.glsl");
    CHECK(temporal.find("const float kHistoryWeight = 0.2;") != std::string::npos);
    CHECK(temporal.find("const float kSkyDistance = 60000.0;") != std::string::npos);
    CHECK(temporal.find("return abs(expected - kept) <= 0.05 * max(expected, kept) + 0.1;") != std::string::npos);
    CHECK(temporal.find("history = clamp(history, lo, hi);") != std::string::npos);
    CHECK(vl::kHistorySkyDistance == 60000.0f);

    const std::string composite = wowee::test::slurp("assets/shaders/volumetric_light_composite.frag.glsl");
    CHECK(composite.find("const float knee = 0.4;") != std::string::npos);
    CHECK(composite.find("light *= push.mode.y;") != std::string::npos);
    const std::string cpp = wowee::test::slurp("src/rendering/volumetric_light.cpp");
    CHECK(cpp.find("volumetric_light::kDebugViewGain") != std::string::npos);
    CHECK(cpp.find("volumetric_light::historyUsable(") != std::string::npos);
    CHECK(cpp.find("volumetric_light_temporal.comp.spv") != std::string::npos);
}

TEST_CASE("volumetric light: the debug view is a Debug Glow row for the session only",
          "[volumetric_light][settings]") {
    const auto* r = row("dbgvlonly");
    REQUIRE(r != nullptr);
    CHECK(r->kind == wowee::ui::SettingKind::Bool);
    CHECK(r->defaultValue == 0.0f);
    CHECK(std::string(r->category) == "Debug Glow");
    CHECK(std::string(r->enabledWhen) == "volumetriclight");
    CHECK(std::string(r->tooltip).find("ten") != std::string::npos);
    const auto* de = wowee::ui::germanSettingText("dbgvlonly");
    REQUIRE(de != nullptr);
    REQUIRE(de->label != nullptr);
    CHECK(std::string(de->label) == "VL nur Zusatzlicht");
    CHECK(characters(de->label) <= 24);
    REQUIRE(de->tooltip != nullptr);
    // Says how it is scaled.
    CHECK(std::string(de->tooltip).find("zehnfach") != std::string::npos);
    // Bound, applied live, and never written to the settings file.
    const std::string panel = wowee::test::slurp("src/ui/settings_panel.cpp");
    CHECK(panel.find("{.key = \"dbgvlonly\", .asBool = &SettingsPanel::pendingDbgVlOnly}") != std::string::npos);
    CHECK(panel.find("renderer->setVolumetricLightDebugView(pendingDbgVlOnly);") != std::string::npos);
    CHECK(panel.find("key == \"dbgvlonly\"") != std::string::npos);
    const std::string io = wowee::test::slurp("src/ui/game_screen_minimap.cpp");
    CHECK(io.find("pendingDbgVlOnly") == std::string::npos);
    for (const auto& e : wowee::ui::kSuperUltraDebugValues) CHECK(std::string(e.key) != "dbgvlonly");
    // Drawn after the eye's exposure, under the interface.
    const std::string renderer = wowee::test::slurp("src/rendering/renderer.cpp");
    const size_t exposure = renderer.find("eyeAdaptation_->composite(currentCmd");
    const size_t debug = renderer.find("volumetricLight_ && volumetricLight_->debugViewDrawn(vkCtx->getCurrentFrame())) {");
    const size_t ui = renderer.find("ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), currentCmd);");
    REQUIRE(exposure != std::string::npos);
    REQUIRE(debug != std::string::npos);
    REQUIRE(ui != std::string::npos);
    CHECK(exposure < debug);
    CHECK(debug < ui);
    // The names over heads are world text with depth: under the debug view
    // they ended the light's rays and stood black in it (V594 note), so both
    // places that draw them leave them out while it is shown.
    size_t names = 0, guarded = 0;
    for (size_t at = renderer.find("worldText_->render("); at != std::string::npos;
         at = renderer.find("worldText_->render(", at + 1)) {
        ++names;
        const size_t line = renderer.rfind('\n', at);
        if (renderer.substr(line, at - line).find("!volumetricLightDebugViewShown()") != std::string::npos) ++guarded;
    }
    CHECK(names == 2);
    CHECK(guarded == names);
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
    CHECK(vl::coverageFade(1.0f) == Catch::Approx(0.0f).margin(1e-6));
    CHECK(vl::coverageFade(1.5f) == Catch::Approx(0.0f).margin(1e-6));
    // Exactly nothing at the edge, on every compiler (macOS CI at 726e08daf).
    CHECK(vl::coverageFade(1.0f) == 0.0f);
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
    CHECK(s->first == Catch::Approx(0.0f).margin(1e-6));
    CHECK(s->second == Catch::Approx(6.0f));
    // Behind the eye, or beside the ray: nothing.
    CHECK_FALSE(vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(-10.0f, 0.0f, 0.0f), 5.0f).has_value());
    CHECK_FALSE(vl::segmentThroughSphere(eye, forward, 100.0f, glm::vec3(10.0f, 6.0f, 0.0f), 5.0f).has_value());
}

TEST_CASE("volumetric light: the sky body's light only from a body that is up", "[volumetric_light]") {
    // Noon: the sun's, no moon.
    vl::BodyShares s = vl::bodyShares(0.8f, 0.5f);
    CHECK(s.sun == Catch::Approx(1.0f));
    CHECK(s.moon == Catch::Approx(0.0f).margin(1e-6));
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
    // The forward lobe whole whether the disc is seen or not (U23): behind
    // leaves is where the shafts are.
    const std::string renderer = wowee::test::slurp("src/rendering/renderer.cpp");
    CHECK(renderer.find("lobeShare(") == std::string::npos);
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

TEST_CASE("volumetric light: three Debug Glow sliders, 200 % by default", "[volumetric_light][settings]") {
    // User request 2026-11-27 (VL3): the sun's, the moon's and the lamps'
    // part each as a percent of today's, live. 200 by default (user
    // 2026-11-27), also for a settings file that has none of them.
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
    const std::string header = wowee::test::slurp("include/ui/settings_panel.hpp");
    for (const Slider& s : sliders) {
        INFO(s.key);
        const auto* r = row(s.key);
        REQUIRE(r != nullptr);
        CHECK(r->kind == wowee::ui::SettingKind::Int);
        CHECK(r->minValue == 0.0f);
        CHECK(r->maxValue == 400.0f);
        CHECK(r->step == 5.0f);
        CHECK(r->defaultValue == 200.0f);
        CHECK(std::string(r->category) == "Debug Glow");
        CHECK(std::string(r->enabledWhen) == "volumetriclight");
        // The panel opens there when the file has no value, and Super-Ultra
        // sets the same.
        CHECK(header.find(std::string("int ") + s.field + " = 200;") != std::string::npos);
        bool inPreset = false;
        for (const auto& e : wowee::ui::kSuperUltraDebugValues) {
            if (std::string(e.key) == s.key) {
                inPreset = true;
                CHECK(e.value == 200.0f);
            }
        }
        CHECK(inPreset);
        const auto* de = wowee::ui::germanSettingText(s.key);
        REQUIRE(de != nullptr);
        REQUIRE(de->label != nullptr);
        CHECK(std::string(de->label) == s.german);
        CHECK(characters(de->label) <= 24);
        REQUIRE(de->tooltip != nullptr);
        CHECK(std::string(de->tooltip).size() > 40);
        CHECK(std::string(de->tooltip).find("Standard 200") != std::string::npos);
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
    CHECK(r.find("in.debugView = volumetricLightDebugView_;") != std::string::npos);
    CHECK(r.find("tunedScatter(bodyScatter, lightingTune_.volumetricMoon)") != std::string::npos);
    CHECK(r.find("tunedScatter(scatter.lamp, lightingTune_.volumetricLights)") != std::string::npos);
}

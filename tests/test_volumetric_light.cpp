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
    const float core = 1.6f;  // a fifth of the reach
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
    // The core is held between three quarters of a yard and three yards.
    CHECK(vl::glowFalloff(0.75f, 1.0f) == Catch::Approx(2.0f * std::pow(1.0f - std::pow(0.75f, 4.0f), 2.0f) / 2.0f));
    CHECK(vl::glowFalloff(3.0f, 40.0f) == Catch::Approx(2.0f * std::pow(1.0f - std::pow(3.0f / 40.0f, 4.0f), 2.0f) / 2.0f));
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

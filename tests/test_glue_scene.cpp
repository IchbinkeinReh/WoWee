// The glue screens' background scene (0x004e3620, 0x004e0dd0).
#include <catch_amalgamated.hpp>

#include "rendering/glue_scene.hpp"

#include <string>

namespace gs = wowee::rendering::glue_scene;
using wowee::game::Class;
using wowee::game::Race;

TEST_CASE("a race's scene, a gnome's the dwarf's and a troll's the orc's", "[glue_scene]") {
    const auto warrior = static_cast<uint8_t>(Class::WARRIOR);
    CHECK(std::string(gs::backgroundScene(Race::HUMAN, warrior)) == "UI_Human");
    CHECK(std::string(gs::backgroundScene(Race::UNDEAD, warrior)) == "UI_Scourge");
    CHECK(std::string(gs::backgroundScene(Race::GNOME, warrior)) == "UI_Dwarf");
    CHECK(std::string(gs::backgroundScene(Race::TROLL, warrior)) == "UI_Orc");
    CHECK(std::string(gs::backgroundScene(Race::DRAENEI, warrior)) == "UI_Draenei");
}

TEST_CASE("a death knight stands in its class's scene, whatever its race", "[glue_scene]") {
    const auto dk = static_cast<uint8_t>(Class::DEATH_KNIGHT);
    CHECK(std::string(gs::backgroundScene(Race::HUMAN, dk)) == "UI_DeathKnight");
    CHECK(std::string(gs::backgroundScene(Race::TAUREN, dk)) == "UI_DeathKnight");
}

// The degrees of it went to the renderer as radians: a blood elf showed its back, a human its side.
TEST_CASE("a character on the character screens faces the scene's camera", "[glue_scene]") {
    using wowee::rendering::glue_scene::standsTurned;
    const auto looksAtCamera = [](const glm::vec3& toCamera) {
        const float turned = standsTurned(toCamera, 0.0f);
        const glm::vec2 forward(std::cos(turned), std::sin(turned));
        return glm::dot(forward, glm::normalize(glm::vec2(toCamera)));
    };
    for (const float degrees : {143.6f, 0.0f, -17.7f, -0.8f, 90.0f})
        CHECK(looksAtCamera({std::cos(glm::radians(degrees)), std::sin(glm::radians(degrees)), 0.3f}) ==
              Catch::Approx(1.0f));
    CHECK(standsTurned({1.0f, 0.0f, 0.0f}, wowee::rendering::glue_scene::kCreateFacing) ==
          Catch::Approx(-0.261799f));
}

// GlueParent.lua: RaceLights, CharModelFogInfo; 0x004e3a20.
TEST_CASE("a character screen's scene has its lights and its fog", "[glue_scene]") {
    const auto light = [](const char* scene) {
        const auto l = gs::lighting(scene);
        REQUIRE(l.has_value());
        return *l;
    };
    // One of nothing but ambient: straight down, as no direction is.
    const auto dk = light("UI_DeathKnight");
    CHECK(dk.direction == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(dk.diffuse == glm::vec3(0.0f));
    CHECK(dk.ambient.r == Catch::Approx(99.0f / 255.0f));
    CHECK(dk.ambient.g == Catch::Approx(169.0f / 255.0f));
    // Two lights, the second of twice its colour.
    const auto tauren = light("UI_Tauren");
    CHECK(glm::length(tauren.direction) == Catch::Approx(1.0f));
    CHECK(tauren.direction.x < -0.5f);
    CHECK(tauren.diffuse.r > tauren.diffuse.b);
    // An intensity of 0 has no colour, whatever stands behind it.
    const auto dwarf = light("UI_Dwarf");
    CHECK(dwarf.direction.x == Catch::Approx(-0.88314f).margin(1e-3));
    CHECK(dwarf.diffuse.b == Catch::Approx(2.0f * 194.0f / 255.0f).margin(2e-3));
    CHECK(dwarf.ambient.r == Catch::Approx(77.0f / 255.0f).margin(2e-3));
    for (const Race race : {Race::HUMAN, Race::ORC, Race::DWARF, Race::NIGHT_ELF, Race::UNDEAD, Race::TAUREN, Race::GNOME,
                            Race::TROLL, Race::BLOOD_ELF, Race::DRAENEI})
        CHECK(gs::lighting(gs::backgroundScene(race, 1)).has_value());
    CHECK_FALSE(gs::lighting("").has_value());
    REQUIRE(gs::fog("UI_Scourge"));
    CHECK(gs::fog("UI_Scourge")->farDist == 26.0f);
    CHECK(gs::fog("UI_BloodElf") == nullptr);
}

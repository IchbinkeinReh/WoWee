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

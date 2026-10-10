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

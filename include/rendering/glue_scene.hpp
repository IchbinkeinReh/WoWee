#pragma once

// The glue screens' background scene, Interface\Glues\Models\UI_<name>,
// as GetSelectBackgroundModel (0x004e3620) and GetCreateBackgroundModel
// (0x004e0dd0) name it for GlueXML.

#include "game/character.hpp"

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::rendering::glue_scene {

/// A death knight's scene is its class's (ChrClasses 6, Filename +0x1c);
/// anyone else's its race's ClientFileString (ChrRaces +0x2c), a gnome
/// taking the dwarf's (7 -> 3) and a troll the orc's (8 -> 2). Nothing for
/// a race with no row.
inline const char* backgroundScene(game::Race race, uint8_t classId) {
    if (classId == static_cast<uint8_t>(game::Class::DEATH_KNIGHT)) return "UI_DeathKnight";
    switch (race) {
        case game::Race::HUMAN:     return "UI_Human";
        case game::Race::ORC:       return "UI_Orc";
        case game::Race::DWARF:     return "UI_Dwarf";
        case game::Race::NIGHT_ELF: return "UI_NightElf";
        case game::Race::UNDEAD:    return "UI_Scourge";
        case game::Race::TAUREN:    return "UI_Tauren";
        case game::Race::GNOME:     return "UI_Dwarf";
        case game::Race::TROLL:     return "UI_Orc";
        case game::Race::GOBLIN:    return "UI_Goblin";
        case game::Race::BLOOD_ELF: return "UI_BloodElf";
        case game::Race::DRAENEI:   return "UI_Draenei";
    }
    return nullptr;
}

/// How the character is turned, in radians: toward the scene's camera - the scene's stand, its
/// attachment 0, is turned there - and by SetCharacterSelectFacing's or SetCharacterCreateFacing's
/// degrees.
[[nodiscard]] inline float standsTurned(const glm::vec3& toCamera, float facingDegrees) {
    return std::atan2(toCamera.y, toCamera.x) + glm::radians(facingDegrees);
}
/// charactercreate.lua: as the screen is shown, and at every other race.
inline constexpr float kCreateFacing = -15.0f;
/// charactercreate.lua, characterselect.lua: CHARACTER_ROTATION_CONSTANT, a pixel of dragging.
inline constexpr float kDegreesPerPixel = 0.6f;

}  // namespace wowee::rendering::glue_scene

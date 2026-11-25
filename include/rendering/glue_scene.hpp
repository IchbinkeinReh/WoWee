#pragma once

// The glue screens' background scene, Interface\Glues\Models\UI_<name>,
// as GetSelectBackgroundModel (0x004e3620) and GetCreateBackgroundModel
// (0x004e0dd0) name it for GlueXML.

#include "game/character.hpp"
#include "rendering/model_lights.hpp"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

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

/// GlueParent.lua's RaceLights: what SetLighting gives the scene, the character and its pet
/// (AddLight, AddCharacterLight, AddPetLight) - directional, with an intensity to each colour.
struct RaceLight {
    const char* scene;
    glm::vec3 travels;
    float ambientIntensity;
    glm::vec3 ambient;
    float diffuseIntensity;
    glm::vec3 diffuse;
};
inline constexpr RaceLight kRaceLights[] = {
    {"UI_Human", {0.0f, 0.0f, -1.0f}, 1.0f, {0.27f, 0.27f, 0.27f}, 1.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_Human", {-0.45756075f, -0.58900136f, -0.66611975f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.19882353f, 0.34921569f, 0.43588236f}},
    {"UI_Human", {-0.64623469f, 0.57582057f, -0.50081086f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.52196085f, 0.44f, 0.29764709f}},
    {"UI_Orc", {0.0f, 0.0f, -1.0f}, 1.0f, {0.15f, 0.15f, 0.15f}, 1.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_Orc", {-0.74919f, 0.35208f, -0.56103f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.44706f, 0.5451f, 0.73725f}},
    {"UI_Orc", {0.53162f, -0.8434f, 0.0778f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.55f, 0.338625f, 0.148825f}},
    {"UI_Dwarf", {0.0f, 0.0f, -1.0f}, 1.0f, {0.3f, 0.3f, 0.3f}, 0.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_Dwarf", {-0.88314f, 0.42916f, -0.18945f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.44706f, 0.67451f, 0.760785f}},
    {"UI_Tauren", {-0.48073f, 0.71827f, -0.50297f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.65f, 0.397645f, 0.2727f}},
    {"UI_Tauren", {-0.49767f, -0.78677f, 0.36513f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.6f, 0.47059f, 0.32471f}},
    {"UI_Scourge", {0.0f, 0.0f, -1.0f}, 1.0f, {0.2f, 0.2f, 0.2f}, 1.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_NightElf", {0.0f, 0.0f, -1.0f}, 1.0f, {0.0902f, 0.0902f, 0.1702f}, 1.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_Draenei", {0.61185f, 0.62942f, -0.47903f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.56941f, 0.52f, 0.6f}},
    {"UI_Draenei", {-0.64345f, -0.31052f, -0.69968f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.60941f, 0.60392f, 0.7f}},
    {"UI_Draenei", {-0.46481f, -0.1432f, 0.87376f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.5835f, 0.48941f, 0.6f}},
    {"UI_BloodElf", {-0.82249f, -0.54912f, -0.14822f}, 1.0f, {0.0f, 0.0f, 0.0f}, 2.0f, {0.581175f, 0.50588f, 0.42588f}},
    {"UI_BloodElf", {0.0f, 0.0f, -1.0f}, 1.0f, {0.60392f, 0.6149f, 0.7f}, 1.0f, {0.0f, 0.0f, 0.0f}},
    {"UI_BloodElf", {0.02575f, 0.86518f, -0.50081f}, 1.0f, {0.0f, 0.0f, 0.0f}, 1.0f, {0.59137f, 0.51745f, 0.63471f}},
    {"UI_DeathKnight", {0.0f, 0.0f, -1.0f}, 1.0f, {0.38824f, 0.66353f, 0.76941f}, 1.0f, {0.0f, 0.0f, 0.0f}},
};

/// They take the place of the scene model's own directional lights (0x004e3a20, 0x004e2730).
[[nodiscard]] inline std::optional<model_lights::Lighting> lighting(std::string_view scene) {
    // As 0x00960a10 reads a colour: in bytes (0x0061be50), and only with an intensity above 0.
    const auto colour = [](float intensity, const glm::vec3& c) {
        if (intensity <= 0.0f) return glm::vec3(0.0f);
        return glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f) / 255.0f * intensity;
    };
    model_lights::Sum sum;
    bool any = false;
    for (const RaceLight& l : kRaceLights) {
        if (scene != l.scene) continue;
        any = true;
        sum.addAmbient(colour(l.ambientIntensity, l.ambient));
        sum.addDirectional(colour(l.diffuseIntensity, l.diffuse), l.travels);
    }
    if (!any) return std::nullopt;
    return sum.merged();
}

/// CharModelFogInfo, which fogs the scene and not who stands in it (0x004e3c10 sets +0x1b5 for
/// the background's set alone); from 0 to `farDist`.
struct SceneFog {
    const char* scene;
    glm::vec3 color;
    float farDist;
};
inline constexpr SceneFog kFog[] = {
    {"UI_Human", {0.8f, 0.65f, 0.73f}, 222.0f},     {"UI_Orc", {0.5f, 0.5f, 0.5f}, 270.0f},
    {"UI_Dwarf", {0.85f, 0.88f, 1.0f}, 500.0f},     {"UI_NightElf", {0.25f, 0.22f, 0.55f}, 611.0f},
    {"UI_Tauren", {1.0f, 0.61f, 0.42f}, 153.0f},    {"UI_Scourge", {0.0f, 0.22f, 0.22f}, 26.0f},
};
[[nodiscard]] inline const SceneFog* fog(std::string_view scene) {
    for (const SceneFog& f : kFog)
        if (scene == f.scene) return &f;
    return nullptr;
}

}  // namespace wowee::rendering::glue_scene

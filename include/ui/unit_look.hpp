#pragma once

/// What a unit looks like in a portrait or a model frame: a copy of what it is
/// in the world (0x0081f970) - a character, dressed, or the model of the
/// creature its display names.

#include "game/character.hpp"
#include "game/guild_emblem.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wowee::ui {

struct UnitLook {
    /// A character, by what follows up to `tabardDesigner`; else `model`.
    bool character = false;
    uint8_t race = 0;
    uint8_t gender = 0;        ///< game::Gender
    bool femaleModel = false;
    /// A death knight's eyes glow (0x004ed900); 0 where it is not known.
    uint8_t classId = 0;
    uint32_t appearance = 0;   ///< skin, face, hair style, hair colour, from the bottom up
    uint8_t facialFeatures = 0;
    std::vector<game::EquipmentItem> worn;
    /// An NPC's skin, already composited with what it wears.
    std::string bakedSkin;
    /// The guild's design for a guild tabard (0x006db510).
    std::optional<game::GuildEmblem> emblem;
    /// The tabard designer is open (0x006d2900): a tabard and the design,
    /// whatever is worn.
    bool tabardDesigner = false;

    std::string model;
    std::vector<std::pair<uint32_t, std::string>> skins;   ///< by M2 texture type
    std::optional<std::array<glm::vec3, 9>> particleColors;   ///< the display's (ParticleColor.dbc)
    uint32_t geosetData = 0;   ///< the display's (core::creatureGeosets)

    [[nodiscard]] bool any() const { return character || !model.empty(); }
    friend bool operator==(const UnitLook&, const UnitLook&) = default;
};

/// Whether a unit is drawn as its character: a player, while its display
/// (UNIT_FIELD_DISPLAYID) is its own (UNIT_FIELD_NATIVEDISPLAYID, 0x00717a20).
/// In a form, a disguise or as a sheep it is the creature its display names.
[[nodiscard]] constexpr bool drawnAsCharacter(bool player, uint32_t display, uint32_t nativeDisplay) {
    return player && (display == 0 || nativeDisplay == 0 || display == nativeDisplay);
}

}  // namespace wowee::ui

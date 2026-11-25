#pragma once

/// What a unit looks like in a portrait or a model frame: a copy of what it is
/// in the world (0x0081f970) - a character, dressed, or the model of the
/// creature its display names.

#include "game/character.hpp"
#include "game/guild_emblem.hpp"

#include <algorithm>
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

    [[nodiscard]] bool any() const { return character || !model.empty(); }
    friend bool operator==(const UnitLook&, const UnitLook&) = default;
};

/// Whether a unit is drawn as its character: a player, while its display
/// (UNIT_FIELD_DISPLAYID) is its own (UNIT_FIELD_NATIVEDISPLAYID, 0x00717a20).
/// In a form, a disguise or as a sheep it is the creature its display names.
[[nodiscard]] constexpr bool drawnAsCharacter(bool player, uint32_t display, uint32_t nativeDisplay) {
    return player && (display == 0 || nativeDisplay == 0 || display == nativeDisplay);
}

/// Where a dressing room puts an item (0x005980d0): one of the armour's places, a
/// hand, or nowhere - a ring, a trinket, what cannot be worn.
constexpr int kDressNowhere = -1, kDressMainHand = 15, kDressOffHand = 16;
[[nodiscard]] constexpr int dressPlace(uint8_t inventoryType) {
    switch (inventoryType) {
        case 1: return 0;
        case 3: return 1;
        case 4: return 2;
        case 5: case 20: return 3;
        case 6: return 4;
        case 7: return 5;
        case 8: return 6;
        case 9: return 7;
        case 10: return 8;
        case 19: return 9;
        case 16: return 10;
        case 13: case 17: case 21: case 25: case 26: return kDressMainHand;
        case 14: case 15: case 22: case 23: return kDressOffHand;
        default: return kDressNowhere;
    }
}

/// Whether what the off hand has stays with a weapon in the main hand (0x00597c50).
[[nodiscard]] constexpr bool handsFit(uint8_t mainType, uint8_t offType, bool dualWields) {
    if (mainType != 13 && mainType != 21) return false;
    if (offType == 14 || offType == 23) return true;
    return dualWields && (offType == 13 || offType == 22);
}

/// What a dressing room's figure wears: what its unit does, or nothing once it
/// was undressed, each piece tried on in the place of the one that is there.
/// `Tried` has a displayInfoId, an inventoryType and whether it is for the off hand;
/// `dualWields` is the player's own figure, who can.
template <typename Tried>
[[nodiscard]] std::vector<game::EquipmentItem> wornWithTriedOn(std::vector<game::EquipmentItem> worn, bool undressed,
                                                               const std::vector<Tried>& triedOn, bool dualWields = false) {
    if (undressed) worn.clear();
    if (triedOn.empty()) return worn;
    struct Piece {
        game::EquipmentItem item;
        int place = kDressNowhere;
    };
    constexpr uint8_t kOffHandWeapon = 22;
    std::vector<Piece> pieces;
    const auto at = [&pieces](int place) {
        return std::find_if(pieces.begin(), pieces.end(), [place](const Piece& p) { return p.place == place; });
    };
    for (const auto& item : worn) {
        // The first weapon is the main hand's, a second one the other's; what is worn
        // for shooting is not held beside it.
        int place = item.inventoryType == 15 ? kDressMainHand : dressPlace(item.inventoryType);
        game::EquipmentItem piece = item;
        if (place == kDressMainHand && at(kDressMainHand) != pieces.end()) {
            if (item.inventoryType != 13 && item.inventoryType != 17) continue;
            place = kDressOffHand;
            piece.inventoryType = kOffHandWeapon;
        }
        pieces.push_back({piece, place});
    }
    for (const auto& tried : triedOn) {
        const int place = tried.offHand ? kDressOffHand : dressPlace(tried.inventoryType);
        if (place == kDressNowhere) continue;
        if (place == kDressMainHand) {
            if (const auto off = at(kDressOffHand);
                off != pieces.end() && !handsFit(tried.inventoryType, off->item.inventoryType, dualWields))
                pieces.erase(off);
        } else if (place == kDressOffHand) {
            if (const auto main = at(kDressMainHand);
                main != pieces.end() && !handsFit(main->item.inventoryType, tried.inventoryType, dualWields))
                pieces.erase(main);
        }
        const bool weapon = tried.inventoryType == 13 || tried.inventoryType == 17 || tried.inventoryType == 21;
        const game::EquipmentItem item{
            .displayModel = tried.displayInfoId,
            .inventoryType = place == kDressOffHand && weapon ? kOffHandWeapon : tried.inventoryType,
            .enchantment = 0u};
        if (const auto there = at(place); there != pieces.end()) there->item = item;
        else pieces.push_back({item, place});
    }
    worn.clear();
    for (const Piece& p : pieces) worn.push_back(p.item);
    return worn;
}

}  // namespace wowee::ui

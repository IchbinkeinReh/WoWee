#pragma once

/// Reading what the client's character component reads for its geosets -
/// ItemDisplayInfo's geoset columns, a helmet's HelmetGeosetVisData masks and
/// the face's death-knight flag - for core::characterGeosets
/// (geoset_rules.hpp, 0x004ed900).

#include "core/geoset_rules.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace core {

/// One worn item's ItemDisplayInfo geoset columns (0x004cfd90 copies the
/// row): GeosetGroup[0..2], Flags, and whether it textures body regions 1
/// to 6. Not worn when the display is 0 or has no row.
ItemGeosets itemGeosets(pipeline::AssetManager& assets, uint32_t displayInfoId);

/// The display ids of the worn items the geosets read, by body slot.
struct WornDisplays {
    uint32_t head = 0, shirt = 0, chest = 0, belt = 0, legs = 0, boots = 0,
             gloves = 0, tabard = 0, cape = 0;
};

/// Every worn item's columns, and the chest's flag 0x4 (0x004f2640 case 3).
CharacterEquipmentGeosets equipmentGeosets(pipeline::AssetManager& assets, const WornDisplays& worn,
                                           CharacterGeosetFlags* flags = nullptr);

/// A head item's HelmetGeosetVisData masks (0x004ef0d0): ItemDisplayInfo
/// names a row per gender. None when the item names no row.
std::optional<std::array<uint32_t, 7>> helmetGeosetVisMasks(pipeline::AssetManager& assets,
                                                            uint32_t displayInfoId, uint8_t genderId);

/// Whether the face's CharSections row - section 1, the face variation, the
/// skin colour - has flag 4, the death knight's eyes (0x004ed900 asks
/// 0x004f3ba0).
bool faceHasDeathKnightGlow(pipeline::AssetManager& assets, uint8_t raceId, uint8_t genderId,
                            uint8_t faceId, uint8_t skinId);

/// CharHairGeosets.dbc and CharacterFacialHairStyles.dbc, by
/// appearanceKey(race, sex, variation).
struct AppearanceGeosetTables {
    std::unordered_map<uint32_t, uint32_t> hair;               ///< the Geoset column
    std::unordered_map<uint32_t, FacialGeosetColumns> facial;  ///< columns 3 to 7

    /// 0x004ea050: the row's geoset when above 0, else 1, the bald scalp.
    [[nodiscard]] uint32_t hairGeoset(uint8_t raceId, uint8_t sexId, uint8_t style) const {
        auto it = hair.find(appearanceKey(raceId, sexId, style));
        return it != hair.end() && it->second > 0 ? it->second : 1;
    }
    /// 0x004ea000: the row, if there is one.
    [[nodiscard]] const FacialGeosetColumns* facialColumns(uint8_t raceId, uint8_t sexId,
                                                           uint8_t style) const {
        auto it = facial.find(appearanceKey(raceId, sexId, style));
        return it != facial.end() ? &it->second : nullptr;
    }
};
AppearanceGeosetTables loadAppearanceGeosetTables(pipeline::AssetManager& assets);

/// The class whose eyes glow (0x004ed900: component +0x20 == 6).
constexpr uint8_t kClassDeathKnight = 6;

/// Everything the component does for one character: its defaults from the
/// hair and facial rows, the helmet's masks, the glow and the equipment, as
/// the geosets the model draws.
struct CharacterLook {
    uint8_t raceId = 0, genderId = 0, classId = 0, skinId = 0, faceId = 0;
    uint32_t hairGeoset = 1;                       ///< CharHairGeosets' geoset
    std::optional<FacialGeosetColumns> facial;     ///< CharacterFacialHairStyles
    WornDisplays worn;
};
std::unordered_set<uint16_t> characterLookGeosets(pipeline::AssetManager& assets,
                                                  const CharacterLook& look);

}  // namespace core
}  // namespace wowee

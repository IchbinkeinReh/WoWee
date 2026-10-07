#pragma once

/**
 * geoset_rules.hpp - which of a character model's geosets are drawn.
 *
 * A geoset id is a group and a variant: group * 100 + variant. The client
 * decides them in its character component (CCharacterComponent): one default
 * per group from the customisation and the helmet, and then what the worn
 * items' ItemDisplayInfo GeosetGroup columns add and take away (0x004ed900).
 * Every character path here - the player, other players, dressed NPCs and the
 * preview - asks the same function, so they cannot disagree about a body part.
 */

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace core {

/// Group 15's variant 1: the back with no cloak, which takes the body's
/// texture rather than a cloak's.
constexpr uint16_t kGeosetNoCape = 1501;

/// Key for the (race, sex, variation) maps built from CharHairGeosets.dbc and
/// CharacterFacialHairStyles.dbc.
constexpr uint32_t appearanceKey(uint8_t race, uint8_t sex, uint8_t variation) {
    return (static_cast<uint32_t>(race) << 16) |
           (static_cast<uint32_t>(sex) << 8) |
           static_cast<uint32_t>(variation);
}

// ---------------------------------------------------------------------------
// The client's own choice of a character's geosets (CCharacterComponent).
//
// The component keeps one geoset per group, 0 to 18 (+0x144..+0x18c), which
// the customisation and the helmet set; 0x004ed900 then hides every geoset
// up to 2000, shows the body and those nineteen, and lets the equipment's
// ItemDisplayInfo GeosetGroup columns add to and take from them. Ids above
// 2000 it never touches.
// ---------------------------------------------------------------------------

/// The nineteen per-group geosets, group 0 (the hair) to 18 (the belt).
using CharacterGeosetDefaults = std::array<uint16_t, 19>;

/// As the component starts them (0x004dfda0, at +0x12c of the data it
/// copies to +0x18): the bald scalp 1, then variant 1 of every group except
/// the ears, 702.
constexpr CharacterGeosetDefaults kInitialCharacterGeosets = {
    1, 101, 201, 301, 401, 501, 601, 702, 801, 901,
    1001, 1101, 1201, 1301, 1401, 1501, 1601, 1701, 1801};

/// A CharacterFacialHairStyles row's five geoset columns (3 to 7), in file
/// order. 0x004ee460 adds them to groups 1, 3, 2, 16 and 17.
struct FacialGeosetColumns {
    std::array<uint32_t, 5> column{};
};

/// 0x004ee460 with the head bare: the hair is the CharHairGeosets row's
/// geoset, 1 when the row names none or there is no row (0x004ea050 - its
/// Showscalp column is not read); the facial groups come from the
/// CharacterFacialHairStyles row (0x004ea000), left as they start when
/// there is none; the ears are 702.
inline CharacterGeosetDefaults characterGeosetDefaults(uint32_t hairGeoset,
                                                       const FacialGeosetColumns* facial) {
    CharacterGeosetDefaults d = kInitialCharacterGeosets;
    d[0] = static_cast<uint16_t>(hairGeoset > 0 ? hairGeoset : 1);
    if (facial) {
        // 100 + a column the file leaves at 0 is an id no model carries, and
        // so is one past 0xFFFF (a column of 0xCCCCCCCC); that one is kept
        // as 0, the body, which is drawn anyway.
        auto id = [](uint32_t base, uint32_t v) -> uint16_t {
            const uint32_t sum = base + v;
            return sum > 0xFFFFu ? 0 : static_cast<uint16_t>(sum);
        };
        d[1] = id(100, facial->column[0]);
        d[3] = id(300, facial->column[1]);
        d[2] = id(200, facial->column[2]);
        d[16] = id(1600, facial->column[3]);
        d[17] = id(1700, facial->column[4]);
    }
    d[7] = 702;
    return d;
}

/// 0x004ef0d0: a helmet's HelmetGeosetVisData row (ItemDisplayInfo +0x34
/// for a male, +0x38 for a female) holds seven masks by race bit; each set
/// bit puts its group back to the variant that hides it - the hair to the
/// bald scalp, the three facial groups to x01, the ears to 701, groups 16
/// and 17 to x01.
inline void applyHelmetGeosetVis(CharacterGeosetDefaults& d, const std::array<uint32_t, 7>& masks,
                                 uint8_t raceId) {
    const uint32_t bit = 1u << (raceId & 31);
    if (masks[0] & bit) d[0] = 1;
    if (masks[1] & bit) d[1] = 101;
    if (masks[2] & bit) d[2] = 201;
    if (masks[3] & bit) d[3] = 301;
    if (masks[4] & bit) d[7] = 701;
    if (masks[5] & bit) d[16] = 1601;
    if (masks[6] & bit) d[17] = 1701;
}

/// What one worn item's ItemDisplayInfo row hands the geosets.
struct ItemGeosets {
    bool worn = false;
    std::array<uint32_t, 3> group{};  ///< GeosetGroup[0..2]
    uint32_t flags = 0;               ///< Flags (+0x28)
    /// Whether it textures any of the body regions 1 to 6 - lower arm to
    /// lower leg. Read for the chest only, whose layer (1) carries that mask
    /// at +0x244 (0x004ed900, table 0x009f6a00).
    bool texturesArmsOrBody = false;
};

/// The worn items the geosets read, by the component's slots.
struct CharacterEquipmentGeosets {
    ItemGeosets shirt, chest, belt, legs, boots, gloves, tabard, cape;
};

/// The component's flags +8: 0x20 a chest whose display has flag 0x4
/// (0x004f2640 case 3) - its legs are its own - and 0x40, which this
/// client never sets.
struct CharacterGeosetFlags {
    bool chestOwnsLegs = false;
    bool flag40 = false;
};

/// 0x004ed900: the geosets the client shows, of those up to 2000, for
/// these defaults and this equipment. `deathKnightGlow` is the class being
/// a death knight or the face's CharSections row having flag 4; it turns
/// group 17 to 1703.
inline std::unordered_set<uint16_t> characterGeosets(const CharacterGeosetDefaults& d,
                                                     bool deathKnightGlow,
                                                     const CharacterEquipmentGeosets& eq,
                                                     const CharacterGeosetFlags& flags = {}) {
    std::unordered_set<uint16_t> shown;
    auto show = [&](uint32_t id) {
        if (id <= 0xFFFFu) shown.insert(static_cast<uint16_t>(id));
    };
    auto hide = [&](uint32_t lo, uint32_t hi) {
        for (auto it = shown.begin(); it != shown.end();) {
            if (*it >= lo && *it <= hi) it = shown.erase(it);
            else ++it;
        }
    };
    show(0);
    for (size_t i = 0; i < d.size(); ++i) {
        show(i == 17 && deathKnightGlow ? 1703 : d[i]);
    }

    // Hands: the gloves' forearms, or else the chest's sleeves.
    if (eq.gloves.worn && eq.gloves.group[0] != 0) {
        hide(401, 499);
        show(401 + eq.gloves.group[0]);
    } else if (eq.chest.worn && eq.chest.group[0] != 0) {
        show(801 + eq.chest.group[0]);
    }
    // The shirt's sleeves, unless the chest textures the arms or the body.
    if (!(eq.chest.worn && eq.chest.texturesArmsOrBody) && eq.shirt.worn && eq.shirt.group[0] != 0) {
        show(801 + eq.shirt.group[0]);
    }

    bool robe = false, legsRobe = false, tabard = false;
    auto robeHides = [&] {
        hide(501, 599);
        hide(902, 999);
        hide(1100, 1199);
        hide(1300, 1399);
    };
    if (eq.chest.worn && eq.chest.group[2] != 0) {
        // A robe: the chest's skirt over everything below the waist.
        robeHides();
        show(1301 + eq.chest.group[2]);
        robe = true;
    } else if (eq.legs.worn && eq.legs.group[2] != 0 && !flags.chestOwnsLegs) {
        robeHides();
        show(1301 + eq.legs.group[2]);
        legsRobe = true;
    } else {
        if (eq.boots.worn && eq.boots.group[0] != 0) {
            hide(501, 599);
            show(901);
            show(501 + eq.boots.group[0]);
        } else if (eq.legs.worn && eq.legs.group[1] != 0 && !flags.chestOwnsLegs) {
            show(901 + eq.legs.group[1]);
        } else {
            show(901);
        }
        if (eq.tabard.worn && eq.tabard.group[0] != 0) {
            show(1201 + eq.tabard.group[0]);
            tabard = true;
        }
    }

    bool toBeltAndCape = false;
    if (flags.flag40) {
        show(1201);
        if (robe) toBeltAndCape = true;
        else if (!legsRobe) show(1202);
    } else if (robe) {
        toBeltAndCape = true;
    }
    if (!toBeltAndCape) {
        if (!tabard && eq.shirt.worn && eq.shirt.group[1] != 0) show(1001 + eq.shirt.group[1]);
        if (!flags.chestOwnsLegs && eq.legs.worn && eq.legs.group[0] != 0) {
            const uint32_t g = eq.legs.group[0];
            if (g >= 3) {
                hide(1300, 1399);
                show(1101 + g);
            } else if (!tabard) {
                show(1101 + g);
            }
        }
    }

    if (eq.cape.worn && eq.cape.group[0] != 0) {
        hide(1500, 1599);
        show(1501 + eq.cape.group[0]);
    }
    if (eq.belt.worn && eq.belt.group[0] != 0) {
        hide(1800, 1899);
        show(1801 + eq.belt.group[0]);
    }
    return shown;
}

/// What a model draws of what the component shows: a submesh up to 2000
/// only when shown, and every one above 2000, which the client never hides.
inline std::unordered_set<uint16_t> modelGeosetsShown(const std::unordered_set<uint16_t>& shown,
                                                      const std::vector<uint16_t>& modelIds) {
    std::unordered_set<uint16_t> out;
    for (uint16_t id : modelIds) {
        if (id > 2000 || shown.count(id)) out.insert(id);
    }
    return out;
}

}  // namespace core
}  // namespace wowee

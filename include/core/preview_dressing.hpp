#pragma once

/// How the client dresses the model of its character-select and
/// character-create screens (0x004e0fd0): which items it wears and where
/// each weapon goes. Neither screen shows a head item.

#include "core/weapon_attachment.hpp"

#include <cstdint>
#include <vector>

namespace wowee::core {

struct PreviewItem {
    uint32_t display = 0;
    uint8_t inventoryType = 0;
    uint32_t enchant = 0;
};

struct PreviewWeapon {
    uint32_t display = 0;
    uint32_t enchant = 0;
    uint32_t point = attachment::kNone;
    bool shield = false;  ///< From the Shield folder
};

struct PreviewDress {
    std::vector<PreviewItem> worn;   ///< Armour: geosets, skin, shoulders
    std::vector<PreviewWeapon> held;
};

constexpr uint8_t kClassHunter = 3;
/// Character flag: the cloak hidden (0x004e0fd0 tests +0x170 & 0x800).
constexpr uint32_t kCharacterFlagHideCloak = 0x800;

/// 0x004e0fd0 with a character selected: its 23 enumerated slots, by slot.
/// The head (slot 0) is never put on, nor the cloak (14) when hidden. The
/// weapons are drawn, at no Sheath (0x004eacd0's 0, 0): a hunter holds only
/// its ranged weapon (17), everyone else the main hand (15) and off hand
/// (16) - a shield at the shield point. The ranged weapon goes in the left
/// hand, as no "right-hand ranged" is passed here.
inline PreviewDress characterSelectDress(const std::vector<PreviewItem>& slots, uint8_t classId,
                                         uint32_t characterFlags) {
    PreviewDress d;
    const bool hunter = classId == kClassHunter;
    for (size_t s = 1; s < slots.size() && s <= 22; ++s) {
        const PreviewItem& it = slots[s];
        if (it.display == 0) continue;
        if (s == 14 && (characterFlags & kCharacterFlagHideCloak) != 0) continue;
        if (s == 15 || s == 16 || s == 17) {
            if (hunter ? s != 17 : s == 17) continue;
            const bool shield = it.inventoryType == 14;
            const WeaponSlot slot = s == 15 ? WeaponSlot::MainHand : s == 16 ? WeaponSlot::OffHand : WeaponSlot::Ranged;
            d.held.push_back({.display = it.display,
                              .enchant = it.enchant,
                              .point = weaponAttachmentPoint(slot, 0, false, shield, false),
                              .shield = shield});
            continue;
        }
        d.worn.push_back(it);
    }
    return d;
}

/// 0x004e0fd0 with none selected: the race, class and sex's CharStartOutfit
/// items. No head item (inventory type 1). A hunter holds a bow (15) in the
/// left hand or a gun or crossbow (26) in the right and no melee weapon;
/// everyone else a main hand (13, 17, 21, 0x004ef970) in the right hand, an
/// off hand (22, 0x004ef990) in the left and a shield (14, 0x004ef9b0) at
/// the shield point. The rest is armour for 0x004f29c0.
inline PreviewDress characterCreateDress(const std::vector<PreviewItem>& outfit, uint8_t classId) {
    PreviewDress d;
    const bool hunter = classId == kClassHunter;
    for (const PreviewItem& it : outfit) {
        if (it.display == 0 || it.inventoryType == 1) continue;
        switch (it.inventoryType) {
            case 15:
            case 26:
                if (hunter) {
                    d.held.push_back({.display = it.display,
                                      .point = weaponAttachmentPoint(WeaponSlot::Ranged, 0, false, false,
                                                                     rangedInRightHand(it.inventoryType))});
                }
                continue;
            case 13:
            case 17:
            case 21:
                if (!hunter) {
                    d.held.push_back({.display = it.display,
                                      .point = weaponAttachmentPoint(WeaponSlot::MainHand, 0, false, false, false)});
                }
                continue;
            case 22:
                if (!hunter) {
                    d.held.push_back({.display = it.display,
                                      .point = weaponAttachmentPoint(WeaponSlot::OffHand, 0, false, false, false)});
                }
                continue;
            case 14:
                d.held.push_back({.display = it.display,
                                  .point = weaponAttachmentPoint(WeaponSlot::OffHand, 0, false, true, false),
                                  .shield = true});
                continue;
            case 23:
            case 25:
                continue;  // 0x004f29c0 has nothing for these
            default:
                d.worn.push_back(it);
                continue;
        }
    }
    return d;
}

}  // namespace wowee::core

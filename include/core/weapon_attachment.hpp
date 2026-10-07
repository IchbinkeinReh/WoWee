#pragma once

/// Where the client hangs a weapon on a character model (0x004eacd0): in a
/// hand, or put away at the sheath point its item's Sheath names.

#include <cstdint>

namespace wowee::core {

/// M2 attachment ids the weapon code uses.
namespace attachment {
constexpr uint32_t kShield = 0;
constexpr uint32_t kHandRight = 1;
constexpr uint32_t kHandLeft = 2;
constexpr uint32_t kShoulderRight = 5;
constexpr uint32_t kShoulderLeft = 6;
constexpr uint32_t kSheathMainHand = 26;
constexpr uint32_t kSheathOffHand = 27;
constexpr uint32_t kSheathShield = 28;
constexpr uint32_t kLargeWeaponLeft = 30;
constexpr uint32_t kLargeWeaponRight = 31;
constexpr uint32_t kHipWeaponLeft = 32;
constexpr uint32_t kHipWeaponRight = 33;
constexpr uint32_t kNone = 0xFFFFFFFFu;
}  // namespace attachment

/// The three slots a weapon is drawn from.
enum class WeaponSlot : uint8_t { MainHand, OffHand, Ranged };

/// 0x004eacd0. The main hand - and a ranged weapon held in the right hand,
/// a gun, crossbow, wand or thrown weapon (0x0072b7f0: inventory type 25 or
/// 26) - goes in the right hand, its Sheath 1 putting it at 26, 2 at 30,
/// 3 at 32 and 4 at 28. The off hand and a bow go in the left, put away at
/// 27, 31, 33 or 28; a shield in the off hand is held at 0. Any other
/// Sheath has nowhere to go put away: not drawn at all.
constexpr uint32_t weaponAttachmentPoint(WeaponSlot slot, uint32_t sheath, bool sheathed,
                                         bool shield, bool rangedInRightHand) {
    const bool right = slot == WeaponSlot::MainHand ||
                       (slot == WeaponSlot::Ranged && rangedInRightHand);
    if (!sheathed) {
        if (right) return attachment::kHandRight;
        if (slot == WeaponSlot::OffHand && shield) return attachment::kShield;
        return attachment::kHandLeft;
    }
    switch (sheath) {
        case 1: return right ? attachment::kSheathMainHand : attachment::kSheathOffHand;
        case 2: return right ? attachment::kLargeWeaponLeft : attachment::kLargeWeaponRight;
        case 3: return right ? attachment::kHipWeaponLeft : attachment::kHipWeaponRight;
        case 4: return attachment::kSheathShield;
        default: return attachment::kNone;
    }
}

/// 0x0072b7f0: a ranged weapon held in the right hand.
constexpr bool rangedInRightHand(uint8_t inventoryType) {
    return inventoryType == 25 || inventoryType == 26;
}

/// 0x004ef840: a shoulder display's first model (ItemDisplayInfo +4, with
/// its texture +0xc) goes on the left shoulder, 6; its second (+8, +0x10)
/// on the right, 5.
constexpr uint32_t shoulderAttachmentPoint(int modelIndex) {
    return modelIndex == 0 ? attachment::kShoulderLeft : attachment::kShoulderRight;
}

}  // namespace wowee::core

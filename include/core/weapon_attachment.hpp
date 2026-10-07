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

/// A unit's sheath state, UNIT_FIELD_BYTES_2 byte 0 (0x00731f40 compares
/// the old and new): weapons put away, melee drawn, ranged drawn.
enum class SheathState : uint8_t { Unarmed = 0, Melee = 1, Ranged = 2 };

/// What a unit's weapon slot is, from its item: Sheath, InventoryType and,
/// for the main hand, class and subclass.
struct UnitWeaponItem {
    uint32_t sheath = 0;
    uint8_t inventoryType = 0;
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
};

/// 0x0072dbc0 (and 0x00731f40 for the ranged slot on a change of state):
/// where a unit's weapon goes in a sheath state, or attachment::kNone.
/// - The main and off hands are put away when the state is unarmed or
///   ranged, held when it is melee.
/// - The ranged weapon is only on the unit in the ranged state, held - or
///   put away at its point when the state has just gone from ranged to
///   melee (0x0072b7f0 with 1); a later dressing takes it off again.
/// - A creature (not a player) whose main hand is a two-handed weapon -
///   class 2, subclass 1, 5, 6, 8, 10, 12, 17 or 20 - shows no off hand.
constexpr uint32_t unitWeaponPoint(WeaponSlot slot, const UnitWeaponItem& item, SheathState state,
                                   bool rangedJustPutAway, bool isPlayer,
                                   const UnitWeaponItem& mainHand) {
    const bool shield = item.inventoryType == 14;
    const bool rangedRight = rangedInRightHand(item.inventoryType);
    if (slot == WeaponSlot::Ranged) {
        if (state == SheathState::Ranged) {
            return weaponAttachmentPoint(slot, item.sheath, false, shield, rangedRight);
        }
        if (state == SheathState::Melee && rangedJustPutAway) {
            return weaponAttachmentPoint(slot, item.sheath, true, shield, rangedRight);
        }
        return attachment::kNone;
    }
    if (slot == WeaponSlot::OffHand && !isPlayer && mainHand.itemClass == 2) {
        switch (mainHand.subClass) {
            case 1: case 5: case 6: case 8: case 10: case 12: case 17: case 20:
                return attachment::kNone;
            default: break;
        }
    }
    const bool sheathed = state != SheathState::Melee;
    return weaponAttachmentPoint(slot, item.sheath, sheathed, shield, rangedRight);
}

/// 0x004ef840: a shoulder display's first model (ItemDisplayInfo +4, with
/// its texture +0xc) goes on the left shoulder, 6; its second (+8, +0x10)
/// on the right, 5.
constexpr uint32_t shoulderAttachmentPoint(int modelIndex) {
    return modelIndex == 0 ? attachment::kShoulderLeft : attachment::kShoulderRight;
}

}  // namespace wowee::core

#pragma once

/// Where the client hangs a weapon on a character model (0x004eacd0): in a
/// hand, or put away at the sheath point its item's Sheath names.

#include <array>
#include <cstddef>
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

/// What a unit's weapon slot is, from its item: Sheath, InventoryType,
/// class and subclass.
struct UnitWeaponItem {
    uint32_t sheath = 0;
    uint8_t inventoryType = 0;
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
};

/// Before WotLK a creature's slot is a display id
/// (UNIT_VIRTUAL_ITEM_SLOT_DISPLAY) with two UNIT_VIRTUAL_ITEM_INFO fields:
/// the first's bytes class, subclass, material and inventory type, the
/// second's first byte the Sheath.
constexpr UnitWeaponItem virtualItemInfo(uint32_t info0, uint32_t info1) {
    return {.sheath = info1 & 0xFFu,
            .inventoryType = static_cast<uint8_t>(info0 >> 24),
            .itemClass = info0 & 0xFFu,
            .subClass = (info0 >> 8) & 0xFFu};
}

/// A unit's main hand, off hand and ranged item; null for an empty slot.
using UnitWeaponItems = std::array<const UnitWeaponItem*, 3>;

/// UNIT_FIELD_FLAGS 0x200000 (disarmed) and UNIT_FIELD_FLAGS_2 0x80 (off
/// hand disarmed), as 0x0072dbc0 and 0x00718fc0 test them.
constexpr uint32_t kUnitFlagDisarmed = 0x200000;
constexpr uint32_t kUnitFlag2DisarmOffhand = 0x80;

/// 0x00718fc0: a slot's weapon is taken away. Disarmed takes the main hand
/// when it is a weapon (class 2), else the off hand when that is one; the
/// FLAGS_2 bit takes the off hand. Never the ranged slot.
constexpr bool unitSlotDisarmed(WeaponSlot slot, uint32_t unitFlags, uint32_t unitFlags2,
                                const UnitWeaponItems& items) {
    if (slot == WeaponSlot::Ranged) return false;
    if ((unitFlags & kUnitFlagDisarmed) != 0) {
        const bool mainTaken = items[0] && items[0]->itemClass == 2;
        if (slot == WeaponSlot::MainHand) return mainTaken;
        if (!mainTaken && items[1] && items[1]->itemClass == 2) return true;
    }
    return slot == WeaponSlot::OffHand && (unitFlags2 & kUnitFlag2DisarmOffhand) != 0;
}

/// 0x0071d450: AnimationData behaviors of an unarmed attack, parry or
/// ready (16, 20, 25, 117, 118).
constexpr bool unarmedHandsBehavior(uint32_t behavior) {
    switch (behavior) {
        case 0x10: case 0x14: case 0x19: case 0x75: case 0x76: return true;
        default: return false;
    }
}

/// 0x0071d590: the behaviors that count as empty-handed when the main
/// hand has nothing in it.
constexpr bool emptyHandBehavior(uint32_t behavior) {
    switch (behavior) {
        case 10: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
        case 0x17: case 0x18: case 0x1e: case 0x24: case 0x39: case 0x3a: case 0x3b: case 0x55:
        case 0x56: case 0x57: case 0x58: case 0x5f: case 0x75: case 0x76: case 0xaa: case 0xab:
        case 0xac: case 0xad: case 0xae: case 0xaf: case 0xb0: case 0xb1: case 0xb2: case 0xb3:
        case 0xd4:
            return true;
        default: return false;
    }
}

/// The behavior 0x0071d450 and 0x0071d590 read for an animation that
/// AnimationData.dbc does not have.
constexpr uint32_t kNoAnimationBehavior = 0x1fa;

/// 0x00721ed0: the animation the unit plays (AnimationData +0x18, its
/// BehaviorID) decides its off hand through 0x00715d00.
constexpr bool offHandFollowsAnimation(uint32_t behavior, bool hasMainHand) {
    return unarmedHandsBehavior(behavior) || (emptyHandBehavior(behavior) && !hasMainHand);
}

/// 0x00715d00: the sheath state the off hand is dressed by in such an
/// animation. Melee with nothing in the main hand and nothing, or a held
/// item (inventory type 23), in the off hand puts it away; unarmed with a
/// shield in the off hand or a weapon in either hand draws it.
constexpr SheathState offHandAnimationSheath(SheathState state, const UnitWeaponItems& items) {
    const UnitWeaponItem* main = items[0];
    const UnitWeaponItem* off = items[1];
    if (state == SheathState::Melee) {
        if (!main && (!off || off->inventoryType == 23)) return SheathState::Unarmed;
    } else if (state == SheathState::Unarmed) {
        if ((off && off->inventoryType == 14) || (main && main->itemClass == 2) ||
            (off && off->itemClass == 2)) {
            return SheathState::Melee;
        }
    }
    return state;
}

/// What 0x0072dbc0 reads of the unit besides its items.
struct UnitWeaponDress {
    SheathState state = SheathState::Unarmed;
    /// The state has just gone from ranged to melee (0x00731f40).
    bool rangedJustPutAway = false;
    bool isPlayer = false;
    uint32_t unitFlags = 0;   // UNIT_FIELD_FLAGS
    uint32_t unitFlags2 = 0;  // UNIT_FIELD_FLAGS_2
    /// 0x00721ed0 for the animation playing.
    bool offHandFollowsAnimation = false;
};

/// 0x0072dbc0 (and 0x00731f40 for the ranged slot on a change of state):
/// where a unit's weapon goes, or attachment::kNone.
/// - The main and off hands are put away when the state is unarmed or
///   ranged, held when it is melee - the off hand by 0x00715d00's state
///   instead while 0x00721ed0 holds.
/// - The ranged weapon is only on the unit in the ranged state, held,
///   unless the hand it is held in is disarmed (0x00718fc0) - or put away
///   at its point when the state has just gone from ranged to melee
///   (0x0072b7f0 with 1); a later dressing takes it off again.
/// - A disarmed slot shows nothing; nor does an off hand when a creature
///   (not a player) has a two-handed weapon - class 2, subclass 1, 5, 6,
///   8, 10, 12, 17 or 20 - in its main hand.
constexpr uint32_t unitWeaponPoint(WeaponSlot slot, const UnitWeaponItems& items,
                                   const UnitWeaponDress& dress) {
    const UnitWeaponItem* item = items[static_cast<size_t>(slot)];
    if (!item) return attachment::kNone;
    const bool shield = item->inventoryType == 14;
    const bool rangedRight = rangedInRightHand(item->inventoryType);
    if (slot == WeaponSlot::Ranged) {
        if (dress.state == SheathState::Ranged) {
            const WeaponSlot hand = rangedRight ? WeaponSlot::MainHand : WeaponSlot::OffHand;
            if (unitSlotDisarmed(hand, dress.unitFlags, dress.unitFlags2, items)) return attachment::kNone;
            return weaponAttachmentPoint(slot, item->sheath, false, shield, rangedRight);
        }
        if (dress.state == SheathState::Melee && dress.rangedJustPutAway) {
            return weaponAttachmentPoint(slot, item->sheath, true, shield, rangedRight);
        }
        return attachment::kNone;
    }
    bool sheathed = true;
    if (dress.state != SheathState::Ranged) {
        SheathState state = dress.state;
        if (slot == WeaponSlot::OffHand && dress.offHandFollowsAnimation) {
            state = offHandAnimationSheath(state, items);
        }
        sheathed = state == SheathState::Unarmed;
    }
    if (unitSlotDisarmed(slot, dress.unitFlags, dress.unitFlags2, items)) return attachment::kNone;
    if (slot == WeaponSlot::OffHand && !dress.isPlayer && items[0] && items[0]->itemClass == 2) {
        switch (items[0]->subClass) {
            case 1: case 5: case 6: case 8: case 10: case 12: case 17: case 20:
                return attachment::kNone;
            default: break;
        }
    }
    return weaponAttachmentPoint(slot, item->sheath, sheathed, shield, rangedRight);
}

/// 0x006e23a0, the sheath key: unarmed goes to melee with a main or off
/// hand, else to ranged with a ranged weapon the class may draw (ChrClasses
/// +0x24 without 8), else stays; melee goes to ranged with such a weapon,
/// else to unarmed; ranged to unarmed.
constexpr SheathState toggledSheathState(SheathState current, bool hasMainOrOff, bool rangedDrawable) {
    switch (current) {
        case SheathState::Unarmed:
            if (hasMainOrOff) return SheathState::Melee;
            return rangedDrawable ? SheathState::Ranged : SheathState::Unarmed;
        case SheathState::Melee: return rangedDrawable ? SheathState::Ranged : SheathState::Unarmed;
        default: return SheathState::Unarmed;
    }
}

/// 0x004ef840: a shoulder display's first model (ItemDisplayInfo +4, with
/// its texture +0xc) goes on the left shoulder, 6; its second (+8, +0x10)
/// on the right, 5.
constexpr uint32_t shoulderAttachmentPoint(int modelIndex) {
    return modelIndex == 0 ? attachment::kShoulderLeft : attachment::kShoulderRight;
}

}  // namespace wowee::core

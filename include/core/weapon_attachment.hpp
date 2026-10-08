#pragma once

/// Where the client hangs a weapon on a character model (0x004eacd0): in a
/// hand, or put away at the sheath point its item's Sheath names.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

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

/// Where a sheath reach has left each weapon (0x007310a0 holds the main or
/// off hand or puts it away; 0x0072b7f0 the ranged weapon; 0x0072dbc0 takes
/// the ranged weapon off the unit).
enum class RangedShown : uint8_t { None, Away, Held };
struct WeaponsShown {
    bool mainHeld = false;
    bool offHeld = false;
    RangedShown ranged = RangedShown::None;
    bool operator==(const WeaponsShown&) const = default;
};

/// How a state, dressed, shows the weapons.
constexpr WeaponsShown weaponsShownFor(SheathState state) {
    return {.mainHeld = state == SheathState::Melee,
            .offHeld = state == SheathState::Melee,
            .ranged = state == SheathState::Ranged ? RangedShown::Held : RangedShown::None};
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
    /// While a sheath reach plays: where it has left each weapon, in place
    /// of the state's dressing.
    std::optional<WeaponsShown> reachShown;
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
    if (slot == WeaponSlot::Ranged && dress.reachShown) {
        switch (dress.reachShown->ranged) {
            case RangedShown::None: return attachment::kNone;
            case RangedShown::Away: return weaponAttachmentPoint(slot, item->sheath, true, shield, rangedRight);
            case RangedShown::Held: break;
        }
        const WeaponSlot hand = rangedRight ? WeaponSlot::MainHand : WeaponSlot::OffHand;
        if (unitSlotDisarmed(hand, dress.unitFlags, dress.unitFlags2, items)) return attachment::kNone;
        return weaponAttachmentPoint(slot, item->sheath, false, shield, rangedRight);
    }
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
    if (dress.reachShown) {
        sheathed = !(slot == WeaponSlot::MainHand ? dress.reachShown->mainHeld : dress.reachShown->offHeld);
    } else if (dress.state != SheathState::Ranged) {
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

/// The sheath key's reach (0x00736d30 without its immediate argument,
/// 0x00736b60): the arms put the weapons away and draw them with the
/// shoulders' own animations, and each weapon moves when its arm's
/// animation reaches its event or ends. Hand 0 is the right - the main
/// hand, ShoulderR (key bone 3), +0xa38 0x100000, "$SHR" (0x00732650) -
/// and hand 1 the left - the off hand, ShoulderL (2), 0x200000, "$SHL".
struct SheathReach {
    SheathState from = SheathState::Unarmed;  ///< +0xb58
    SheathState to = SheathState::Unarmed;    ///< +0xb5c
    /// +0xa38 0x100000 and 0x200000: the hand's reach is drawing the new
    /// state's weapon, not putting the old one away.
    bool drawing[2] = {false, false};
    WeaponsShown shown;
};

/// A reach each hand's arm starts: Sheath (89), or HipSheath (90) for an
/// item whose Sheath is 3 or 7 (0x88).
struct ReachPlay {
    bool play[2] = {false, false};
    uint32_t animId[2] = {0, 0};
};

constexpr uint32_t kAnimSheath = 89;
constexpr uint32_t kAnimHipSheath = 90;
/// The events the reach's weapons move at (0x00732650).
constexpr uint32_t kEventSheathRight = 0x52485324;  // "$SHR"
constexpr uint32_t kEventSheathLeft = 0x4c485324;   // "$SHL"

constexpr uint32_t reachAnimation(const UnitWeaponItem& item) {
    return ((1u << (item.sheath & 31u)) & 0x88u) != 0 ? kAnimHipSheath : kAnimSheath;
}

namespace detail {
constexpr void playReach(ReachPlay& play, int hand, const UnitWeaponItem& item) {
    play.play[hand] = true;
    play.animId[hand] = reachAnimation(item);
}

/// 0x007367b0 (hand 0) and 0x007368b0 (hand 1): the hand's reach turns to
/// drawing; from anything but unarmed it draws the new state's weapon in
/// that hand - the main or off hand, or a ranged weapon held there, put at
/// its point first (0x0072b7f0 with 1). True when it starts a reach.
constexpr bool reachSecondHalf(SheathReach& reach, ReachPlay& play, int hand, const UnitWeaponItems& items) {
    reach.drawing[hand] = true;
    if (reach.from == SheathState::Unarmed) return false;
    if (reach.to == SheathState::Melee) {
        if (const UnitWeaponItem* item = items[static_cast<size_t>(hand)]) {
            playReach(play, hand, *item);
            return true;
        }
    } else if (reach.to == SheathState::Ranged) {
        const UnitWeaponItem* ranged = items[2];
        if (ranged && rangedInRightHand(ranged->inventoryType) == (hand == 0)) {
            reach.shown.ranged = RangedShown::Away;
            playReach(play, hand, *ranged);
            return true;
        }
    }
    return false;
}

/// 0x007369b0: from unarmed, each hand with something to draw reaches.
/// A ranged weapon is put at its point first and reached for by the hand
/// that holds it.
constexpr void reachFromUnarmed(SheathReach& reach, ReachPlay& play, const UnitWeaponItems& items) {
    if (reach.to == SheathState::Melee) {
        for (int hand = 0; hand < 2; ++hand) {
            if (const UnitWeaponItem* item = items[static_cast<size_t>(hand)]) {
                playReach(play, hand, *item);
                reach.drawing[hand] = true;
            }
        }
    } else if (reach.to == SheathState::Ranged && items[2]) {
        const int hand = rangedInRightHand(items[2]->inventoryType) ? 0 : 1;
        reach.shown.ranged = RangedShown::Away;
        playReach(play, hand, *items[2]);
        reach.drawing[hand] = true;
    }
}
}  // namespace detail

/// 0x00736b60: the reach a change from `from` to `to` starts.
/// - From unarmed, 0x007369b0.
/// - From melee, each hand with a weapon puts it away; an empty hand goes
///   straight to its second half.
/// - From ranged, the hand that holds the ranged weapon puts it away and
///   the other goes straight to its second half; with no ranged weapon,
///   as from unarmed.
constexpr SheathReach beginSheathReach(SheathState from, SheathState to, const UnitWeaponItems& items,
                                       ReachPlay& play) {
    SheathReach reach{.from = from, .to = to};
    reach.shown = weaponsShownFor(from);
    play = {};
    switch (from) {
        case SheathState::Unarmed:
            detail::reachFromUnarmed(reach, play, items);
            break;
        case SheathState::Melee:
            for (int hand = 0; hand < 2; ++hand) {
                if (const UnitWeaponItem* item = items[static_cast<size_t>(hand)]) {
                    detail::playReach(play, hand, *item);
                    reach.drawing[hand] = false;
                } else {
                    detail::reachSecondHalf(reach, play, hand, items);
                }
            }
            break;
        case SheathState::Ranged:
            if (const UnitWeaponItem* ranged = items[2]) {
                const int hand = rangedInRightHand(ranged->inventoryType) ? 0 : 1;
                detail::playReach(play, hand, *ranged);
                reach.drawing[hand] = false;
                detail::reachSecondHalf(reach, play, 1 - hand, items);
            } else {
                detail::reachFromUnarmed(reach, play, items);
            }
            break;
    }
    return reach;
}

/// 0x00732500, at the hand's event ("$SHR", "$SHL") or its reach's end
/// (0x0073bbd0): a drawing hand holds the new state's weapon, any other
/// puts the old state's away - a ranged weapon only by the hand it is held
/// in (a bow by the left, a gun, crossbow, wand or thrown weapon by the
/// right). From ranged, the main hand is then dressed for the new state
/// (0x0072dbc0 with 0).
constexpr void sheathReachSwap(SheathReach& reach, int hand, const UnitWeaponItems& items) {
    const bool draw = reach.drawing[hand] && reach.to != SheathState::Unarmed;
    const SheathState state = draw ? reach.to : reach.from;
    if (state == SheathState::Ranged) {
        bool skip = false;
        if (const UnitWeaponItem* ranged = items[2]) {
            if (ranged->inventoryType == 15) skip = hand == 0;
            else if (rangedInRightHand(ranged->inventoryType)) skip = hand == 1;
        }
        if (!skip) reach.shown.ranged = draw ? RangedShown::Held : RangedShown::Away;
    } else if (hand == 0) {
        reach.shown.mainHeld = draw;
    } else {
        reach.shown.offHeld = draw;
    }
    if (reach.from == SheathState::Ranged) reach.shown.mainHeld = reach.to == SheathState::Melee;
}

/// 0x00737bd0, a hand's reach ended: one that was putting away turns to
/// drawing (0x007367b0, 0x007368b0) and may start a second reach.
constexpr ReachPlay sheathReachEnded(SheathReach& reach, int hand, const UnitWeaponItems& items) {
    ReachPlay play;
    if (!reach.drawing[hand]) detail::reachSecondHalf(reach, play, hand, items);
    return play;
}

/// 0x0071d2e0: the ranged behaviors (AnimationData +0x18) - shooting,
/// loading and the ranged ready stances.
constexpr bool rangedBehavior(uint32_t behavior) {
    switch (behavior) {
        case 0x2e: case 0x31: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e:
        case 0x6f: case 0x70:
            return true;
        default: return false;
    }
}

/// What 0x00738180 reads once the unit's animation has been chosen.
struct AnimationSheathInput {
    SheathState current = SheathState::Unarmed;  ///< the client's own, +0xb5c
    uint32_t animId = 0xFFFFFFFFu;                ///< the model's, -1 for none
    bool animKnown = false;                       ///< AnimationData has a row for it
    uint32_t weaponFlags = 0;                     ///< AnimationData +8
    uint32_t behavior = kNoAnimationBehavior;     ///< AnimationData +0x18
    bool keepWeapons = false;                     ///< its argument
    bool casting = false;                         ///< a spell on the unit, +0xa60
    bool castSheathes = false;  ///< Spell.dbc has it, without Attributes 0x40000
    bool attacking = false;     ///< a melee target, +0xa20
    bool activePlayer = false;
    SheathState field = SheathState::Unarmed;     ///< UNIT_FIELD_BYTES_2 byte 0
};

/// 0x00738180: the state the animation, cast or attack sets (0x00736d30
/// is called with it), or none.
/// - An animation with WeaponFlags 4 puts the weapons away.
/// - In the ranged state only an animation that is no load (105, 106, 112)
///   nor ranged behavior moves it: WeaponFlags 0x10 away, 0x20 melee.
/// - Otherwise a cast puts them away unless its spell has Attributes
///   0x40000; attacking in a combat behavior (0x0071d590) draws melee;
///   WeaponFlags 0x10 away, 0x20 melee; attacking melee; and any unit but
///   the active player takes the field's state.
constexpr std::optional<SheathState> animationSheathState(const AnimationSheathInput& in) {
    const uint32_t wf = in.animKnown ? in.weaponFlags : 0;
    if ((wf & 4) != 0 && !in.keepWeapons) return SheathState::Unarmed;
    if (in.current == SheathState::Ranged) {
        if (in.animId == 105 || in.animId == 106 || in.animId == 112) return std::nullopt;
        if (rangedBehavior(in.behavior) || !in.animKnown) return std::nullopt;
        if ((wf & 0x10) != 0) return SheathState::Unarmed;
        if ((wf & 0x20) != 0) return SheathState::Melee;
        return std::nullopt;
    }
    if (in.casting) {
        if (in.castSheathes) return SheathState::Unarmed;
        return std::nullopt;
    }
    if (in.attacking && emptyHandBehavior(in.behavior)) return SheathState::Melee;
    if ((wf & 0x10) != 0 && !in.keepWeapons) return SheathState::Unarmed;
    if ((wf & 0x20) != 0 || in.attacking) return SheathState::Melee;
    if (!in.activePlayer && in.field != in.current) return in.field;
    return std::nullopt;
}

/// 0x00738180's other result: the unit's +0xa30 0x10000, which with its
/// weapons away and no cast lets a SpellVisual Flags 8 state kit show
/// (0x00720400). Cleared by an animation with WeaponFlags 4, by a cast and
/// by attacking in a combat behavior; set (0x00726090) where the function
/// runs to its end - no other state taken; unchanged otherwise.
constexpr std::optional<bool> animationKitIdle(const AnimationSheathInput& in) {
    const uint32_t wf = in.animKnown ? in.weaponFlags : 0;
    if ((wf & 4) != 0 && !in.keepWeapons) return false;
    if (in.current == SheathState::Ranged) return std::nullopt;
    if (in.casting) return false;
    if (in.attacking && emptyHandBehavior(in.behavior)) return false;
    if ((wf & 0x10) != 0 && !in.keepWeapons) return std::nullopt;
    if ((wf & 0x20) != 0 || in.attacking) return std::nullopt;
    if (!in.activePlayer && in.field != in.current) return std::nullopt;
    return true;
}

/// 0x00737aa0, on a change of UNIT_FIELD_BYTES_2 byte 0: every unit but
/// the active player takes the new state; the active player only when it
/// was in the old one (it has moved on by itself otherwise).
constexpr std::optional<SheathState> fieldSheathChange(SheathState current, SheathState oldField,
                                                       SheathState newField, bool activePlayer) {
    if (activePlayer && current != oldField) return std::nullopt;
    return newField;
}

/// CREATURE_TYPEFLAGS 0x10000000 (the creature cache row's +0xc, which
/// 0x00736d30 reads through the unit's +0x964): the creature keeps the
/// state the server gives it and changes it for nothing else.
constexpr uint32_t kCreatureTypeFlagDoNotSheathe = 0x10000000;

/// What 0x00736d30 reads of the unit besides the state asked for.
struct SheathSetInput {
    SheathState current = SheathState::Unarmed;  ///< +0xb5c
    bool isPlayer = false;            ///< its type mask has 0x10
    bool classMayDrawRanged = false;  ///< a player's ChrClasses Flags (+0x24) without 8
    bool doNotSheathe = false;        ///< kCreatureTypeFlagDoNotSheathe
    bool fromServer = false;          ///< its fourth argument: 0x00737aa0's
    bool offHandFollowsAnimation = false;  ///< 0x00721ed0
    bool hasModel = true;             ///< +0xb4
};

/// 0x00736d30: the state a unit takes when one is asked for, or none.
/// While the animation dresses the off hand (0x00721ed0) the state is
/// 0x00715d00's; a player draws a ranged weapon only when its class may;
/// an unchanged state does nothing; a creature with CREATURE_TYPEFLAGS
/// 0x10000000 takes only the server's; a unit without a model none.
constexpr std::optional<SheathState> sheathStateChange(SheathState requested, const UnitWeaponItems& items,
                                                       const SheathSetInput& in) {
    if (in.offHandFollowsAnimation) requested = offHandAnimationSheath(requested, items);
    if (requested == SheathState::Ranged && in.isPlayer && !in.classMayDrawRanged) return std::nullopt;
    if (requested == in.current) return std::nullopt;
    if (in.doNotSheathe && !in.fromServer) return std::nullopt;
    if (!in.hasModel) return std::nullopt;
    return requested;
}

/// 0x0073f060: a unit whose stand state changes puts its weapons away for
/// anything but standing (0) or sitting in a chair (2).
constexpr bool standStateSheathes(SheathState current, uint8_t standState) {
    return current != SheathState::Unarmed && standState != 0 && standState != 2;
}

/// GAMEOBJECT_TYPE_FISHINGNODE, the bobber.
constexpr uint32_t kGameObjectTypeFishingNode = 17;

/// 0x0073a520, on a change of UNIT_FIELD_CHANNEL_OBJECT: a channel at a
/// fishing bobber draws melee - the fishing pole - unless it is drawn.
constexpr std::optional<SheathState> channelSheathState(SheathState current, uint32_t channelObjectType,
                                                        uint32_t channelSpell) {
    if (channelObjectType != kGameObjectTypeFishingNode || channelSpell == 0) return std::nullopt;
    if (current == SheathState::Melee) return std::nullopt;
    return SheathState::Melee;
}

/// What 0x006dd9e0 (the active player sending a text emote) reads.
struct TextEmoteSheathInput {
    bool emoteKnown = false;    ///< EmotesText's EmoteRef names an Emotes row
    uint32_t emoteFlags = 0;    ///< Emotes +0xc
    uint32_t specProc = 0;      ///< Emotes +0x10
    uint8_t standState = 0;     ///< UNIT_FIELD_BYTES_1 byte 0
    uint32_t moveFlags = 0;     ///< the movement flags
    uint32_t unitFlags = 0;     ///< UNIT_FIELD_FLAGS
    bool charmed = false;       ///< UNIT_FIELD_CHARMEDBY set
    /// On a move spline with Flying (0x2000) and without NO_SPLINE (0x400):
    /// the movement's +0xbc spline, which 0x004f5260 tests.
    bool onFlyingSpline = false;
};

/// 0x004f5410 with its second argument 1: whether the emote may be done
/// now. 0x400 never; 1 only standing; 0x80 not swimming (0x200000);
/// 0x8000 neither on a flying spline (0x004f5260) nor flying (0x2000000);
/// without 0x200 not asleep (3) or dead (7).
constexpr bool textEmoteAllowed(const TextEmoteSheathInput& in) {
    const uint32_t f = in.emoteFlags;
    if ((f & 0x400u) != 0) return false;
    if ((f & 1u) != 0 && in.standState != 0) return false;
    if ((f & 0x80u) != 0 && (in.moveFlags & 0x200000u) != 0) return false;
    if ((f & 0x8000u) != 0 && (in.onFlyingSpline || (in.moveFlags & 0x2000000u) != 0)) return false;
    if ((f & 0x200u) == 0 && (in.standState == 3 || in.standState == 7)) return false;
    return true;
}

/// What 0x006dd9e0 does with a text emote the active player sends.
enum class TextEmoteVerdict : uint8_t {
    Send,                ///< weapons put away, played and sent
    Refused,             ///< nothing, nothing sent
    RefusedWhileMoving,  ///< ERR_NOEMOTEWHILERUNNING (0x14c), nothing sent
};

/// 0x006dd9e0: nothing for a player possessed (UNIT_FIELD_FLAGS 0x1000000)
/// or an emote that may not be done (0x004f5410); a 0x4000 emote while
/// moving (0xc010ff) by a player in control of itself (0x00716710) is
/// refused with ERR_NOEMOTEWHILERUNNING; asleep only an emote whose
/// EmoteSpecProc is 1. Otherwise the weapons are put away and it is sent.
constexpr TextEmoteVerdict textEmoteVerdict(const TextEmoteSheathInput& in) {
    if (!in.emoteKnown || (in.unitFlags & 0x1000000u) != 0) return TextEmoteVerdict::Refused;
    if (!textEmoteAllowed(in)) return TextEmoteVerdict::Refused;
    // 0x00716710: neither confused, fleeing nor held (0xc00004) unless
    // non-attackable (2); not charmed; not server controlled (1).
    const bool held = (in.unitFlags & 2u) == 0 && (in.unitFlags & 0xc00004u) != 0;
    const bool inControl = !held && !in.charmed && (in.unitFlags & 1u) == 0;
    if ((in.emoteFlags & 0x4000u) != 0 && (in.moveFlags & 0xc010ffu) != 0 && inControl) {
        return TextEmoteVerdict::RefusedWhileMoving;
    }
    if (in.standState == 3 && in.specProc != 1) return TextEmoteVerdict::Refused;
    return TextEmoteVerdict::Send;
}

/// What 0x007fa2e0 (a spell's cast beginning on the unit) and 0x0073a6c0
/// (a visual kit on it) read of the spell.
struct SpellSheathInput {
    bool known = false;                 ///< Spell.dbc has it
    uint32_t attributes = 0;            ///< Spell.dbc Attributes
    int32_t missileModel = 0;           ///< SpellVisual +0x20 with +0x1c set
    bool kitWeaponEffect = false;       ///< a kit's Left/RightWeaponEffect
    bool modelHoldsEffects = true;      ///< CreatureModelData +4 without 0x10
};

/// 0x007fa2e0, with 0x0073a6c0 for the kit its visual plays: a ranged
/// spell (Attributes 2) draws the ranged weapon; a kit with a weapon effect
/// puts the weapons away unless the spell has 0x40000; a visual that throws
/// the weapon (missile model -1 or -2) draws melee. The last call wins.
constexpr std::optional<SheathState> spellSheathState(const SpellSheathInput& in) {
    std::optional<SheathState> out;
    if (in.known && (in.attributes & 2u) != 0) out = SheathState::Ranged;
    if (in.kitWeaponEffect && in.modelHoldsEffects && (!in.known || (in.attributes & 0x40000u) == 0)) {
        out = SheathState::Unarmed;
    }
    if (in.missileModel == -1 || in.missileModel == -2) out = SheathState::Melee;
    return out;
}

/// 0x0073a6c0: a kit's LeftWeaponEffect (SpellVisualKit +0x24) hangs at the
/// left hand, 2, its RightWeaponEffect (+0x28) at the right, 1 - the hands
/// of the unit's model, where the weapons were before the kit put them
/// away (0x00744790, 0x006f8c50).
constexpr uint32_t kitWeaponEffectAttachment(bool left) {
    return left ? attachment::kHandLeft : attachment::kHandRight;
}

/// 0x006f8c50 (CEffect::UpdateAttachment): an effect's model scale on a
/// unit - its CreatureModelData AttachedEffectScale (+0x60) times the
/// SpellVisualEffectName Scale (+0x10) - kept within the effect's
/// MinAllowedScale and MaxAllowedScale (+0x14, +0x18) as the attachment
/// point's own scale makes it.
constexpr float kitEffectScale(float attachedEffectScale, float effectScale, float attachmentScale,
                               float minScale, float maxScale) {
    float scale = attachedEffectScale * effectScale;
    const float shown = attachmentScale * scale;
    if (shown > 1e-6f) {
        if (maxScale < shown) scale = scale / shown * maxScale;
        else if (shown < minScale) scale = scale * (minScale / shown);
    }
    return scale;
}

/// 0x004ef840: a shoulder display's first model (ItemDisplayInfo +4, with
/// its texture +0xc) goes on the left shoulder, 6; its second (+8, +0x10)
/// on the right, 5.
constexpr uint32_t shoulderAttachmentPoint(int modelIndex) {
    return modelIndex == 0 ? attachment::kShoulderLeft : attachment::kShoulderRight;
}

}  // namespace wowee::core

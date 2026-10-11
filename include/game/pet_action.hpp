#pragma once

// How a pet action is packed into the CMSG_PET_ACTION `action` field.
//
// It is not a flat list of ids. The field is a pair: the high byte says what
// kind of thing this is, and the low 24 bits say which one. The kinds reuse the
// same small numbers, so 1 means "follow" as a command and "defensive" as a
// reaction, and the two are only told apart by the byte above them.
//
// Four callers each invented their own reading of this - a raw 1..6 with no
// type byte at all, the type and the action swapped, and a dismiss that packed
// action 0 and so told the pet to stay. The bar slots the server sends down are
// already packed correctly and can be sent straight back; everything the client
// builds itself goes through packPetAction.

#include <cstddef>
#include <cstdint>

namespace wowee {
namespace game {
namespace pet {

/// High byte: what kind of action this is.
enum class ActionType : uint8_t {
    Decide   = 0x00,
    Passive  = 0x01,
    Reaction = 0x06,  ///< Passive / defensive / aggressive stance.
    Command  = 0x07,  ///< Stay / follow / attack / dismiss / move-to.
    Disabled = 0x81,  ///< Castable spell, autocast off.
    Enabled  = 0xC1,  ///< Castable spell, autocast on.
};

/// Low 24 bits when the type is Command.
enum Command : uint32_t {
    kStay    = 0,
    kFollow  = 1,
    kAttack  = 2,
    kAbandon = 3,  ///< Dismiss. Not present in the default bar - built by hand.
    kMoveTo  = 4,
};

/// Low 24 bits when the type is Reaction.
enum Reaction : uint32_t {
    kPassive    = 0,
    kDefensive  = 1,
    kAggressive = 2,
};

constexpr uint32_t packPetAction(ActionType type, uint32_t action) {
    return (static_cast<uint32_t>(type) << 24) | (action & 0x00FFFFFFu);
}

constexpr uint32_t petActionId(uint32_t packed) { return packed & 0x00FFFFFFu; }
constexpr ActionType petActionType(uint32_t packed) {
    return static_cast<ActionType>((packed >> 24) & 0xFFu);
}

/// Whether a packed slot holds a castable spell rather than a built-in command
/// or stance. The spell types are the ones that carry an autocast bit.
constexpr bool isPetSpellAction(uint32_t packed) {
    const ActionType t = petActionType(packed);
    return t != ActionType::Command && t != ActionType::Reaction;
}

/// The spell an action-bar reference to a pet slot reads (0x005a80e0): the
/// state byte's low six bits 1 - a castable spell, autocast or not - or
/// 8..17; a command or a stance reads none.
constexpr uint32_t petSlotSpell(uint32_t packed) {
    const uint32_t state = (packed >> 24) & 0x3fu;
    return state == 1 || (state >= 8 && state < 18) ? packed & 0x00FFFFFFu : 0u;
}

/// Whether a pet slot's spell is cast by the pet as the client casts the
/// player's (0x005d4210 -> 0x0080cce0, CMSG_PET_CAST_SPELL) rather than
/// asked of it by CMSG_PET_ACTION: Spell.dbc AttributesEx4 (+0x20) 0x20, or
/// the pet is the active mover - a vehicle the player drives (0x006dd060).
constexpr bool petCastsItself(uint32_t attrEx4, bool petIsMover) { return (attrEx4 & 0x20u) != 0 || petIsMover; }

/// 0x005d4210: a spell no pet is in a state to cast - every pet stunned
/// (UNIT_FIELD_FLAGS 0x40000) unless the spell's AttributesEx5 has 0x8,
/// else every one fleeing (0x800000) unless 0x20000, else every one confused
/// (0x400000) unless 0x40000. Nothing is said.
template <typename Flags>
[[nodiscard]] constexpr bool petsCannotCast(const Flags& petUnitFlags, uint32_t attrEx5) {
    size_t pets = 0, stunned = 0, fleeing = 0, confused = 0;
    for (const uint32_t f : petUnitFlags) {
        ++pets;
        if ((attrEx5 & 0x8u) == 0 && (f & 0x40000u) != 0) ++stunned;
        else if ((attrEx5 & 0x20000u) == 0 && (f & 0x800000u) != 0) ++fleeing;
        else if ((attrEx5 & 0x40000u) == 0 && (f & 0x400000u) != 0) ++confused;
    }
    return pets != 0 && (stunned == pets || fleeing == pets || confused == pets);
}

/// What 0x00802cb0 reads of a spell: its ActiveIconID (+0x218), Effect
/// (+0x11c), EffectApplyAuraName (+0x17c), EffectMiscValue (+0x1b8) and the
/// first EffectMiscValueB (+0x1c4).
struct ToggleSpell {
    uint32_t id = 0;
    uint32_t activeIconId = 0;
    uint32_t effects[3] = {0, 0, 0};
    uint32_t auras[3] = {0, 0, 0};
    uint32_t misc[3] = {0, 0, 0};
    uint32_t miscB0 = 0;
};
/// What it reads of the unit, one other than the player: the entry its
/// UNIT_FIELD_CRITTER guid carries, and the vehicle it rides (0x00747ff0) -
/// its entry and whether the player made it (UNIT_FIELD_CREATEDBY).
struct ToggleUnit {
    uint32_t critterEntry = 0;
    uint32_t vehicleEntry = 0;
    bool vehicleMadeByPlayer = false;
};
inline constexpr uint32_t kEffectSummon = 28;
inline constexpr uint32_t kAuraMounted = 78;
inline constexpr uint32_t kSummonSlotCritter = 5;

/// 0x00802cb0: whether a spell is in effect on a unit, so that its button
/// takes it off rather than casting it. Only a spell with an ActiveIconID,
/// or one that summons a companion - its first effect SUMMON (28) whose
/// SummonProperties row (the first EffectMiscValueB) has Slot (+0x10) 5 -
/// or mounts (first aura 78) (0x008009b0). Then: a SUMMON effect naming the
/// vehicle the unit rides, made by the player; or naming the unit's critter,
/// where that row is a companion's; an aura of the spell on the unit with
/// the positive flag (0x10); or for a mount, an aura on the unit mounting
/// the same creature. `slotOf` gives a SummonProperties row's Slot,
/// `mountOf` an aura spell's first ApplyAuraName and EffectMiscValue.
template <typename SlotOf, typename Auras, typename MountOf>
[[nodiscard]] bool spellInEffectOn(const ToggleSpell& s, const ToggleUnit& u, SlotOf slotOf, const Auras& auras,
                                   MountOf mountOf) {
    const bool companion = s.effects[0] == kEffectSummon && slotOf(s.miscB0) == kSummonSlotCritter;
    if (s.activeIconId == 0 && !companion && s.auras[0] != kAuraMounted) return false;  // 0x008009b0
    for (int k = 0; k < 3; ++k) {
        if (s.effects[k] != kEffectSummon) continue;
        if (u.vehicleEntry != 0 && u.vehicleEntry == s.misc[k] && u.vehicleMadeByPlayer) return true;
        if (slotOf(s.miscB0) == kSummonSlotCritter) return u.critterEntry == s.misc[k];
    }
    for (const auto& aura : auras) {
        if (aura.spellId == s.id && (aura.flags & 0x10u) != 0) return true;
    }
    if (s.auras[0] == kAuraMounted) {
        for (const auto& aura : auras) {
            const auto [auraName, misc] = mountOf(aura.spellId);
            if (auraName == kAuraMounted && misc == s.misc[0]) return true;
        }
    }
    return false;
}

/// 0x007fdf20: a spell with an effect that applies an aura (0x0076be10) -
/// APPLY_AURA (6) or an area aura (35, 65, 119, 128, 129, 143). Taking one
/// off a unit not the player is CMSG_PET_CANCEL_AURA (0x00802f80); any
/// other is not sent.
[[nodiscard]] constexpr bool spellAppliesAura(const uint32_t (&effects)[3]) {
    for (const uint32_t e : effects) {
        if (e == 6 || e == 35 || e == 65 || e == 119 || e == 128 || e == 129 || e == 143) return true;
    }
    return false;
}

/// 0x0080cce0: a spell with AttributesEx6 (+0x28) 0x40000 is cast by its
/// caster's charmer - UNIT_FIELD_CHARMEDBY, else UNIT_FIELD_CREATEDBY.
[[nodiscard]] constexpr uint64_t castingUnit(uint64_t caster, uint32_t attrEx6, uint64_t charmedBy,
                                             uint64_t createdBy) {
    if ((attrEx6 & 0x40000u) == 0) return caster;
    return charmedBy != 0 ? charmedBy : createdBy;
}

/// 0x0080cce0: AttributesEx7 (+0x2c) 0x8 is cast only by a caster with
/// UNIT_FIELD_FLAGS_2 0x40000; else SPELL_FAILED_SPELL_UNAVAILABLE (0x6b).
[[nodiscard]] constexpr bool casterMayCast(uint32_t attrEx7, uint32_t casterFlags2) {
    return (attrEx7 & 0x8u) == 0 || (casterFlags2 & 0x40000u) != 0;
}

/// What PetDismiss does (CGPetInfo::PetDismiss, 0x005d46f0).
enum class DismissAction : uint8_t {
    None,
    /// The player drives its pet, a vehicle, from a seat with Flags
    /// 0x2000000: CMSG_DISMISS_CONTROLLED_VEHICLE (0x0074bb20).
    DismissControlledVehicle,
    /// CMSG_PET_ACTION with COMMAND_ABANDON, and PET_DISMISS_START.
    PetAction,
};

struct DismissContext {
    uint64_t player = 0;
    uint64_t playerCharmedBy = 0;  ///< UNIT_FIELD_CHARMEDBY of the player
    uint64_t farsight = 0;         ///< PLAYER_FARSIGHT
    uint64_t pet = 0;              ///< the first of the pet guids (DAT_00c23500)
    bool petInView = false;
    uint64_t petOwner = 0;         ///< its CHARMEDBY, else its SUMMONEDBY
    uint32_t petUnitFlags = 0;     ///< its UNIT_FIELD_FLAGS
    uint64_t mover = 0;            ///< DAT_00ca1238
    uint64_t playerTransport = 0;  ///< the player's vehicle (its +0x40)
    bool moverIsVehicle = false;   ///< the mover's +0xf5c
    bool seatKnown = false;        ///< a VehicleSeat row for the player's seat
    uint32_t seatFlags = 0;
    /// Its creating spell's SUMMON effect's SummonProperties Flags (0x0071d1f0).
    uint32_t summonPropertiesFlags = 0;
};

/// 0x005d3e80(0): the player, charmed by none and looking through nothing
/// but its pet, has that pet in view - its own, charmed or summoned by it -
/// and not stunned, fleeing or confused (UNIT_FIELD_FLAGS 0x40000, 0x800000,
/// 0x400000, 0x005d3310).
[[nodiscard]] constexpr bool canDismissPet(const DismissContext& c) {
    if (c.player == 0 || c.playerCharmedBy != 0) return false;
    if (c.farsight != 0 && c.farsight != c.pet) return false;
    if (c.pet == 0 || !c.petInView || c.petOwner != c.player) return false;
    return (c.petUnitFlags & (0x40000u | 0x800000u | 0x400000u)) == 0;
}

/// 0x005d46f0.
[[nodiscard]] constexpr DismissAction dismissAction(const DismissContext& c) {
    if (!canDismissPet(c)) return DismissAction::None;
    // The pet is the mover the player rides: its seat says (0x00756ec0).
    if (c.pet == c.mover && c.playerTransport == c.pet) {
        if (!c.moverIsVehicle || !c.seatKnown || (c.seatFlags & 0x2000000u) == 0) return DismissAction::None;
        return DismissAction::DismissControlledVehicle;
    }
    // A summon whose SummonProperties Flags have 0x20 is not sent away.
    if ((c.summonPropertiesFlags & 0x20u) != 0) return DismissAction::None;
    return DismissAction::PetAction;
}

} // namespace pet
} // namespace game
} // namespace wowee

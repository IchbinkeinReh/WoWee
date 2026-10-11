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

#pragma once

// What a unit's model events play, as the client's unit callback plays them.
//
// A unit's model, and the mount model it rides (0x0073d5d0), call the unit
// back with each event its animation passes (0x00824060 -> 0x00734a40 ->
// 0x00732650). Of what that callback does, the sounds of the unit's
// CreatureSoundData:
//
//   $FD1..$FD5   the row's Fidget sound 1..5, once, two yards over the unit
//                (0x007471a0 -> 0x007462e0);
//   $WNG, $WGG   its WingFlap and WingGlide sound, and $FDX its Stand
//                sound for any unit but the active player - each by the
//                slot's chance (0x00747310, 0x00adb5d8), Stand at most once
//                in ten seconds of all units;
//   $BRT, $SCD,  its Birth, SpellCastDirected, Submerge and Submerged
//   $SMG, $SMD   sound, once, where the event is.
//
// The row is the unit's display's: CreatureDisplayInfo SoundID, else its
// CreatureModelData's (+0x974, set by 0x0072d940). The fidgets and the
// slots read it through 0x0071a3f0, which takes the row's
// CreatureSoundDataIDPet in its place while the unit has a
// UNIT_FIELD_PETNUMBER.

#include <cstdint>

namespace wowee::rendering::unit_event {

/// Four characters read as a little-endian word, as the client compares them.
constexpr uint32_t fourCC(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

enum class Kind : uint8_t {
    None,
    /// CreatureSoundData Fidget[index], over the unit (0x007462e0).
    Fidget,
    /// A creature sound slot, by its chance (0x00747310).
    Slot,
    /// A CreatureSoundData column of the display's own row, at the event.
    AtEvent,
};

struct Action {
    Kind kind = Kind::None;
    /// Fidget: 0..4. Slot: the slot. AtEvent: the column.
    uint32_t index = 0;
};

/// The creature sound slots 0x00747310 is asked to play by an event.
constexpr uint32_t kSlotStand = 5;      ///< $FDX
constexpr uint32_t kSlotWingFlap = 7;   ///< $WNG
constexpr uint32_t kSlotWingGlide = 10; ///< $WGG

/// CreatureSoundData columns (3.3.5a, 38 of them).
constexpr uint32_t kColumnFidget0 = 14;
constexpr uint32_t kColumnBirth = 33;
constexpr uint32_t kColumnSpellCastDirected = 34;
constexpr uint32_t kColumnSubmerge = 35;
constexpr uint32_t kColumnSubmerged = 36;
constexpr uint32_t kColumnPetRow = 37;  ///< CreatureSoundDataIDPet
constexpr uint32_t kColumnCount = 38;

/// What an event of a unit's model does with its sounds.
constexpr Action classify(uint32_t eventId) {
    for (uint32_t i = 0; i < 5; ++i) {
        const char name[5] = {'$', 'F', 'D', static_cast<char>('1' + i), 0};
        if (eventId == fourCC(name)) return {Kind::Fidget, i};
    }
    if (eventId == fourCC("$WNG")) return {Kind::Slot, kSlotWingFlap};
    if (eventId == fourCC("$WGG")) return {Kind::Slot, kSlotWingGlide};
    if (eventId == fourCC("$FDX")) return {Kind::Slot, kSlotStand};
    if (eventId == fourCC("$BRT")) return {Kind::AtEvent, kColumnBirth};
    if (eventId == fourCC("$SCD")) return {Kind::AtEvent, kColumnSpellCastDirected};
    if (eventId == fourCC("$SMG")) return {Kind::AtEvent, kColumnSubmerge};
    if (eventId == fourCC("$SMD")) return {Kind::AtEvent, kColumnSubmerged};
    return {};
}

/// The CreatureSoundData column a slot plays (0x007461e0), 0 for none.
constexpr uint32_t slotColumn(uint32_t slot) {
    switch (slot) {
    case 0: return 1;    // Exertion
    case 1: return 2;    // ExertionCritical
    case 2:
    case 13: return 3;   // Injury
    case 3: return 4;    // InjuryCritical
    case 4: return 7;    // Stun
    case 5: return 8;    // Stand
    case 7: return 11;   // WingFlap
    case 8: return 13;   // Alert
    case 9: return 5;    // InjuryCrushingBlow
    case 10: return 12;  // WingGlide
    case 11: return 26;  // JumpStart
    case 12: return 27;  // JumpEnd
    default: return 0;
    }
}

/// The slots' chances, out of 100 (0x00adb5d8).
constexpr uint32_t slotChance(uint32_t slot) {
    constexpr uint32_t kChance[13] = {70, 100, 60, 100, 100, 40, 100, 100, 100, 100, 100, 100, 100};
    return slot < 13 ? kChance[slot] : 0;
}

/// Whether a slot plays for a 32-bit random draw: the draw times 101, its
/// high word, no more than the chance (0x00747310) - so 40 plays 41 times in
/// 101, and 100 always.
constexpr bool slotPlays(uint32_t slot, uint32_t random32) {
    const auto roll = static_cast<uint32_t>((static_cast<uint64_t>(random32) * 101u) >> 32);
    return roll <= slotChance(slot);
}

/// $FDX's Stand sound: at most once in this long, of every unit (0x00ca12d4).
constexpr uint32_t kStandSoundIntervalMs = 10000;
constexpr bool standSoundDue(uint32_t nowMs, uint32_t lastMs) {
    return static_cast<int32_t>(nowMs - lastMs - kStandSoundIntervalMs) >= 0;
}

/// A slot's sound for the active player, with the listener at the
/// character (Sound_ListenerAtCharacter, default 1): where the listener is,
/// at this much of its volume (0x00747310).
constexpr float kListenerAtCharacterVolume = 0.65f;

/// The fidgets: two yards over the unit (0x007462e0).
constexpr float kFidgetHeight = 2.0f;

}  // namespace wowee::rendering::unit_event

#pragma once

/// Whether a unit's aura is shown to the player at all, by what the player can
/// see (0x00724820 asks 0x007fe3e0 before it takes an aura into a unit's slot
/// and plays its state kit).
///
/// An aura that detects stealth or invisibility - and a spell that names an
/// aura vision of its own - is only shown to a player who has that vision:
/// PLAYER_FIELD_BYTES2's high byte (the active player's +0x1008 descriptor,
/// +0x10e7), 0x20 for stealth, 0x40 for the invisibility glow. Elsewhere the
/// unit keeps the aura but shows nothing of it. Silvermoon's Arcane Guardians
/// carry an invisibility detection whose visual is a floating eye; the client
/// never draws it for a player without that vision.
///
/// Pure: tests/test_aura_vision.cpp checks it.

#include <cstdint>

namespace wowee::game::aura_vision {

/// SPELL_AURA_MOD_STEALTH_DETECT and SPELL_AURA_MOD_INVISIBILITY_DETECT.
inline constexpr uint32_t kAuraStealthDetect = 17;
inline constexpr uint32_t kAuraInvisibilityDetect = 19;
/// The vision bits they ask for.
inline constexpr uint32_t kVisionStealth = 0x20;
inline constexpr uint32_t kVisionInvisibility = 0x40;

/// The vision a spell's aura asks for (0x007fe3e0): its RequiredAuraVision
/// as a bit (1 << (n - 1)), the stealth bit for any effect that detects
/// stealth, and the invisibility bit for one that detects the ordinary
/// invisibility (misc value 0) or type 10. 0 asks for none.
inline uint32_t requiredVision(uint32_t requiredAuraVision, const uint32_t auras[3], const uint32_t misc[3]) {
    uint32_t mask = requiredAuraVision >= 1 && requiredAuraVision <= 32 ? 1u << (requiredAuraVision - 1) : 0u;
    for (int e = 0; e < 3; ++e) {
        if (auras[e] == kAuraStealthDetect) mask |= kVisionStealth;
        if (auras[e] == kAuraInvisibilityDetect && (misc[e] == 0 || misc[e] == 10)) mask |= kVisionInvisibility;
    }
    return mask;
}

/// Whether the player sees an aura asking for `required`, with `vision`
/// (PLAYER_FIELD_BYTES2 >> 24).
inline bool visibleTo(uint32_t required, uint32_t vision) {
    return required == 0 || (required & vision) != 0;
}

}  // namespace wowee::game::aura_vision

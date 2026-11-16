#pragma once

/// A destructible building's (game object type 33) answer to a change of its
/// GAMEOBJECT_FLAGS, as the client's type handler takes it (0x0070ed00 hands
/// 0x0070e6a0 the bits that changed). Arithmetic only: the caller reads the
/// world and moves the models.

#include <cstdint>

namespace wowee::game::destructible {

/// GAMEOBJECT_FLAGS 0x100: the building was hit.
inline constexpr uint32_t kFlagHit = 0x100u;
/// The bits 0x0070e6a0 reacts to: the hit and the three state bits.
inline constexpr uint32_t kFlagsWatched = 0xf00u;

/// The state the flags give: 0 intact, 1 damaged, 2 destroyed, 3 rebuilding
/// ((flags >> 8 & 0xe) >> 1).
[[nodiscard]] constexpr uint32_t stateOfFlags(uint32_t flags) {
    return ((flags >> 8) & 0xeu) >> 1;
}

/// 0x0070e6a0: the current state's impact doodad set plays (Custom0, then
/// Custom1, then Stand) when the hit bit changed, either way.
[[nodiscard]] constexpr bool impactPlays(uint32_t changedBits) {
    return (changedBits & kFlagHit) != 0;
}

/// 0x0070e6a0 then runs the state change (0x0070dfa0) whenever any of the
/// watched bits changed.
[[nodiscard]] constexpr bool stateChecked(uint32_t changedBits) {
    return (changedBits & kFlagsWatched) != 0;
}

/// 0x0070dfa0: the building worsened under the player - a look before
/// this one, the new state damaged or destroyed and worse than the old, the
/// player in no vehicle (0x0074b8b0) and not flying (movement flag
/// 0x2000000), standing inside the old look's bounds in the building's own
/// space (0x006cb930). The player's placed fall is queued then (0x007189f0),
/// so it drops through the broken floor.
[[nodiscard]] constexpr bool playerFallsThrough(uint32_t oldState, uint32_t newState, bool hadLook,
                                                bool inVehicle, uint32_t moveFlags, bool insideOldBounds) {
    return hadLook && oldState < newState && newState != 3 && newState != 0 && !inVehicle &&
           (moveFlags & 0x2000000u) == 0 && insideOldBounds;
}

}  // namespace wowee::game::destructible

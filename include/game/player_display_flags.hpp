#pragma once

#include <array>
#include <cstdint>

// What a player's PLAYER_FLAGS take off their model: with 0x400 the head
// slot and with 0x800 the back slot are not dressed, for every player, not
// only the one being played (CGPlayer_C, 0x006e0fd0 on a flags change undoes
// or redoes slot 0 and 14; 0x006e0960 skips them when dressing).

namespace wowee::game::player_display {

inline constexpr uint32_t kHideHelm = 0x00000400;
inline constexpr uint32_t kHideCloak = 0x00000800;
inline constexpr int kHeadSlot = 0;
inline constexpr int kBackSlot = 14;

/// The display bits the flags carry, for noticing when they change.
inline uint32_t hideBits(uint32_t playerFlags) { return playerFlags & (kHideHelm | kHideCloak); }

/// Clears the slots the flags hide.
inline void applyHidden(uint32_t playerFlags, std::array<uint32_t, 19>& displayIds,
                        std::array<uint8_t, 19>& inventoryTypes) {
    if (playerFlags & kHideHelm) {
        displayIds[kHeadSlot] = 0;
        inventoryTypes[kHeadSlot] = 0;
    }
    if (playerFlags & kHideCloak) {
        displayIds[kBackSlot] = 0;
        inventoryTypes[kBackSlot] = 0;
    }
}

}  // namespace wowee::game::player_display

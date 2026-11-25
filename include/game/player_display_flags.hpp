#pragma once

#include "game/character.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

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

/// The flags from the two settings they carry.
inline uint32_t hideBits(bool helmShown, bool cloakShown) {
    return (helmShown ? 0u : kHideHelm) | (cloakShown ? 0u : kHideCloak);
}

/// What the player's own model wears in the character window, a portrait or
/// the dressing room: the model of the unit in the world, duplicated with all
/// that hangs on it (0x005975e0), or for a dressing room dressed again slot by
/// slot, the head and the back not where the flags hide them (0x00598b80).
/// `displayIds` as resolveOtherPlayerEquipment leaves them where that knew
/// any; before, the character list's, which is by slot as well.
inline std::vector<EquipmentItem> wornByPlayer(bool known, const std::array<uint32_t, 19>& displayIds,
                                               const std::array<uint8_t, 19>& inventoryTypes,
                                               const std::vector<EquipmentItem>& listed, uint32_t playerFlags) {
    std::vector<EquipmentItem> worn;
    if (known) {
        for (size_t slot = 0; slot < displayIds.size(); ++slot) {
            if (displayIds[slot] == 0) continue;
            worn.push_back({.displayModel = displayIds[slot], .inventoryType = inventoryTypes[slot], .enchantment = 0u});
            // A weapon in each hand is two weapons: by its kind alone the one
            // in the off hand was a second main hand, and not shown.
            if (slot == 15) worn.back().hand = EquipmentItem::Hand::Main;
            if (slot == 16) worn.back().hand = EquipmentItem::Hand::Off;
        }
        return worn;
    }
    worn = listed;
    for (const auto& [bit, slot] : {std::pair{kHideHelm, kHeadSlot}, std::pair{kHideCloak, kBackSlot}}) {
        if ((playerFlags & bit) != 0 && worn.size() > static_cast<size_t>(slot))
            worn[static_cast<size_t>(slot)] = EquipmentItem{};
    }
    return worn;
}

/// The same by equipment slot, for a dressing room (0x005989e0).
inline std::array<EquipmentItem, 19> wornBySlot(bool known, const std::array<uint32_t, 19>& displayIds,
                                                const std::array<uint8_t, 19>& inventoryTypes,
                                                const std::vector<EquipmentItem>& listed, uint32_t playerFlags) {
    std::array<EquipmentItem, 19> out{};
    const std::vector<EquipmentItem> worn =
        known ? std::vector<EquipmentItem>{}
              : wornByPlayer(false, displayIds, inventoryTypes, listed, playerFlags);
    for (size_t slot = 0; slot < out.size(); ++slot) {
        if (known) {
            out[slot].displayModel = displayIds[slot];
            out[slot].inventoryType = inventoryTypes[slot];
        } else if (slot < worn.size()) {
            out[slot] = worn[slot];
        }
    }
    return out;
}

}  // namespace wowee::game::player_display

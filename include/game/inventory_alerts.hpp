#pragma once

/// The durability figure's alerts, as the client keeps them (0x005e90d0):
/// twelve entries - eleven equipment slots and the ammunition - each worked
/// out by 0x005e8fe0 and held until the next pass, which raises
/// UPDATE_INVENTORY_ALERTS when one moved and UPDATE_INVENTORY_DURABILITY
/// always. GetInventoryAlertStatus (0x005e7fa0) reads the held value.

#include <array>
#include <cstdint>

namespace wowee::game::inventory_alerts {

inline constexpr int kCount = 12;
/// DAT_00a1ce4c: head, shoulders, chest, waist, legs, feet, wrists, hands,
/// main hand, off hand, ranged, and -1 for the ammunition.
inline constexpr std::array<int, kCount> kSlots = {0, 2, 4, 5, 6, 7, 8, 9, 15, 16, 17, -1};

/// ITEM_FIELD_FLAGS the check reads.
inline constexpr uint32_t kFieldFlagNoDurability = 0x8;  ///< wrapped
inline constexpr uint32_t kFieldFlagBroken = 0x10;

struct Item {
    bool present = false;
    uint32_t fieldFlags = 0;
    uint32_t durability = 0;
    uint32_t maxDurability = 0;
};

struct Result {
    int status = 0;     ///< 0 fine, 1 low, 2 broken
    int tutorial = -1;  ///< 0x23 low, 0x24 broken, -1 none
};

/// 0x005e8fe0 for an equipment slot: broken by its flag, or out of
/// durability (tutorial 0x24); low with fewer than six points left (0x23).
[[nodiscard]] inline Result itemAlert(const Item& item) {
    if (!item.present) return {};
    if (item.fieldFlags & kFieldFlagBroken) return {2, -1};
    if ((item.fieldFlags & kFieldFlagNoDurability) || item.maxDurability == 0) return {};
    if (item.durability == 0) return {2, 0x24};
    if (item.durability < 6) return {1, 0x23};
    return {};
}

/// The ammunition's entry: low with fewer than 21 rounds of it.
[[nodiscard]] inline int ammoAlert(uint32_t ammoId, uint32_t count) {
    return ammoId != 0 && count < 0x15 ? 1 : 0;
}

}  // namespace wowee::game::inventory_alerts

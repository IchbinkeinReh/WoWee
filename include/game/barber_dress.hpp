#pragma once
// What the character wears in a barber's chair (0x0052f9b0): the barber's cape
// over its clothes in the tabard's place (display 0xd51f), no helm, no shoulders
// and no shield; its own again as it gets up (0x0052e9f0).
#include "game/inventory.hpp"

#include <cstdint>

namespace wowee::game::barber_dress {

inline constexpr uint32_t kCapeDisplay = 0xd51f;

/// Whether an item is put away for it.
[[nodiscard]] inline bool hides(EquipSlot slot, uint8_t inventoryType) {
    return slot == EquipSlot::HEAD || slot == EquipSlot::SHOULDERS ||
           (slot == EquipSlot::OFF_HAND && inventoryType == InvType::SHIELD);
}

/// What it wears, as it is drawn.
inline void apply(Inventory& worn) {
    for (const EquipSlot slot : {EquipSlot::HEAD, EquipSlot::SHOULDERS, EquipSlot::OFF_HAND}) {
        const ItemSlot& own = worn.getEquipSlot(slot);
        if (!own.empty() && hides(slot, static_cast<uint8_t>(own.item.inventoryType)))
            worn.setEquipSlot(slot, ItemDef{});
    }
    ItemDef cape = worn.getEquipSlot(EquipSlot::TABARD).item;
    if (cape.itemId == 0) cape.itemId = 1;  // shown, whether or not a tabard is worn
    cape.displayInfoId = kCapeDisplay;
    cape.inventoryType = InvType::TABARD;
    worn.setEquipSlot(EquipSlot::TABARD, cape);
}

}  // namespace wowee::game::barber_dress

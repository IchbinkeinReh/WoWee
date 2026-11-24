#pragma once

// Which of the interface's bags a change of the inventory reaches.
//
// The client raises BAG_UPDATE for the container an item is in (0x005d7320 ->
// 0x005d7070) and UNIT_INVENTORY_CHANGED where the item is worn (0x005e84c0),
// as it is created, changes, moves, or its template arrives (0x007078e0).
// Every bag and both events went out for each of them here: an item query
// answered was twelve bags redrawn, and arriving in a town thirty answers.

#include "game/inventory.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace wowee {
namespace game {

struct InventoryChange {
    /// The interface's own numbers: 0 the backpack, 1 to 4 the worn bags, 5 to
    /// 11 the bank's. The player's own slots - the backpack's, the keyring's,
    /// the tokens' - are one container to the client, told as 0, -2 and -4.
    bool own = false;
    std::array<bool, Inventory::NUM_BAG_SLOTS + Inventory::BANK_BAG_SLOTS> bags{};
    bool worn = false;

    [[nodiscard]] bool any() const {
        return own || worn || std::any_of(bags.begin(), bags.end(), [](bool b) { return b; });
    }
    /// BAG_UPDATE's arguments, in the client's order.
    [[nodiscard]] std::vector<int> bagNumbers() const {
        std::vector<int> out;
        if (own) out = {0, -2, -4};
        for (size_t i = 0; i < bags.size(); ++i)
            if (bags[i]) out.push_back(static_cast<int>(i) + 1);
        return out;
    }
};

/// Whether a slot shows something else: another item, or another count, wear,
/// state, roll or - its template come - name of it.
[[nodiscard]] inline bool slotChanged(const ItemSlot& was, const ItemSlot& now) {
    const ItemDef &a = was.item, &b = now.item;
    return a.guid != b.guid || a.itemId != b.itemId || a.stackCount != b.stackCount ||
           a.curDurability != b.curDurability || a.maxDurability != b.maxDurability || a.soulbound != b.soulbound ||
           a.wrapped != b.wrapped || a.randomPropertyId != b.randomPropertyId || a.suffixFactor != b.suffixFactor ||
           a.displayInfoId != b.displayInfoId || a.quality != b.quality || a.name != b.name;
}

[[nodiscard]] inline InventoryChange inventoryChange(const Inventory& was, const Inventory& now) {
    InventoryChange c;
    for (int i = 0; i < Inventory::BACKPACK_SLOTS; ++i)
        c.own = c.own || slotChanged(was.getBackpackSlot(i), now.getBackpackSlot(i));
    for (int i = 0; i < Inventory::KEYRING_SLOTS; ++i)
        c.own = c.own || slotChanged(was.getKeyringSlot(i), now.getKeyringSlot(i));
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i)
        c.worn = c.worn || slotChanged(was.getEquipSlot(static_cast<EquipSlot>(i)),
                                       now.getEquipSlot(static_cast<EquipSlot>(i)));
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b) {
        bool changed = was.getBagSize(b) != now.getBagSize(b);
        for (int i = 0; i < std::max(was.getBagSize(b), now.getBagSize(b)); ++i)
            changed = changed || slotChanged(was.getBagSlot(b, i), now.getBagSlot(b, i));
        c.bags[static_cast<size_t>(b)] = changed;
    }
    for (int b = 0; b < Inventory::BANK_BAG_SLOTS; ++b) {
        bool changed = was.getBankBagSize(b) != now.getBankBagSize(b);
        for (int i = 0; i < std::max(was.getBankBagSize(b), now.getBankBagSize(b)); ++i)
            changed = changed || slotChanged(was.getBankBagSlot(b, i), now.getBankBagSlot(b, i));
        c.bags[static_cast<size_t>(Inventory::NUM_BAG_SLOTS + b)] = changed;
    }
    return c;
}

/// Where one item is: the bag its own change is told of, or worn.
[[nodiscard]] inline InventoryChange placeOfItem(const Inventory& inv, uint64_t guid) {
    InventoryChange c;
    if (guid == 0) return c;
    for (int i = 0; i < Inventory::BACKPACK_SLOTS; ++i) c.own = c.own || inv.getBackpackSlot(i).item.guid == guid;
    for (int i = 0; i < Inventory::KEYRING_SLOTS; ++i) c.own = c.own || inv.getKeyringSlot(i).item.guid == guid;
    for (int i = 0; i < Inventory::NUM_EQUIP_SLOTS; ++i) {
        if (inv.getEquipSlot(static_cast<EquipSlot>(i)).item.guid != guid) continue;
        // What is worn is the player's own container too.
        c.worn = true;
        c.own = true;
    }
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBagSize(b); ++i)
            if (inv.getBagSlot(b, i).item.guid == guid) c.bags[static_cast<size_t>(b)] = true;
    for (int b = 0; b < Inventory::BANK_BAG_SLOTS; ++b)
        for (int i = 0; i < inv.getBankBagSize(b); ++i)
            if (inv.getBankBagSlot(b, i).item.guid == guid) c.bags[static_cast<size_t>(Inventory::NUM_BAG_SLOTS + b)] = true;
    return c;
}

}  // namespace game
}  // namespace wowee

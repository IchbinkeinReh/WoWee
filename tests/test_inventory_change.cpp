// Which bags a change of the inventory is told of (game/inventory_change.hpp,
// 0x005d7070, 0x005e84c0): every answered item query told all twelve.
#include <catch_amalgamated.hpp>

#include "game/inventory_change.hpp"

using namespace wowee::game;

namespace {

ItemDef item(uint32_t entry, uint64_t guid, const char* name = "") {
    ItemDef d;
    d.itemId = entry;
    d.guid = guid;
    d.name = name;
    return d;
}

Inventory carrying() {
    Inventory inv;
    inv.setBackpackSlot(0, item(6948, 1, "Hearthstone"));
    inv.setBagSize(0, 8);
    inv.setBagSlot(0, 2, item(1179, 2));
    inv.setBagSize(1, 6);
    inv.setBagSlot(1, 5, item(858, 3, "Potion"));
    inv.setEquipSlot(EquipSlot::CHEST, item(6120, 4, "Robe"));
    return inv;
}

}  // namespace

TEST_CASE("nothing changed is nothing told", "[inventory_change]") {
    CHECK_FALSE(inventoryChange(carrying(), carrying()).any());
    CHECK(inventoryChange(carrying(), carrying()).bagNumbers().empty());
}

TEST_CASE("an item's answer is told to the bag it is in", "[inventory_change]") {
    Inventory now = carrying();
    now.setBagSlot(0, 2, item(1179, 2, "Ice Cold Milk"));
    const InventoryChange c = inventoryChange(carrying(), now);
    CHECK(c.bagNumbers() == std::vector<int>{1});
    CHECK_FALSE(c.worn);
}

TEST_CASE("the player's own slots are one container: 0, -2 and -4", "[inventory_change]") {
    Inventory now = carrying();
    ItemDef stone = item(6948, 1, "Hearthstone");
    stone.stackCount = 2;
    now.setBackpackSlot(0, stone);
    CHECK(inventoryChange(carrying(), now).bagNumbers() == std::vector<int>{0, -2, -4});
    now = carrying();
    now.setKeyringSlot(3, item(5396, 9, "Key"));
    CHECK(inventoryChange(carrying(), now).bagNumbers() == std::vector<int>{0, -2, -4});
}

TEST_CASE("what is worn is the character's, with no bag", "[inventory_change]") {
    Inventory now = carrying();
    ItemDef robe = item(6120, 4, "Robe");
    robe.maxDurability = 40;
    robe.curDurability = 12;
    now.setEquipSlot(EquipSlot::CHEST, robe);
    const InventoryChange c = inventoryChange(carrying(), now);
    CHECK(c.worn);
    CHECK(c.bagNumbers().empty());
}

TEST_CASE("an item moved is told where it was and where it is", "[inventory_change]") {
    Inventory now = carrying();
    now.setBagSlot(1, 5, ItemDef{});
    now.setBankBagSize(2, 10);
    now.setBankBagSlot(2, 0, item(858, 3, "Potion"));
    CHECK(inventoryChange(carrying(), now).bagNumbers() == std::vector<int>{2, 7});
}

TEST_CASE("a change of the item itself is its bag's (0x00707f50)", "[inventory_change]") {
    const Inventory inv = carrying();
    CHECK(placeOfItem(inv, 3).bagNumbers() == std::vector<int>{2});
    CHECK(placeOfItem(inv, 1).bagNumbers() == std::vector<int>{0, -2, -4});
    CHECK(placeOfItem(inv, 4).worn);
    CHECK_FALSE(placeOfItem(inv, 99).any());
    CHECK_FALSE(placeOfItem(inv, 0).any());
}

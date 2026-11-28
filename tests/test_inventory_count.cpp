// How many of an item the bags hold.
//
// Counted in five places with two different rules. The Lua bindings treated a
// slot holding an item with a stack count of zero as one item; the client's
// own counters added the zero. So FrameXML would say a reagent was in the bags
// and the crafting check, reading the other counter, would say it was not -
// "you cannot make this" with the reagents sitting in the bag.
//
// A stack count of zero on an occupied slot is what several of the item paths
// produce for something that does not stack, so this is not a hypothetical
// row. The rule the tests below pin is the one the slot itself states: a slot
// that is not empty holds at least one of what is in it.
#include "catch_amalgamated.hpp"

#include "game/inventory.hpp"

using namespace wowee::game;

namespace {

ItemDef item(uint32_t id, uint32_t stack) {
    ItemDef def;
    def.itemId = id;
    def.stackCount = stack;
    return def;
}

}  // namespace

TEST_CASE("an occupied slot holds at least one", "[inventory-count]") {
    // The rule the two conventions disagreed on.
    Inventory inv;
    inv.setBackpackSlot(0, item(4306, 0));
    CHECK(inv.countItem(4306) == 1);
}

TEST_CASE("stacks add up across the backpack", "[inventory-count]") {
    Inventory inv;
    inv.setBackpackSlot(0, item(4306, 12));
    inv.setBackpackSlot(1, item(4306, 8));
    inv.setBackpackSlot(2, item(2589, 20));
    CHECK(inv.countItem(4306) == 20);
    CHECK(inv.countItem(2589) == 20);
}

TEST_CASE("the equip bags count too", "[inventory-count]") {
    // A reagent in a bag is as usable as one in the backpack, and a counter
    // that stopped at the backpack would refuse a craft for no visible reason.
    Inventory inv;
    inv.setBagSize(0, 16);
    inv.setBackpackSlot(0, item(4306, 5));
    inv.setBagSlot(0, 3, item(4306, 7));
    CHECK(inv.countItem(4306) == 12);
}

TEST_CASE("an item nowhere in the bags is none of it", "[inventory-count]") {
    Inventory inv;
    inv.setBackpackSlot(0, item(4306, 5));
    CHECK(inv.countItem(2589) == 0);
    CHECK(inv.countItem(0) == 0);
}

TEST_CASE("empty slots hold nothing", "[inventory-count]") {
    // The guard that keeps an empty slot's leftover id from being counted.
    Inventory inv;
    CHECK(inv.countItem(4306) == 0);
    inv.setBackpackSlot(0, item(4306, 3));
    inv.clearBackpackSlot(0);
    CHECK(inv.countItem(4306) == 0);
}

TEST_CASE("zero-stack slots each count once", "[inventory-count]") {
    // Two non-stacking items of the same kind are two, not zero and not one.
    Inventory inv;
    inv.setBagSize(0, 16);
    inv.setBackpackSlot(0, item(7005, 0));
    inv.setBagSlot(0, 0, item(7005, 0));
    CHECK(inv.countItem(7005) == 2);
}

// T473: the reagent -2 of Create Healthstone and the quest combines is "one
// free bag slot" - 0x00754d00 answers it with 0x00754470's count of the empty
// slots in the backpack and in the worn containers (not a quiver).
TEST_CASE("the reagent -2 is counted as the free bag slots", "[inventory-count]") {
    Inventory inv;
    const uint32_t backpack = static_cast<uint32_t>(inv.getBackpackSize());
    CHECK(inv.countCarried(Inventory::kFreeSlotReagent) == backpack);
    inv.setBackpackSlot(0, item(6265, 1));
    CHECK(inv.countCarried(Inventory::kFreeSlotReagent) == backpack - 1);
    inv.setBagSize(0, 4);
    inv.setBagSlot(0, 1, item(4306, 3));
    CHECK(inv.countCarried(Inventory::kFreeSlotReagent) == backpack - 1 + 3);
    inv.setBagSize(1, 6);
    inv.setBagQuiver(1, true);
    CHECK(inv.countCarried(Inventory::kFreeSlotReagent) == backpack - 1 + 3);
    for (int i = 0; i < inv.getBackpackSize(); ++i) inv.setBackpackSlot(i, item(4306, 1));
    for (int i = 0; i < 4; ++i) inv.setBagSlot(0, i, item(4306, 1));
    CHECK(inv.countCarried(Inventory::kFreeSlotReagent) == 0);
}

// The equipment manager's own sets: saving (0x005ade50, 0x005af7e0), renaming
// (0x005af400), the server's answer (0x005ae760) and what
// CMSG_EQUIPMENT_SET_USE says for each slot (0x005ada20).
#include <catch_amalgamated.hpp>

#include "game/equipment_sets.hpp"

using namespace wowee::game::equipment_sets;

namespace {

Worn worn() {
    Worn w{};
    for (size_t i = 0; i < w.size(); ++i) w[i] = 0x4000000000000100ull + i;
    w[3] = 0;   // no shirt
    return w;
}

}  // namespace

TEST_CASE("a new set takes the lowest place and waits for its answer", "[equipment_sets]") {
    std::vector<Set> sets;
    Set* a = save(sets, "Tank", "INV_Shield_06", worn(), 0);
    REQUIRE(a);
    CHECK(a->setId == 0);
    CHECK(a->setGuid == 0);
    CHECK(a->items[0] == worn()[0]);
    CHECK(a->items[3] == 0);
    // Not saved again before the server has answered ...
    CHECK(save(sets, "Tank", "INV_Shield_06", worn(), 0) == nullptr);
    saved(sets, 0, 0x77);
    CHECK(sets[0].setGuid == 0x77);
    // ... and then with what is worn now, the same set.
    Worn changed = worn();
    changed[0] = 0x99;
    Set* again = save(sets, "Tank", "INV_Helmet_01", changed, 1u << 4);
    REQUIRE(again);
    CHECK(sets.size() == 1);
    CHECK(again->setGuid == 0x77);
    CHECK(again->iconName == "INV_Helmet_01");
    CHECK(again->items[0] == 0x99);
    CHECK(again->items[4] == kIgnored);
    CHECK(ignoreMask(*again) == 1u << 4);
}

TEST_CASE("places are given again, and there are ten", "[equipment_sets]") {
    std::vector<Set> sets;
    for (int i = 0; i < 10; ++i) REQUIRE(save(sets, "S" + std::to_string(i), "i", worn(), 0));
    CHECK(save(sets, "S10", "i", worn(), 0) == nullptr);
    CHECK_FALSE(freeId(sets));
    std::erase_if(sets, [](const Set& s) { return s.setId == 4; });
    CHECK(freeId(sets) == 4u);
    Set* back = save(sets, "S10", "i", worn(), 0);
    REQUIRE(back);
    CHECK(back->setId == 4);
}

TEST_CASE("a set the server would not keep goes", "[equipment_sets]") {
    std::vector<Set> sets;
    save(sets, "Tank", "i", worn(), 0);
    saved(sets, 0, 0);
    CHECK(sets.empty());
}

TEST_CASE("a renamed set has its new name at once", "[equipment_sets]") {
    std::vector<Set> sets;
    save(sets, "Tank", "i", worn(), 0);
    save(sets, "Heal", "i", worn(), 0);
    saved(sets, 0, 0x77);
    // A name another set has, and a set there is none of.
    CHECK_FALSE(rename(sets, "Tank", "Heal", worn(), 0).changed);
    CHECK_FALSE(rename(sets, "Nope", "New", worn(), 0).changed);
    const auto renamed = rename(sets, "Tank", "Block", worn(), 1u << 2);
    CHECK(renamed.changed);
    REQUIRE(renamed.send);
    CHECK(renamed.send->name == "Block");
    CHECK(renamed.send->setGuid == 0x77);
    CHECK(renamed.send->items[2] == kIgnored);
    CHECK(byName(sets, "Tank") == nullptr);
    // One not answered yet is named, and not sent.
    const auto waiting = rename(sets, "Heal", "Mend", worn(), 0);
    CHECK(waiting.changed);
    CHECK(waiting.send == nullptr);
    CHECK(byName(sets, "Mend"));
}

TEST_CASE("a slot the set leaves alone is told as one", "[equipment_sets]") {
    std::array<uint64_t, kSlots> items{};
    items[0] = 0x500;
    items[1] = kIgnored;
    items[2] = 0;
    items[4] = 0x501;   // nowhere
    const auto find = [](uint64_t guid) -> std::optional<Place> {
        if (guid == 0x500) return Place{0xFF, 23};
        return std::nullopt;
    };
    const Swap s = swap(items, worn(), false, find);
    CHECK(s.slots[0].guid == 0x500);
    CHECK(s.slots[0].from.bag == 0xFF);
    CHECK(s.slots[0].from.slot == 23);
    // Sent as 0, the server takes off what is worn there.
    CHECK(s.slots[1].guid == kIgnored);
    CHECK(s.slots[2].guid == 0);
    CHECK(s.slots[4].guid == kIgnored);
    CHECK(s.missing);
    CHECK_FALSE(s.combat);
    // Nor is it an item that is missing.
    items[4] = kIgnored;
    CHECK_FALSE(swap(items, worn(), false, find).missing);
}

TEST_CASE("in combat the weapons alone are changed", "[equipment_sets]") {
    std::array<uint64_t, kSlots> items = worn();
    items[0] = 0x500;
    items[15] = 0x501;
    items[16] = 0;
    const auto find = [](uint64_t) -> std::optional<Place> { return Place{0xFF, 24}; };
    const Swap s = swap(items, worn(), true, find);
    CHECK(s.slots[0].guid == kIgnored);
    CHECK(s.slots[15].guid == 0x501);
    CHECK(s.slots[16].guid == 0);
    CHECK(s.combat);
    // Nothing is said where the armour is the same.
    items[0] = worn()[0];
    CHECK_FALSE(swap(items, worn(), true, find).combat);
}

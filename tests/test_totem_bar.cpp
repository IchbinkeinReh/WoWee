// The totem bar's element lists and slots, as 0x00542030, 0x005a7ae0 and
// 0x005a7b50 keep them.
#include <catch_amalgamated.hpp>

#include <map>

#include "game/totem_bar.hpp"

namespace tb = wowee::game::totem_bar;

TEST_CASE("a totem category names its elements", "[totembar]") {
    CHECK(tb::elementMask(4) == 0x1);   // fire
    CHECK(tb::elementMask(2) == 0x2);   // earth
    CHECK(tb::elementMask(5) == 0x4);   // water
    CHECK(tb::elementMask(3) == 0x8);   // air
    CHECK(tb::elementMask(21) == 0xf);
    CHECK(tb::elementMask(1) == 0);
}

TEST_CASE("the bar's slots go fire, earth, water, air", "[totembar]") {
    CHECK(tb::slotCategory(0x84) == 4);
    CHECK(tb::slotCategory(0x85) == 2);
    CHECK(tb::slotCategory(0x86) == 5);
    CHECK(tb::slotCategory(0x87) == 3);
    CHECK(tb::slotCategory(0x88) == 4);
    CHECK(tb::slotCategory(0x83) == 0);
    CHECK(tb::slotCategory(0x90) == 0);
    const uint32_t fire[2] = {4, 0};
    CHECK(tb::slotTakes(0x84, fire));
    CHECK_FALSE(tb::slotTakes(0x85, fire));
}

TEST_CASE("a list keeps the highest rank of a name, in place", "[totembar]") {
    std::map<uint32_t, std::pair<std::string, uint32_t>> book = {
        {3599, {"Searing Totem", 1}}, {6363, {"Searing Totem", 2}}, {8190, {"Magma Totem", 1}}};
    const auto nameOf = [&](uint32_t id) { return book[id].first; };
    const auto rankOf = [&](uint32_t id) { return book[id].second; };
    tb::Lists lists;
    for (uint32_t id : {3599u, 8190u, 6363u}) {
        tb::Spell s{id, book[id].first, book[id].second, {4, 0}};
        tb::add(lists, s, nameOf, rankOf);
    }
    CHECK(lists[0] == std::vector<uint32_t>{6363, 8190});
    CHECK(lists[1].empty());
    // A lower rank after does not replace it, and a spell is listed once.
    tb::add(lists, {3599, "Searing Totem", 1, {4, 0}}, nameOf, rankOf);
    tb::add(lists, {8190, "Magma Totem", 1, {4, 0}}, nameOf, rankOf);
    CHECK(lists[0] == std::vector<uint32_t>{6363, 8190});
    // Category 21 goes to all four.
    tb::add(lists, {66842, "Call of the Elements", 0, {21, 0}}, nameOf, rankOf);
    for (const auto& l : lists) CHECK(l.back() == 66842);
}

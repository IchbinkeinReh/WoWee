// The profession window's list and its links (game/trade_skill_list.hpp).
#include <catch_amalgamated.hpp>

#include "game/trade_skill_list.hpp"

using namespace wowee::game::trade_skill;

// 0x005dd5a0: Leinenverband is trivial at 60 with 30 as its low rank.
TEST_CASE("how hard a recipe is at a rank", "[trade_skill]") {
    CHECK(difficulty(1, 60, 30) == 0);
    CHECK(difficulty(29, 60, 30) == 0);
    CHECK(difficulty(30, 60, 30) == 1);
    CHECK(difficulty(44, 60, 30) == 1);
    CHECK(difficulty(45, 60, 30) == 2);
    CHECK(difficulty(59, 60, 30) == 2);
    CHECK(difficulty(60, 60, 30) == 3);
    // Without a low rank it is 25 under the high one.
    CHECK(difficulty(74, 100, 0) == 0);
    CHECK(difficulty(75, 100, 0) == 1);
    CHECK(difficulty(87, 100, 0) == 2);
    CHECK(difficulty(1, 0, 0) == 3);
}

TEST_CASE("the slots of a recipe", "[trade_skill]") {
    CHECK(itemSlots(1) == 0x1);          // head
    CHECK(itemSlots(20) == 0x10);        // robe, as a chest piece
    CHECK(itemSlots(11) == 0x400);       // ring: the first finger alone
    CHECK(itemSlots(12) == 0x1000);
    CHECK(itemSlots(18) == 0x80000);     // bag
    CHECK(itemSlots(13) == 0x18000);     // either hand
    CHECK(itemSlots(0) == kNoSlot);
    CHECK(itemSlots(24) == kNoSlot);     // ammunition
    CHECK(itemSlots(99) == kNoSlot);
    CHECK(enchantSlots(2, 0x1, 0) == kWeaponEnchant);
    CHECK(enchantSlots(2, 0x2, 0) == kTwoHandEnchant);
    CHECK(enchantSlots(4, 0x40, 0) == kShieldEnchant);
    CHECK(enchantSlots(4, 0x1f, 0) == kNoSlot);
    CHECK(enchantSlots(4, 0x1f, 1u << 9) == 0x100);               // bracers
    CHECK(enchantSlots(4, 0x1f, (1u << 5) | (1u << 20)) == 0x10); // the lowest type's
    CHECK(enchantSlots(4, 0, 1u << 11) == 0x400);
    CHECK(enchantSlots(4, 0, 1u << 18) == 0x80000);
    CHECK(enchantSlots(-1, 0, 0) == kNoSlot);
}

namespace {
Recipe recipe(uint32_t spell, const char* name, int hard, int high, int32_t cls, int32_t sub, uint32_t level = 0,
              uint32_t slots = kNoSlot, int canMake = 0) {
    Recipe r;
    r.spellId = spell;
    r.name = name;
    r.difficulty = hard;
    r.trivialAt = high;
    r.itemClass = cls;
    r.itemSubClass = sub;
    r.itemLevel = level;
    r.slots = slots;
    r.canMake = canMake;
    return r;
}
std::string nameOf(int32_t cls, int32_t sub) {
    if (cls < 0) return "Verzaubern";
    if (cls == 4) return sub == 1 ? "Stoff" : "Leder";
    return "Beutel";
}
std::vector<Recipe> sample() {
    return {recipe(10, "Grau", 3, 50, 4, 1, 10, 0x10, 0),     recipe(11, "Gelb B", 1, 90, 4, 1, 20, 0x1, 2),
            recipe(12, "Gelb A", 1, 90, 4, 1, 20, 0x1, 0),    recipe(13, "Gelb hoch", 1, 95, 4, 1, 5, 0x10, 1),
            recipe(14, "Gelb teuer", 1, 90, 4, 1, 30, 0x1, 1), recipe(15, "Orange", 0, 80, 4, 1, 1, 0x2, 1),
            recipe(16, "Lederhemd", 1, 90, 4, 2, 20, 0x10, 1), recipe(17, "Beutel", 2, 70, 1, 0, 5, 0x80000, 1),
            recipe(18, "Armschiene", 2, 70, -1, -1, 0, 0x100, -1)};
}
std::vector<uint32_t> order(const List& list) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < list.visibleCount(); ++i) out.push_back(list.rows()[i].recipe.spellId);
    return out;
}
}  // namespace

// 0x005dae60, 0x005da6b0
TEST_CASE("the order of the list", "[trade_skill]") {
    List list;
    list.build(sample(), nameOf, {});
    // By the class, then the subclass's name; the hardest first, then by the rank it is
    // trivial at and the item's level, from the top, then by name.
    CHECK(order(list) == std::vector<uint32_t>{0, 18, 0, 17, 0, 16, 0, 15, 13, 14, 12, 11, 10});
    REQUIRE(list.groups().size() == 4);
    CHECK(list.groups()[0].name == "Verzaubern");
    CHECK(list.groups()[2].name == "Leder");
    CHECK(list.groups()[3].name == "Stoff");
    CHECK(list.rows().size() == 13);
    CHECK(list.firstRecipe() == 2);
    CHECK(list.indexOf(13) == 9);
    CHECK(list.indexOf(999) == 0);
    CHECK(list.row(0) == nullptr);
    CHECK(list.row(14) == nullptr);
}

TEST_CASE("the list's filters", "[trade_skill]") {
    List list;
    list.build(sample(), nameOf, {});
    List::Filters f;
    f.onlyMakeable = true;   // one without reagents can be made as well
    list.refilter(f);
    CHECK(order(list) == std::vector<uint32_t>{0, 18, 0, 17, 0, 16, 0, 15, 13, 14, 11});
    // The hidden rows stay in the list, past the shown.
    CHECK(list.rows().size() == 13);
    CHECK(list.indexOf(12) > 11);
    f = {};
    f.onlySkillUps = true;
    list.refilter(f);
    CHECK(order(list) == std::vector<uint32_t>{0, 18, 0, 17, 0, 16, 0, 15, 13, 14, 12, 11});
    f = {};
    f.passes = [](const Recipe& r) { return r.name.find("Gelb") == 0; };
    list.refilter(f);
    CHECK(order(list) == std::vector<uint32_t>{0, 13, 14, 12, 11});
    f = {};
    f.slots = 0x10;
    list.refilter(f);
    CHECK(order(list) == std::vector<uint32_t>{0, 16, 0, 13, 10});
}

TEST_CASE("the list's subclasses and headings", "[trade_skill]") {
    List list;
    list.build(sample(), nameOf, {});
    CHECK(list.allGroupsShown());
    CHECK(list.slots() == (0x100u | 0x80000u | 0x10u | 0x1u | 0x2u));
    CHECK(list.slotBit(0) == 0);
    CHECK(list.slotBit(4) == 19);
    CHECK(list.slotBit(5) == -1);
    // One subclass alone: its recipes, and the slots of it.
    list.showGroup(3, true, true);
    list.refilter({});
    CHECK(order(list) == std::vector<uint32_t>{0, 15, 13, 14, 12, 11, 10});
    CHECK_FALSE(list.allGroupsShown());
    CHECK(list.groupShown(3));
    CHECK_FALSE(list.groupShown(0));
    CHECK(list.slots() == 0x13u);
    list.showGroup(0, true, false);
    list.showGroup(3, false, false);
    list.refilter({});
    CHECK(order(list) == std::vector<uint32_t>{0, 18});
    // A list built again keeps it, and a new line has none of it.
    list.build(sample(), nameOf, {});
    CHECK(order(list) == std::vector<uint32_t>{0, 18});
    CHECK_FALSE(list.groupShown(3));
    list.showAllGroups();
    list.refilter({});
    CHECK(order(list).size() == 13);
    // A closed heading stays and its recipes go.
    list.close(3, true);
    list.refilter({});
    CHECK(order(list) == std::vector<uint32_t>{0, 18, 0, 17, 0, 16, 0});
    CHECK_FALSE(list.open(3));
    list.build(sample(), nameOf, {});
    CHECK_FALSE(list.open(3));
    list.refilter({});
    CHECK(order(list) == std::vector<uint32_t>{0, 18, 0, 17, 0, 16, 0});
    list.close(-1, true);
    list.refilter({});
    CHECK(order(list) == std::vector<uint32_t>{0, 0, 0, 0});
    list.close(-1, false);
    list.refilter({});
    CHECK(order(list).size() == 13);
    list.close(2, true);
    list.refilter({});
    list.newLine();
    CHECK(list.rows().empty());
    list.build(sample(), nameOf, {});
    CHECK(order(list).size() == 13);
}

// 0x005de300, 0x005de120, 0x005db060
TEST_CASE("a profession's link", "[trade_skill]") {
    Link link;
    REQUIRE(readLink("7411:12:85:C:BAg/", link));
    CHECK(link.spellId == 7411);
    CHECK(link.rank == 12);
    CHECK(link.maxRank == 85);
    CHECK(link.guid == 0xC);
    CHECK(link.known == "BAg/");
    REQUIRE(readLink("51304:450:450:100000000AB12CD:", link));
    CHECK(link.guid == 0x100000000AB12CDull);
    CHECK(link.known.empty());
    CHECK_FALSE(readLink("", link));
    CHECK_FALSE(readLink("7411", link));
    CHECK_FALSE(readLink("7411:12:85:0:AA", link));
    CHECK_FALSE(readLink("7411:12:85", link));
    CHECK(linkFits("AAA", 18));
    CHECK(linkFits("AAA", 13));
    CHECK_FALSE(linkFits("AAA", 12));
    CHECK_FALSE(linkFits("AAA", 19));
    // 'B' is the first of its six, 'g' (32) the last, '/' all.
    CHECK(linkKnows("BAg/", 0));
    CHECK_FALSE(linkKnows("BAg/", 1));
    CHECK_FALSE(linkKnows("BAg/", 6));
    CHECK(linkKnows("BAg/", 17));
    CHECK_FALSE(linkKnows("BAg/", 16));
    CHECK(linkKnows("BAg/", 18));
    CHECK(linkKnows("BAg/", 23));
    CHECK_FALSE(linkKnows("BAg/", 24));
    CHECK_FALSE(linkKnows("!", 0));
}

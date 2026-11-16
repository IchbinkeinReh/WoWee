// Choices 0x006277f0 makes from an item's template (item_tooltip_rules.hpp).
#include <catch_amalgamated.hpp>
#include "game/item_tooltip_rules.hpp"

#include <string>

using namespace wowee::game::item_tooltip;

namespace {
std::string key(const UniqueLine& u) { return u.key ? u.key : ""; }
}  // namespace

TEST_CASE("Unique-equipped is flag 0x80000 and comes before MaxCount", "[item_tooltip]") {
    CHECK(key(uniqueLine(4, 0x80000, 1, false, 0)) == "ITEM_UNIQUE_EQUIPPABLE");
    // 0x1000000 is not the unique-equipped flag.
    CHECK(key(uniqueLine(4, 0x1000000, 0, false, 0)).empty());
    CHECK(key(uniqueLine(4, 0, 1, false, 0)) == "ITEM_UNIQUE");
    const auto many = uniqueLine(0, 0, 20, false, 0);
    CHECK(key(many) == "ITEM_UNIQUE_MULTIPLE");
    CHECK(many.count);
}

TEST_CASE("A limit category names itself, by its flag", "[item_tooltip]") {
    const auto equipped = uniqueLine(3, 0, 0, true, 1);
    CHECK(key(equipped) == "ITEM_LIMIT_CATEGORY_MULTIPLE");
    CHECK(equipped.category);
    CHECK(key(uniqueLine(0, 0, 0, true, 0)) == "ITEM_LIMIT_CATEGORY");
    // MaxCount wins over the category.
    CHECK(key(uniqueLine(3, 0, 1, true, 1)) == "ITEM_UNIQUE");
}

TEST_CASE("Class 10 says nothing of uniqueness", "[item_tooltip]") {
    CHECK(key(uniqueLine(10, 0x80000, 1, true, 1)).empty());
}

TEST_CASE("The item level line is for weapons, armour, reagents and projectiles", "[item_tooltip]") {
    CHECK(showsItemLevel(2));
    CHECK(showsItemLevel(4));
    CHECK(showsItemLevel(5));
    CHECK(showsItemLevel(6));
    CHECK_FALSE(showsItemLevel(0));
    CHECK_FALSE(showsItemLevel(1));
    CHECK_FALSE(showsItemLevel(3));
    CHECK_FALSE(showsItemLevel(16));
}

TEST_CASE("Glyph kind by GlyphProperties' type flags", "[item_tooltip]") {
    CHECK(std::string(glyphKey(0)) == "MAJOR_GLYPH");
    CHECK(std::string(glyphKey(1)) == "MINOR_GLYPH");
}

TEST_CASE("A set's piece total counts each slot once", "[item_tooltip]") {
    // Head, chest, legs: three.
    CHECK(setPieceTotal({1, 5, 7}) == 3);
    // A robe and a chest share a slot; a second head is the same slot.
    CHECK(setPieceTotal({1, 5, 20, 1}) == 2);
    // Rings and trinkets each count.
    CHECK(setPieceTotal({11, 11, 12, 12}) == 4);
    // A piece not known counts nothing, and stops the ones after it.
    CHECK(setPieceTotal({1, 0, 7}) == 1);
}

TEST_CASE("Prospecting before milling", "[item_tooltip]") {
    CHECK(std::string(processKey(0x40000)) == "ITEM_PROSPECTABLE");
    CHECK(std::string(processKey(0x20000000)) == "ITEM_MILLABLE");
    CHECK(processKey(0x2) == nullptr);
}

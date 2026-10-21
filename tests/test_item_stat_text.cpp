// An item's stat and resistance lines in the interface's words, in the
// client's order (0x006277f0; item_stat_text.hpp).
#include <catch_amalgamated.hpp>
#include "game/item_stat_text.hpp"

#include <string>

using namespace wowee::game;

namespace {
// GlobalStrings.lua as a German client has them; anything else answers enUS.
std::string german(const char* key, const char* enUS) {
    const std::string k = key;
    if (k == "ITEM_MOD_STAMINA") return "%1$c%2$d Ausdauer";
    if (k == "ITEM_MOD_STRENGTH") return "%1$c%2$d St\xc3\xa4rke";
    if (k == "ITEM_MOD_INTELLECT") return "%1$c%2$d Intelligenz";
    if (k == "ITEM_MOD_HIT_RATING") return "Erh\xc3\xb6ht Trefferwertung um %d.";
    if (k == "ITEM_MOD_SPELL_POWER") return "Erh\xc3\xb6ht die Zaubermacht um %d.";
    if (k == "ITEM_SPELL_TRIGGER_ONEQUIP") return "Anlegen:";
    if (k == "ITEM_RESIST_SINGLE") return "%c%d %swiderstand";
    if (k == "ITEM_RESIST_ALL") return "%1$c%2$d zu allen Widerstandsarten";
    if (k == "SPELL_SCHOOL2_CAP") return "Feuer";
    if (k == "SPELL_SCHOOL6_CAP") return "Arkan";
    return enUS;
}
std::string english(const char*, const char* enUS) { return enUS; }
}  // namespace

TEST_CASE("Primary stats are white and signed, in the client's order", "[item_stat_text]") {
    // Stamina before intellect, strength before both, whatever the item's order.
    const auto lines = itemStatLines({{5, 12}, {7, 20}, {4, 3}}, german);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "+3 St\xc3\xa4rke");
    CHECK(lines[1].text == "+20 Ausdauer");
    CHECK(lines[2].text == "+12 Intelligenz");
    for (const auto& l : lines) CHECK_FALSE(l.equip);
    CHECK(itemStatLines({{7, -5}}, english)[0].text == "-5 Stamina");
}

TEST_CASE("Ratings and powers are green Equip: lines", "[item_stat_text]") {
    const auto lines = itemStatLines({{45, 30}, {31, 20}, {7, 10}}, german);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "+10 Ausdauer");
    CHECK_FALSE(lines[0].equip);
    // The hit rating (31) comes before the spell power (45) in the client's table.
    CHECK(lines[1].text == "Anlegen: Erh\xc3\xb6ht Trefferwertung um 20.");
    CHECK(lines[1].equip);
    CHECK(lines[2].text == "Anlegen: Erh\xc3\xb6ht die Zaubermacht um 30.");
    CHECK(itemStatLines({{31, 20}}, english)[0].text == "Equip: Improves hit rating by 20.");
}

TEST_CASE("A stat of zero or one the client names nothing for is left out", "[item_stat_text]") {
    CHECK(itemStatLines({{7, 0}, {2, 5}, {40, 9}}, german).empty());
}

TEST_CASE("Resistances: one line each, or all at once", "[item_stat_text]") {
    const int32_t fireArcane[6] = {0, 10, 0, 0, 0, 7};
    const auto lines = itemResistLines(fireArcane, german);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "+10 Feuerwiderstand");
    CHECK(lines[1] == "+7 Arkanwiderstand");
    const int32_t all[6] = {5, 5, 5, 5, 5, 5};
    REQUIRE(itemResistLines(all, german).size() == 1);
    CHECK(itemResistLines(all, german)[0] == "+5 zu allen Widerstandsarten");
    const int32_t none[6] = {0, 0, 0, 0, 0, 0};
    CHECK(itemResistLines(none, german).empty());
}

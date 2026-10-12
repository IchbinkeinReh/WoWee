// The client's own loot, money and roll lines (game/interface_text.hpp): the
// GlobalStrings each packet is worded with, chosen as the original chooses them
// (0x00523900, 0x006ceca0, 0x007e7d80, 0x006fa840, 0x006fb700).
#include <catch_amalgamated.hpp>

#include <map>
#include <string>

#include "game/interface_text.hpp"

namespace it = wowee::game::interface_text;

namespace {
// A German 3.3.5 interface's strings, as GlobalStrings.lua ships them.
const std::map<std::string, std::string> kDeDE = {
    {"GOLD_AMOUNT", "%d Gold"},
    {"SILVER_AMOUNT", "%d Silber"},
    {"COPPER_AMOUNT", "%d Kupfer"},
    {"YOU_LOOT_MONEY", "Ihr plündert %s"},
    {"LOOT_MONEY_SPLIT", "Euer Anteil an der Beute ist %s."},
    {"LOOT_ITEM_SELF", "Ihr erhaltet Beute: %s."},
    {"LOOT_ITEM_SELF_MULTIPLE", "Ihr erhaltet Beute: %sx%d."},
    {"LOOT_ITEM_PUSHED_SELF", "Ihr bekommt einen Gegenstand: %s."},
    {"LOOT_ITEM_PUSHED_SELF_MULTIPLE", "Ihr erhaltet den Gegenstand: %sx%d."},
    {"LOOT_ITEM_CREATED_SELF", "Ihr stellt her: %s."},
    {"LOOT_ITEM", "%s bekommt Beute: %s."},
    {"CREATED_ITEM_MULTIPLE", "%s stellt her: %sx%d."},
    {"LOOT_ROLL_NEED_SELF", "Ihr habt für %s 'Bedarf' ausgewählt"},
    {"LOOT_ROLL_GREED", "%s hat für %s 'Gier' ausgewählt"},
    {"LOOT_ROLL_PASSED", "%s würfelt nicht für: %s"},
    {"LOOT_ROLL_PASSED_SELF_AUTO", "Ihr passt automatisch bei %s, weil Ihr den Gegenstand nicht benutzen könnt."},
    {"LOOT_ROLL_PASSED_AUTO_FEMALE", "%1$s passt automatisch bei %2$s, weil sie den Gegenstand nicht benutzen kann."},
    {"LOOT_ROLL_ROLLED_NEED", "Wurf für Bedarf: %d für %s von %s"},
    {"LOOT_ROLL_WON", "%s gewinnt: %s"},
    {"LOOT_ROLL_YOU_WON_NO_SPAM_GREED", "Ihr gewinnt: %2$s |cff818181(Gier - %1$d)|r"},
    {"LOOT_ROLL_WON_NO_SPAM_NEED", "%1$s gewinnt: %3$s |cff818181(Bedarf - %2$d)|r"},
};

std::string deDE(const char* key, const char* enUS) {
    auto f = kDeDE.find(key);
    return f == kDeDE.end() ? std::string(enUS) : f->second;
}

const std::string kLink = "|cffffffff|Hitem:2589:0:0:0:0:0:0:0:80|h[Leinenstoff]|h|r";
}  // namespace

TEST_CASE("money is worded coin by coin, zeros left out, joined by a comma", "[interface_text]") {
    CHECK(it::moneyText(5, deDE) == "5 Kupfer");
    CHECK(it::moneyText(10203, deDE) == "1 Gold, 2 Silber, 3 Kupfer");
    CHECK(it::moneyText(10003, deDE) == "1 Gold, 3 Kupfer");
    CHECK(it::moneyText(200, deDE) == "2 Silber");
    CHECK(it::moneyText(0, deDE).empty());
}

TEST_CASE("looted money is YOU_LOOT_MONEY alone and LOOT_MONEY_SPLIT shared", "[interface_text]") {
    CHECK(it::lootMoneyLine(5, true, deDE) == "Ihr plündert 5 Kupfer");
    CHECK(it::lootMoneyLine(150, false, deDE) == "Euer Anteil an der Beute ist 1 Silber, 50 Kupfer.");
    CHECK(it::lootMoneyLine(0, true, deDE).empty());
}

TEST_CASE("an item push picks loot, pushed or created, single or multiple", "[interface_text]") {
    it::ItemPush p;
    CHECK(it::itemPushLine(p, kLink, "", deDE) == "Ihr erhaltet Beute: " + kLink + ".");
    p.count = 5;
    CHECK(it::itemPushLine(p, kLink, "", deDE) == "Ihr erhaltet Beute: " + kLink + "x5.");
    p.count = 1;
    p.received = true;
    CHECK(it::itemPushLine(p, kLink, "", deDE) == "Ihr bekommt einen Gegenstand: " + kLink + ".");
    p.created = true;
    CHECK(it::itemPushLine(p, kLink, "", deDE) == "Ihr stellt her: " + kLink + ".");
    p.showInChat = false;
    CHECK(it::itemPushLine(p, kLink, "", deDE).empty());
}

TEST_CASE("a party member's loot and creations are said, items handed to them are not", "[interface_text]") {
    it::ItemPush p;
    p.self = false;
    CHECK(it::itemPushLine(p, kLink, "Anna", deDE) == "Anna bekommt Beute: " + kLink + ".");
    p.created = true;
    p.count = 3;
    CHECK(it::itemPushLine(p, kLink, "Anna", deDE) == "Anna stellt her: " + kLink + "x3.");
    p.created = false;
    p.received = true;
    CHECK(it::itemPushLine(p, kLink, "Anna", deDE).empty());
}

TEST_CASE("roll lines follow the server's choice and die messages", "[interface_text]") {
    it::LootRoll r;
    r.self = true;
    r.rollNumber = 0;
    r.rollType = 0;  // need chosen
    CHECK(it::lootRollLine(r, kLink, "Ich", deDE) == "Ihr habt für " + kLink + " 'Bedarf' ausgewählt");
    r.self = false;
    r.rollNumber = -7;
    r.rollType = 2;  // greed chosen
    CHECK(it::lootRollLine(r, kLink, "Anna", deDE) == "Anna hat für " + kLink + " 'Gier' ausgewählt");
    r.rollNumber = -1;
    r.rollType = 0;  // passed
    CHECK(it::lootRollLine(r, kLink, "Anna", deDE) == "Anna würfelt nicht für: " + kLink);
    r.autoPass = true;
    r.female = true;
    CHECK(it::lootRollLine(r, kLink, "Anna", deDE) ==
          "Anna passt automatisch bei " + kLink + ", weil sie den Gegenstand nicht benutzen kann.");
    r.autoPass = false;
    r.rollNumber = 85;
    r.rollType = 1;  // the need die
    CHECK(it::lootRollLine(r, kLink, "Anna", deDE) == "Wurf für Bedarf: 85 für " + kLink + " von Anna");
}

TEST_CASE("without showLootSpam only one's own automatic pass is said", "[interface_text]") {
    it::LootRoll r;
    r.lootSpam = false;
    r.rollNumber = 85;
    r.rollType = 1;
    CHECK(it::lootRollLine(r, kLink, "Anna", deDE).empty());
    r.self = true;
    r.rollNumber = -1;
    r.rollType = 0;
    r.autoPass = true;
    CHECK(it::lootRollLine(r, kLink, "Ich", deDE) ==
          "Ihr passt automatisch bei " + kLink + ", weil Ihr den Gegenstand nicht benutzen könnt.");
}

TEST_CASE("the winner's line is plain with showLootSpam and names the roll without", "[interface_text]") {
    CHECK(it::lootRollWonLine(false, 85, 1, true, kLink, "Anna", deDE) == "Anna gewinnt: " + kLink);
    CHECK(it::lootRollWonLine(false, 85, 1, false, kLink, "Anna", deDE) ==
          "Anna gewinnt: " + kLink + " |cff818181(Bedarf - 85)|r");
    CHECK(it::lootRollWonLine(true, 42, 2, false, kLink, "Ich", deDE) ==
          "Ihr gewinnt: " + kLink + " |cff818181(Gier - 42)|r");
    CHECK(it::lootRollWonLine(true, 0, 0, false, kLink, "Ich", deDE).empty());
}

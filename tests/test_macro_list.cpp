// The macros as the interface numbers them (0x00566240), by index or by name
// (0x00566df0).
#include <catch_amalgamated.hpp>

#include "game/macro_cache.hpp"
#include "game/macro_list.hpp"

using namespace wowee::game::macro_list;

namespace {

// Ids in the order they were made.
constexpr uint32_t kTier = kCharacterBase + 1, kBau = kCharacterBase + 3;
const std::vector<Entry> kMacros = {{1, "Zorn"}, {2, "angriff"}, {3, "Heilen"}, {kTier, "Tier"}, {kBau, "Bau"}};

}  // namespace

TEST_CASE("each half is in the order of its names", "[macro]") {
    CHECK(order(kMacros, false) == std::vector<uint32_t>{2, 3, 1});
    CHECK(order(kMacros, true) == std::vector<uint32_t>{kBau, kTier});
    CHECK(idAt(kMacros, 1) == 2);
    CHECK(idAt(kMacros, 3) == 1);
    CHECK(idAt(kMacros, 4) == 0);
    CHECK(idAt(kMacros, 37) == kBau);
    CHECK(idAt(kMacros, 38) == kTier);
    CHECK(idAt(kMacros, 0) == 0);
    CHECK(idAt(kMacros, 73) == 0);
    CHECK(indexOf(kMacros, 1) == 3);
    CHECK(indexOf(kMacros, kTier) == 38);
    CHECK(indexOf(kMacros, 5) == 0);
}

TEST_CASE("a macro is named without case", "[macro]") {
    CHECK(byName(kMacros, "Angriff") == 2);
    CHECK(byName(kMacros, "BAU") == kBau);
    CHECK(byName(kMacros, "Heil") == 0);
    CHECK(byName(kMacros, "") == 0);
}

// A slot on the bar holds the id: deleting another macro left the slot with
// the one after it.
TEST_CASE("a macro gone takes no other's id", "[macro]") {
    std::vector<Entry> macros = kMacros;
    std::erase_if(macros, [](const Entry& m) { return m.id == 2; });
    CHECK(idAt(macros, 1) == 3);
    CHECK(indexOf(macros, 1) == 2);
    // The next new one has an id of its own, and an index by its name.
    CHECK(newId(macros, false) == 4);
    macros.push_back({4, "Mitte"});
    CHECK(indexOf(macros, 4) == 2);
    CHECK(indexOf(macros, 1) == 3);
    CHECK(newId(macros, true) == kBau + 1);
}

TEST_CASE("there are 36 and 18", "[macro]") {
    std::vector<Entry> macros;
    for (uint32_t i = 1; i <= kAccountMacros; ++i) macros.push_back({newId(macros, false), "m"});
    CHECK(macros.back().id == 36);
    CHECK(newId(macros, false) == 0);
    CHECK(newId(macros, true) == kCharacterBase + 1);
    for (uint32_t i = 1; i <= kCharacterMacros; ++i) macros.push_back({newId(macros, true), "c"});
    CHECK(newId(macros, true) == 0);
    CHECK(macros.back().id == kCharacterBase + 18);
    // An account's id is not held to its place.
    CHECK(newId({{112, "spaet"}}, false) == 113);
}

TEST_CASE("a file's ids are its half's", "[macro]") {
    CHECK(fileId(5, false) == 5);
    CHECK(fileId(112, false) == 112);
    CHECK(fileId(kCharacterBase + 1, false) == 0);
    CHECK(fileId(kCharacterBase + 1, true) == kCharacterBase + 1);
    CHECK(fileId(37, true) == kCharacterBase + 37);
}

// What this client put on the slots for a character's macro.
TEST_CASE("a slot of the old numbers holds the character's macro", "[macro]") {
    const auto character = [](uint32_t id) { return id == kCharacterBase + 37; };
    CHECK(slotId(37, character) == kCharacterBase + 37);
    CHECK(slotId(38, character) == 38);
    CHECK(slotId(1, character) == 1);
    CHECK(slotId(kCharacterBase + 37, character) == kCharacterBase + 37);
    const auto both = [](uint32_t id) { return id == 37 || id == kCharacterBase + 37; };
    CHECK(slotId(37, both) == 37);
}

TEST_CASE("macros-cache.txt is the client's", "[macro]") {
    namespace mc = wowee::game::macro_cache;
    // A character's, as the client writes it.
    const std::string text = "MACRO 16777217 \"Reiten und so\" Ability_Mount_Charger\r\n/cast Schreckensross\r\n"
                             "/in 5 /say x\r\nEND\r\nMACRO 16777230 \"Leer\" INV_Misc_QuestionMark\r\nEND\r\n";
    const auto read = mc::read(text, true);
    REQUIRE(read.size() == 2);
    CHECK(read[0].id == 16777217);
    CHECK(read[0].name == "Reiten und so");
    CHECK(read[0].icon == "Ability_Mount_Charger");
    CHECK(read[0].body == "/cast Schreckensross\n/in 5 /say x");
    CHECK(read[1].id == 16777230);
    CHECK(read[1].body.empty());
    CHECK(mc::write(read) == text);
    // Too low for it, as this client wrote them; the account's any number.
    CHECK(mc::read("MACRO 37 \"a\" x\nb\nEND\n", true).at(0).id == 16777216 + 37);
    CHECK(mc::read("MACRO 211 \"a\" x\nb\nEND\n", false).at(0).id == 211);
    CHECK(mc::read("MACRO 16777217 \"a\" x\nb\nEND\n", false).empty());
    CHECK(mc::read("MACRO 0 \"a\" x\nEND\nMACRO 4 x\nEND\nMACRO 5 \"\" x\nEND\n", false).empty());
    CHECK(mc::read("MACRO 4 \"a\" x\nEND\nMACRO 4 \"b\" x\nEND\n", false).size() == 1);
    std::string many;
    for (int i = 1; i <= 40; ++i) many += "MACRO " + std::to_string(i) + " \"m\" x\nEND\n";
    CHECK(mc::read(many, false).size() == 36);
}

TEST_CASE("macros: an older copy of the account's gives what is not there", "[macros]") {
    const std::vector<Entry> have{{1, "Heal"}, {2, "Run"}};
    // Of a name that is there, the account's own stays.
    CHECK(mergedId(have, {1, "heal"}) == 0);
    CHECK(mergedId(have, {7, "Run"}) == 0);
    // Its own id where nothing has it - the bar's slots hold that - or a new one.
    CHECK(mergedId(have, {7, "Fly"}) == 7);
    CHECK(mergedId(have, {2, "Fly"}) == 3);
    // The character's of that name is another half's.
    CHECK(mergedId({{kCharacterBase + 1, "Fly"}}, {1, "Fly"}) == 1);
    // A full half takes none.
    std::vector<Entry> full;
    for (uint32_t i = 1; i <= kAccountMacros; ++i) full.push_back({i, "m" + std::to_string(i)});
    CHECK(mergedId(full, {40, "Fly"}) == 0);
}

// The macros as the interface numbers them (0x00566240), by index or by name
// (0x00566df0).
#include <catch_amalgamated.hpp>

#include "game/macro_list.hpp"

using namespace wowee::game::macro_list;

namespace {

// Ids in the order they were made.
const std::vector<Entry> kMacros = {{1, "Zorn"}, {2, "angriff"}, {3, "Heilen"}, {37, "Tier"}, {39, "Bau"}};

}  // namespace

TEST_CASE("each half is in the order of its names", "[macro]") {
    CHECK(order(kMacros, false) == std::vector<uint32_t>{2, 3, 1});
    CHECK(order(kMacros, true) == std::vector<uint32_t>{39, 37});
    CHECK(idAt(kMacros, 1) == 2);
    CHECK(idAt(kMacros, 3) == 1);
    CHECK(idAt(kMacros, 4) == 0);
    CHECK(idAt(kMacros, 37) == 39);
    CHECK(idAt(kMacros, 38) == 37);
    CHECK(idAt(kMacros, 0) == 0);
    CHECK(idAt(kMacros, 73) == 0);
    CHECK(indexOf(kMacros, 1) == 3);
    CHECK(indexOf(kMacros, 37) == 38);
    CHECK(indexOf(kMacros, 5) == 0);
}

TEST_CASE("a macro is named without case", "[macro]") {
    CHECK(byName(kMacros, "Angriff") == 2);
    CHECK(byName(kMacros, "BAU") == 39);
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
    // The next new one has the free id, and an index by its name.
    CHECK(freeId(macros, false) == 2);
    macros.push_back({2, "Mitte"});
    CHECK(indexOf(macros, 2) == 2);
    CHECK(indexOf(macros, 1) == 3);
}

TEST_CASE("there are 36 and 18", "[macro]") {
    std::vector<Entry> macros;
    for (uint32_t i = 1; i <= kAccountMacros; ++i) macros.push_back({freeId(macros, false), "m"});
    CHECK(freeId(macros, false) == 0);
    CHECK(freeId(macros, true) == 37);
    for (uint32_t i = 1; i <= kCharacterMacros; ++i) macros.push_back({freeId(macros, true), "c"});
    CHECK(freeId(macros, true) == 0);
    CHECK(macros.back().id == 54);
}

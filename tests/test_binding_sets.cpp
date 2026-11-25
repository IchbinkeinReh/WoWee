// The sets of key bindings (addons/binding_sets.hpp).
#include <catch_amalgamated.hpp>

#include "addons/binding_sets.hpp"

using namespace wowee::addons;

TEST_CASE("an empty set of keys is the one below", "[binding_sets]") {
    CHECK(bindingSetRead(0, true, true) == 0);
    CHECK(bindingSetRead(1, true, true) == 1);
    CHECK(bindingSetRead(1, false, true) == 0);
    CHECK(bindingSetRead(2, true, true) == 2);
    CHECK(bindingSetRead(2, true, false) == 1);
    CHECK(bindingSetRead(2, false, false) == 0);
}

TEST_CASE("a character begins with its own keys where it has some", "[binding_sets]") {
    CHECK(bindingSetAtLogin(true) == 2);
    CHECK(bindingSetAtLogin(false) == 1);
    CHECK(bindingSetSaved(1) == 1);
    CHECK(bindingSetSaved(2) == 2);
    CHECK(bindingSetSaved(0) == 0);
    CHECK(bindingSetSaved(3) == 0);
}

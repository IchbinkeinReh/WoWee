// Reactive spells' announcements (game/reactive_spell.hpp).
#include <catch_amalgamated.hpp>

#include "game/reactive_spell.hpp"

using namespace wowee::game::reactive_spell;

// 0x0053cf10, 0x0053d1b0
TEST_CASE("a reactive spell says so as it comes to be castable", "[reactive]") {
    State s;
    CHECK_FALSE(powerChanged(s, false));        // not castable: the power says nothing
    CHECK_FALSE(powerChanged(s, true));
    CHECK_FALSE(s.shortOf);
    CHECK(rescanned(s, true, false));
    CHECK_FALSE(rescanned(s, true, false));     // once
    CHECK_FALSE(powerChanged(s, true));         // short of it
    CHECK(s.shortOf);
    CHECK_FALSE(powerChanged(s, true));
    CHECK(powerChanged(s, false));              // and back
    CHECK_FALSE(rescanned(s, true, true));
    CHECK(rescanned(s, true, false));
    CHECK_FALSE(rescanned(s, false, false));
    CHECK_FALSE(rescanned(s, false, true));
    CHECK_FALSE(rescanned(s, true, true));      // castable, but for the power
    CHECK(rescanned(s, true, false));
}

// The override bindings as 0x005603d0 and 0x00562490 keep and answer them.
#include <catch_amalgamated.hpp>

#include "addons/override_bindings.hpp"

using wowee::addons::OverrideBindings;

TEST_CASE("a key's overrides answer by priority, then the one set last", "[bindings]") {
    OverrideBindings b;
    int a = 0, c = 0;
    CHECK_FALSE(b.lookup("1"));
    b.set(&a, false, "1", "CLICK A:LeftButton");
    CHECK(b.lookup("1") == "CLICK A:LeftButton");
    b.set(&c, false, "1", "CLICK C:LeftButton");
    CHECK(b.lookup("1") == "CLICK C:LeftButton");
    b.set(&a, true, "1", "SPELL Fireball");
    CHECK(b.lookup("1") == "SPELL Fireball");
    b.set(&c, false, "1", "ITEM Hearthstone");  // set last, but not priority
    CHECK(b.lookup("1") == "SPELL Fireball");
}

TEST_CASE("an owner's override is replaced, taken away and cleared", "[bindings]") {
    OverrideBindings b;
    int a = 0, c = 0;
    b.set(&a, false, "shift-x", "JUMP");
    CHECK(b.lookup("SHIFT-X") == "JUMP");  // keys are not case-sensitive
    b.set(&a, false, "SHIFT-X", "SITORSTAND");
    CHECK(b.lookup("SHIFT-X") == "SITORSTAND");
    b.set(&c, false, "SHIFT-X", "TOGGLEAUTORUN");
    b.set(&c, false, "SHIFT-X", "");
    CHECK(b.lookup("SHIFT-X") == "SITORSTAND");
    b.set(&a, false, "Y", "JUMP");
    b.clear(&a);
    CHECK_FALSE(b.lookup("SHIFT-X"));
    CHECK_FALSE(b.lookup("Y"));
}

TEST_CASE("the mouselook override is one binding a key, nil takes it away", "[bindings]") {
    OverrideBindings b;
    b.setMouselook("BUTTON1", "MOVEFORWARD");
    CHECK(b.mouselook("button1") == "MOVEFORWARD");
    b.setMouselook("BUTTON1", "");
    CHECK_FALSE(b.mouselook("BUTTON1"));
}

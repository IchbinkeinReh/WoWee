// The binding commands the key dispatch carries out itself (0x00563150):
// SPELL, ITEM, MACRO and CLICK, as SetBindingSpell and its siblings write them.
#include <catch_amalgamated.hpp>

#include "addons/binding_command.hpp"

using wowee::addons::DirectBinding;
using wowee::addons::parseDirectBinding;

TEST_CASE("SPELL and ITEM name what they cast or use", "[binding_command]") {
    const DirectBinding spell = parseDirectBinding("SPELL Fireball");
    CHECK(spell.kind == DirectBinding::Kind::Spell);
    CHECK(spell.name == "Fireball");

    const DirectBinding item = parseDirectBinding("ITEM Hearthstone");
    CHECK(item.kind == DirectBinding::Kind::Item);
    CHECK(item.name == "Hearthstone");

    // Matched as _strnicmp matches it, the space included.
    CHECK(parseDirectBinding("spell Frostbolt").kind == DirectBinding::Kind::Spell);
    CHECK(parseDirectBinding("SPELLS").kind == DirectBinding::Kind::None);
}

TEST_CASE("a MACRO of digits is the macro's index", "[binding_command]") {
    const DirectBinding byIndex = parseDirectBinding("MACRO 12");
    CHECK(byIndex.kind == DirectBinding::Kind::Macro);
    CHECK(byIndex.macroByIndex);
    CHECK(byIndex.name == "12");

    const DirectBinding byName = parseDirectBinding("MACRO Pull 2");
    CHECK(byName.kind == DirectBinding::Kind::Macro);
    CHECK_FALSE(byName.macroByIndex);
    CHECK(byName.name == "Pull 2");
}

TEST_CASE("CLICK splits at the first colon", "[binding_command]") {
    const DirectBinding click = parseDirectBinding("CLICK MultiBarLeftButton3:RightButton");
    CHECK(click.kind == DirectBinding::Kind::Click);
    CHECK(click.clickable);
    CHECK(click.name == "MultiBarLeftButton3");
    CHECK(click.mouseButton == "RightButton");

    // No colon: still the client's own command, but nothing to click with.
    const DirectBinding bare = parseDirectBinding("CLICK SomeButton");
    CHECK(bare.kind == DirectBinding::Kind::Click);
    CHECK_FALSE(bare.clickable);
}

TEST_CASE("a Bindings.xml command is none of them", "[binding_command]") {
    CHECK(parseDirectBinding("ACTIONBUTTON1").kind == DirectBinding::Kind::None);
    CHECK(parseDirectBinding("TOGGLEBACKPACK").kind == DirectBinding::Kind::None);
    CHECK(parseDirectBinding("").kind == DirectBinding::Kind::None);
}

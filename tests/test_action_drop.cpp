// What the cursor holds against an action bar slot (UseAction 0x005abbc0,
// PickupAction 0x005abe70, the placing 0x005ab120) and against a click into
// the world (0x005278c0, 0x00527870, 0x0051fa50).

#include <catch_amalgamated.hpp>

#include "game/action_drop.hpp"

using namespace wowee::game::action_drop;

namespace {

constexpr Action kNothing{};
constexpr Action kSpell{Kind::Spell, 348};
constexpr Action kOtherSpell{Kind::Spell, 686};
constexpr Action kItem{Kind::Item, 6948};
constexpr Action kMacro{Kind::Macro, 1};
constexpr Action kSet{Kind::EquipmentSet, 0};

Facts knownSpell() {
    Facts f;
    f.spellKnown = true;
    return f;
}

}  // namespace

TEST_CASE("A click on a button with something held places it, its key uses it", "[action_drop]") {
    for (const Action& held : {kSpell, kItem, kMacro, kSet}) {
        CHECK(useActionPlaces(7, held, false));
        CHECK_FALSE(useActionPlaces(7, held, true));   // 0x00beadd8 + 0x12c
    }
    CHECK_FALSE(useActionPlaces(7, kNothing, false));
    // The bonus bar's slots are used whatever is held; the totem bar's, past
    // them, take a drop.
    CHECK(useActionPlaces(0x77, kSpell, false));
    CHECK_FALSE(useActionPlaces(0x78, kSpell, false));
    CHECK_FALSE(useActionPlaces(0x83, kSpell, false));
    CHECK(useActionPlaces(0x84, kSpell, false));
    CHECK_FALSE(useActionPlaces(0x90, kSpell, false));
    CHECK_FALSE(useActionPlaces(-1, kSpell, false));
}

TEST_CASE("A spell goes into a slot, the passive and the unknown do not", "[action_drop]") {
    CHECK(drop(7, kSpell, kNothing, knownSpell()) == Result::Place);
    CHECK(drop(7, kSpell, kOtherSpell, knownSpell()) == Result::Place);
    CHECK(drop(7, kSpell, kItem, knownSpell()) == Result::Place);
    CHECK(drop(7, kSpell, kNothing, Facts{}) == Result::Keep);
    Facts passive = knownSpell();
    passive.spellPassive = true;
    CHECK(drop(7, kSpell, kNothing, passive) == Result::PassiveSpell);
}

TEST_CASE("The slot's own action again is only put down", "[action_drop]") {
    CHECK(drop(7, kSpell, kSpell, knownSpell()) == Result::PutDown);
    Facts worn;
    worn.itemKnown = true;
    worn.itemUseSpell = true;
    CHECK(drop(7, kItem, kItem, worn) == Result::PutDown);
    CHECK(drop(7, kMacro, kMacro, Facts{}) == Result::PutDown);
    CHECK(drop(7, kSet, kSet, Facts{}) == Result::PutDown);
    // The same number of another kind is another action.
    CHECK(drop(7, Action{Kind::Macro, 5}, Action{Kind::EquipmentSet, 5}, Facts{}) == Result::Place);
    // One that is gone - a macro or a set deleted since - is written over.
    Facts gone;
    gone.slotActionThere = false;
    CHECK(drop(7, kMacro, kMacro, gone) == Result::Place);
}

TEST_CASE("An item goes into a slot when it is used or worn", "[action_drop]") {
    Facts f;
    CHECK(drop(0, kItem, kNothing, f) == Result::Keep);   // no Item.dbc row
    f.itemKnown = true;
    CHECK(drop(0, kItem, kNothing, f) == Result::Keep);   // cloth, a quest item
    f.itemInventoryType = 18;
    CHECK(drop(0, kItem, kNothing, f) == Result::Keep);   // a bag
    f.itemInventoryType = 13;
    CHECK(drop(0, kItem, kNothing, f) == Result::Place);  // a weapon
    f.itemInventoryType = 0;
    f.itemUseSpell = true;
    CHECK(drop(0, kItem, kNothing, f) == Result::Place);  // a potion
    f.itemInventoryType = 18;
    CHECK(drop(0, kItem, kNothing, f) == Result::Place);
}

TEST_CASE("A totem bar slot takes its element's totems alone", "[action_drop]") {
    Facts fire = knownSpell();
    fire.spellTotemCategories[0] = 4;
    CHECK(drop(0x84, kSpell, kNothing, fire) == Result::Place);
    CHECK(drop(0x85, kSpell, kNothing, fire) == Result::WrongTotem);
    CHECK(drop(0x84, kSpell, kNothing, knownSpell()) == Result::WrongTotem);
    CHECK(drop(0x84, kMacro, kNothing, Facts{}) == Result::WrongTotem);
    CHECK(drop(0x84, kSpell, kNothing, Facts{}) == Result::Keep);
    CHECK(drop(0x83, kMacro, kNothing, Facts{}) == Result::Place);
}

TEST_CASE("A left click into the world", "[action_drop]") {
    Held nothing;
    CHECK(leftClick(nothing, false) == WorldClick::Through);
    CHECK(leftClick(nothing, true) == WorldClick::Through);
    Held spell;
    spell.any = true;
    CHECK(leftClick(spell, false) == WorldClick::PutDown);
    // On a unit the click targets it and the spell stays.
    CHECK(leftClick(spell, true) == WorldClick::Through);
    Held item;
    item.any = item.ownItem = true;
    CHECK(leftClick(item, false) == WorldClick::ConfirmDelete);
    CHECK(leftClick(item, true) == WorldClick::Through);
    Held merchant;
    merchant.any = merchant.byEntry = true;
    CHECK(leftClick(merchant, false) == WorldClick::PutDown);
    CHECK(leftClick(merchant, true) == WorldClick::PutDownFirst);
}

TEST_CASE("The right button lets go of what is held", "[action_drop]") {
    Held nothing;
    CHECK_FALSE(rightPressPutsDown(nothing, false));
    CHECK(rightPressPutsDown(nothing, true));   // a gift wrap
    Held item;
    item.any = item.ownItem = true;
    CHECK(rightPressPutsDown(item, false));
    Held set;
    set.any = set.equipmentSet = true;
    CHECK_FALSE(rightPressPutsDown(set, false));
}

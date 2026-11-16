// The macro and item icon lists as 0x00565840 builds them: Ability_ and
// Spell_ for the macros, INV_ for the items, the question mark first, sorted
// without regard to case, case-only duplicates merged, .blp and .tga only.
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "addons/macro_icon_lists.hpp"

namespace mi = wowee::addons::macro_icons;

TEST_CASE("Icons split into the macro and the item list", "[macro_icons]") {
    const std::vector<std::string> files = {
        "spell_nature_heal.blp", "ability_ambush.blp", "inv_sword_04.blp",
        "inv_misc_questionmark.blp", "achievement_boss_01.blp", "Spell_Fire_Fire.tga",
        "ability_ambush.BLP", "spell_readme.txt", "inv_axe_01.blp",
    };
    const auto lists = mi::build(files);
    CHECK(lists.macro == std::vector<std::string>{"INV_Misc_QuestionMark", "ability_ambush",
                                                  "Spell_Fire_Fire", "spell_nature_heal"});
    CHECK(lists.item == std::vector<std::string>{"INV_Misc_QuestionMark", "inv_axe_01", "inv_sword_04"});
}

TEST_CASE("With no icons each list is the question mark alone", "[macro_icons]") {
    const auto lists = mi::build({});
    CHECK(lists.macro == std::vector<std::string>{"INV_Misc_QuestionMark"});
    CHECK(lists.item == std::vector<std::string>{"INV_Misc_QuestionMark"});
}

TEST_CASE("Only a .blp or a .tga names an icon", "[macro_icons]") {
    CHECK(mi::iconName("Ability_Foo.blp") == "Ability_Foo");
    CHECK(mi::iconName("Ability_Foo.TGA") == "Ability_Foo");
    CHECK(mi::iconName("Ability_Foo.bz") == "");
    CHECK(mi::iconName("Ability_Foo") == "");
}

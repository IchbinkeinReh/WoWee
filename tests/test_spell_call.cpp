// How a spell function's arguments name a spell (addons/spell_call.hpp), the
// spell of a name and a rank (game/spell_rank.hpp) and whose picture it has
// (game/weapon_icon.hpp).
#include <catch_amalgamated.hpp>

#include "addons/spell_call.hpp"
#include "game/spell_rank.hpp"
#include "game/weapon_icon.hpp"

using wowee::addons::SpellCall;
using wowee::addons::spellCallOf;

// 0x00540670
TEST_CASE("a spell function's arguments", "[spell_call]") {
    const auto number = [](int count, bool text2, bool takesId, bool takesName = true) {
        return spellCallOf(count, true, true, text2, false, takesId, takesName);
    };
    CHECK(number(1, false, true) == SpellCall::Id);
    CHECK(number(2, true, true) == SpellCall::Slot);
    CHECK(number(2, true, false) == SpellCall::Slot);
    // With a second argument that is none it is no id, and the name "13".
    CHECK(number(2, false, true) == SpellCall::Name);
    CHECK(number(1, false, false) == SpellCall::Name);
    CHECK(number(1, false, false, false) == SpellCall::None);
    CHECK(number(2, true, false, false) == SpellCall::Slot);
    const auto text = [](bool text2, bool unit, bool takesName = true) {
        return spellCallOf(text2 ? 2 : 1, false, true, text2, unit, true, takesName);
    };
    CHECK(text(false, false) == SpellCall::Name);
    CHECK(text(true, true) == SpellCall::Name);
    CHECK(text(true, false) == SpellCall::NameRank);
    CHECK(text(true, false, false) == SpellCall::None);
    CHECK(spellCallOf(0, false, false, false, false, true, true) == SpellCall::None);
    CHECK(spellCallOf(2, false, false, true, false, true, true) == SpellCall::None);
}

// 0x0053f5e0, 0x00540200
TEST_CASE("the spell of a name and a rank", "[spell_call]") {
    using namespace wowee::game::spell_rank;
    const std::vector<Candidate> known = {{686, "Schattenblitz", "Rang 1", 1}, {705, "Schattenblitz", "Rang 3", 12},
                                          {695, "Schattenblitz", "Rang 2", 6}, {5697, "Unendlicher Atem", "", 16},
                                          {7, "", "", 0}, {1, "Ansturm (Kriegsh\xc3\xbctte)", "", 1}};
    CHECK(spellByNameAndRank(known, "schattenblitz", "rang 2") == 695);
    CHECK(spellByNameAndRank(known, "Schattenblitz", "") == 705);
    CHECK(spellByNameAndRank(known, "Schattenblitz", "Rang 9") == 0);
    CHECK(spellByNameAndRank(known, "Feuerball", "") == 0);
    CHECK(spellByNameAndRank(known, "", "") == 0);
    CHECK(spellByName(known, "Schattenblitz(Rang 1)") == 686);
    CHECK(spellByName(known, "!Schattenblitz (Rang 2") == 695);
    CHECK(spellByName(known, "Schattenblitz()") == 705);
    CHECK(spellByName(known, "Ansturm (Kriegsh\xc3\xbctte)") == 1);
}

// 0x0053dc20, 0x005a84d0, 0x005a85e0
TEST_CASE("the attack's picture is the weapon's", "[spell_call]") {
    using namespace wowee::game;
    CHECK(spellIconOf(78, 0, 0) == SpellIcon::MainHand);
    CHECK(spellIconOf(58, 0x12, 0x20) == SpellIcon::Ranged);
    CHECK(spellIconOf(58, 0x2, 0) == SpellIcon::Own);
    CHECK(spellIconOf(58, 0, 0x20) == SpellIcon::Own);
    CHECK(attackIcon("", false, "sword") == "sword");
    CHECK(attackIcon("claw", false, "sword") == "claw");
    CHECK(attackIcon("", true, "sword") == "Interface\\Buttons\\Spell-Reset");
    CHECK(attackIcon("", false, "") == "Interface\\Buttons\\Spell-Reset");
    CHECK(rangedIcon(false, 2, "bow") == "bow");
    CHECK(rangedIcon(true, 2, "bow").empty());
    CHECK(rangedIcon(false, 16, "axe").empty());
}

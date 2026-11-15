// What a GameTooltip setter builds, and so which OnTooltipSet* script it ends
// with - decided as the client decides it (addons/tooltip_setter_rules.hpp).
//
// SetTradeSkillItem showed the recipe's name and description for a crafted
// item, an enchant and every reagent alike, and ended with no script: no
// OnTooltipSetItem for a crafted item or a reagent, no OnTooltipSetSpell for
// an enchant. These pin the choice 0x0062eae0 makes, the count it prices the
// item for (0x008016c0), and which spell tooltips end with their item's
// (0x006238a0).
#include <catch_amalgamated.hpp>

#include "addons/tooltip_setter_rules.hpp"

using namespace wowee::addons::tooltip_rules;

namespace {
constexpr uint32_t kCreateItem = 24;
constexpr uint32_t kCreateRandomItem = 59;
constexpr uint32_t kCreateItem2 = 157;
constexpr uint32_t kEnchantItem = 53;
constexpr uint32_t kDummy = 3;
}  // namespace

TEST_CASE("a recipe that makes an item is that item's tooltip", "[tooltip]") {
    // Linen Bandage (3275): CREATE_ITEM, item 1251, one made.
    const auto t = tradeSkillTooltip(kCreateItem, 1251, 1.0f, 1.0f, nullptr, 0, 0);
    CHECK(t.subject == TradeSkillSubject::CraftedItem);
    CHECK(t.itemId == 1251);
    CHECK(t.minCount == 1);
    CHECK(t.maxCount == 1);

    CHECK(tradeSkillTooltip(kCreateRandomItem, 7, 1.0f, 1.0f, nullptr, 0, 0).subject ==
          TradeSkillSubject::CraftedItem);
    CHECK(tradeSkillTooltip(kCreateItem2, 7, 1.0f, 1.0f, nullptr, 0, 0).subject ==
          TradeSkillSubject::CraftedItem);
}

TEST_CASE("a recipe that makes a range is priced for both ends", "[tooltip]") {
    // A transmute or a stack of arrows: the least and the most of the effect.
    const auto t = tradeSkillTooltip(kCreateItem, 2512, 200.0f, 200.0f, nullptr, 0, 0);
    CHECK(t.minCount == 200);
    const auto r = tradeSkillTooltip(kCreateItem, 6037, 1.0f, 3.0f, nullptr, 0, 0);
    CHECK(r.minCount == 1);
    CHECK(r.maxCount == 3);
}

TEST_CASE("an enchant, or a recipe naming no item, is the spell's tooltip", "[tooltip]") {
    // Enchant Bracer - Minor Health (7418): ENCHANT_ITEM, no item made.
    CHECK(tradeSkillTooltip(kEnchantItem, 0, 1.0f, 1.0f, nullptr, 0, 0).subject ==
          TradeSkillSubject::Spell);
    // An item-making effect with EffectItemType[0] zero is no item either.
    CHECK(tradeSkillTooltip(kCreateItem, 0, 1.0f, 1.0f, nullptr, 0, 0).subject ==
          TradeSkillSubject::Spell);
    // Only the first effect is asked (local_1ac is Effect[0]).
    CHECK(tradeSkillTooltip(kDummy, 1251, 1.0f, 1.0f, nullptr, 0, 0).subject ==
          TradeSkillSubject::Spell);
}

TEST_CASE("a reagent is counted over the slots that name an item", "[tooltip]") {
    const Reagent slots[8] = {{2589, 2}, {0, 0}, {2592, 1}, {}, {}, {}, {}, {}};
    const auto first = tradeSkillTooltip(kCreateItem, 1251, 1.0f, 1.0f, slots, 8, 1);
    CHECK(first.subject == TradeSkillSubject::Reagent);
    CHECK(first.itemId == 2589);
    CHECK(first.minCount == 2);
    CHECK(first.maxCount == 0);
    // The empty second slot is passed over, not counted.
    const auto second = tradeSkillTooltip(kCreateItem, 1251, 1.0f, 1.0f, slots, 8, 2);
    CHECK(second.subject == TradeSkillSubject::Reagent);
    CHECK(second.itemId == 2592);
    CHECK(second.minCount == 1);
    // A reagent of an enchant is still the reagent's item.
    CHECK(tradeSkillTooltip(kEnchantItem, 0, 1.0f, 1.0f, slots, 8, 1).subject ==
          TradeSkillSubject::Reagent);
    // Past the last: the client's error.
    CHECK(tradeSkillTooltip(kCreateItem, 1251, 1.0f, 1.0f, slots, 8, 3).subject ==
          TradeSkillSubject::Invalid);
    CHECK(tradeSkillTooltip(kCreateItem, 1251, 1.0f, 1.0f, slots, 8, -1).subject ==
          TradeSkillSubject::Invalid);
}

TEST_CASE("the count made rounds as 0x008016c0 does", "[tooltip]") {
    CHECK(craftedCount(1.0f) == 1);
    CHECK(craftedCount(1.25f) == 1);
    CHECK(craftedCount(1.5f) == 2);
    CHECK(craftedCount(1.75f) == 2);
    CHECK(craftedCount(2.0f) == 2);
    CHECK(craftedCount(5.0f) == 5);
    CHECK(craftedCount(0.0f) == 0);
}

TEST_CASE("a trade spell ends with the tooltip of the item it makes", "[tooltip]") {
    const uint32_t makes[3] = {kCreateItem, 0, 0};
    CHECK(spellTooltipAppendsItem(0x20, makes, 1251));
    // Not a trade spell, or no item named: the spell alone.
    CHECK_FALSE(spellTooltipAppendsItem(0x0, makes, 1251));
    CHECK_FALSE(spellTooltipAppendsItem(0x20, makes, 0));
    // An item made by a later effect counts...
    const uint32_t later[3] = {kDummy, kCreateItem2, 0};
    CHECK(spellTooltipAppendsItem(0x20, later, 1251));
    // ...unless ENCHANT_ITEM comes first: that stops the walk.
    const uint32_t enchantFirst[3] = {kEnchantItem, kCreateItem, 0};
    CHECK_FALSE(spellTooltipAppendsItem(0x20, enchantFirst, 1251));
    const uint32_t enchantAfter[3] = {kCreateItem, kEnchantItem, 0};
    CHECK(spellTooltipAppendsItem(0x20, enchantAfter, 1251));
}

// ── An action-bar macro, hovered (0x00630d20) ───────────────────────────────
// A macro on the bar answered nothing at all. The client describes what a
// #showtooltip macro shows - its item first, then its spell - and names any
// other macro in white.

TEST_CASE("a #showtooltip macro is its item's tooltip before its spell's", "[tooltip][macro]") {
    CHECK(macroActionTooltip(true, 6948, 8690) == MacroTooltip::Item);
    CHECK(macroActionTooltip(true, 6948, 0) == MacroTooltip::Item);
}

TEST_CASE("a #showtooltip macro with no item is its spell's tooltip", "[tooltip][macro]") {
    CHECK(macroActionTooltip(true, 0, 133) == MacroTooltip::Spell);
    // A command that named nothing (-1) still goes to the spell builder,
    // which refuses it and hides the tooltip.
    CHECK(macroActionTooltip(true, 0, -1) == MacroTooltip::Spell);
}

TEST_CASE("a macro without #showtooltip, or showing nothing, is its name", "[tooltip][macro]") {
    CHECK(macroActionTooltip(false, 6948, 0) == MacroTooltip::Name);
    CHECK(macroActionTooltip(false, 0, 133) == MacroTooltip::Name);
    CHECK(macroActionTooltip(true, 0, 0) == MacroTooltip::Name);
}

// ── The aura filter (0x00608390) ────────────────────────────────────────────

TEST_CASE("no aura filter counts buffs", "[tooltip][aura]") {
    CHECK(auraFilterBits(nullptr) == kAuraHelpful);
    CHECK(auraFilterBits("") == kAuraHelpful);
    CHECK(auraFilterBits("  | ") == kAuraHelpful);
}

TEST_CASE("HELPFUL and HARMFUL replace each other, the last one winning", "[tooltip][aura]") {
    CHECK(auraFilterBits("HARMFUL") == kAuraHarmful);
    CHECK(auraFilterBits("harmful") == kAuraHarmful);
    CHECK(auraFilterBits("HARMFUL|HELPFUL") == kAuraHelpful);
    CHECK(auraFilterBits("HELPFUL HARMFUL") == kAuraHarmful);
}

TEST_CASE("the other filter words add their bit and unknown words are passed over", "[tooltip][aura]") {
    CHECK(auraFilterBits("PLAYER") == (kAuraHelpful | kAuraPlayer));
    CHECK(auraFilterBits("HARMFUL|RAID") == (kAuraHarmful | kAuraRaid));
    CHECK(auraFilterBits("CANCELABLE") == (kAuraHelpful | kAuraCancelable));
    CHECK(auraFilterBits("NOT_CANCELABLE") == (kAuraHelpful | kAuraNotCancelable));
    CHECK(auraFilterBits("HELPFULX|PLAYERS") == kAuraHelpful);
    CHECK(auraFilterBits("Player|harmful") == (kAuraHarmful | kAuraPlayer));
}

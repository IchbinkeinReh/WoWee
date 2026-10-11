// The stance bar's three questions, which have to be about the same list.
//
// FrameXML builds the bar by walking 1..GetNumShapeshiftForms() and asking
// GetShapeshiftFormInfo about each index, then binding CastShapeshiftForm to
// the same index. Those were three separate lists: the count walked eight
// druid forms, the other two a fixed six in a different order. A druid who had
// learned Aquatic Form was counted a button the bar could not describe, and
// every index past the first mismatch showed one form and cast another.
//
// The oracle is retail's contract rather than the old code: the i-th form is
// the i-th form *the character has*, and all three bindings mean the same i.
#include <catch_amalgamated.hpp>

#include <set>
#include <string>
#include <vector>

#include "game/protocol_constants.hpp"
#include "game/shapeshift_forms.hpp"
#include "ui/action_bar_panel.hpp"

using wowee::game::allShapeshiftForms;
using wowee::game::knownShapeshiftForms;

namespace {

constexpr uint8_t kDruid = 11;
constexpr uint8_t kPriest = 5;
constexpr uint8_t kMage = 8;

}  // namespace

TEST_CASE("a form the character has not learned is not on the bar",
          "[shapeshift]") {
    // The case that named this: a level 14 priest was offered Shadowform,
    // which is learned at 40.
    const std::set<uint32_t> nothing;
    CHECK(knownShapeshiftForms(kPriest, nothing).empty());

    const std::set<uint32_t> shadow{wowee::game::SPELL_SHADOWFORM};
    REQUIRE(knownShapeshiftForms(kPriest, shadow).size() == 1);
    CHECK(std::string(knownShapeshiftForms(kPriest, shadow)[0].name) == "Shadowform");
}

TEST_CASE("the forms keep bar order, not the order they were learned",
          "[shapeshift]") {
    // Cat is third in the table and Bear first, whichever the player learned
    // first. An index means a position on the bar.
    const std::set<uint32_t> known{wowee::game::SPELL_CAT_FORM,
                                   wowee::game::SPELL_BEAR_FORM};
    const auto forms = knownShapeshiftForms(kDruid, known);
    REQUIRE(forms.size() == 2);
    CHECK(forms[0].spellId == wowee::game::SPELL_BEAR_FORM);
    CHECK(forms[1].spellId == wowee::game::SPELL_CAT_FORM);
}

TEST_CASE("Aquatic and Flight Form are forms", "[shapeshift]") {
    // Both were in the list the count used and in neither of the other two, so
    // a druid who had them was counted buttons that described nothing.
    const std::set<uint32_t> known{wowee::game::SPELL_BEAR_FORM,
                                   wowee::game::SPELL_AQUATIC_FORM,
                                   wowee::game::SPELL_FLIGHT_FORM};
    const auto forms = knownShapeshiftForms(kDruid, known);
    REQUIRE(forms.size() == 3);
    CHECK(forms[1].spellId == wowee::game::SPELL_AQUATIC_FORM);
    CHECK(forms[2].spellId == wowee::game::SPELL_FLIGHT_FORM);
}

TEST_CASE("every index the count allows describes a form", "[shapeshift]") {
    // The invariant the bar depends on, stated directly: for any set of known
    // spells, walking 1..count and indexing the same list never runs off the
    // end, because they are the same list.
    const std::set<uint32_t> known{
        wowee::game::SPELL_BEAR_FORM, wowee::game::SPELL_AQUATIC_FORM,
        wowee::game::SPELL_CAT_FORM, wowee::game::SPELL_TRAVEL_FORM,
        wowee::game::SPELL_MOONKIN_FORM, wowee::game::SPELL_TREE_OF_LIFE,
        wowee::game::SPELL_FLIGHT_FORM, wowee::game::SPELL_SWIFT_FLIGHT};

    const auto forms = knownShapeshiftForms(kDruid, known);
    CHECK(forms.size() == 8);
    for (size_t i = 0; i < forms.size(); ++i) {
        INFO("index " << (i + 1));
        CHECK(forms[i].spellId != 0);
        CHECK(forms[i].name != nullptr);
        CHECK(forms[i].icon != nullptr);
    }
}

TEST_CASE("every form has a distinct form id and spell", "[shapeshift]") {
    // The form id is what "is this one active" compares against, so two forms
    // sharing one would both light up. Dire Bear is deliberately not in the
    // table for exactly this reason: it shares Bear's slot.
    for (uint8_t classId : {uint8_t{1}, uint8_t{4}, kPriest, uint8_t{6}, kDruid}) {
        std::set<uint8_t> formIds;
        std::set<uint32_t> spellIds;
        for (const auto& form : allShapeshiftForms(classId)) {
            INFO("class " << int(classId) << " form " << form.name);
            CHECK(formIds.insert(form.formId).second);
            CHECK(spellIds.insert(form.spellId).second);
        }
    }
}

TEST_CASE("a class with no stance bar has no forms", "[shapeshift]") {
    CHECK(allShapeshiftForms(kMage).empty());
    const std::set<uint32_t> known{wowee::game::SPELL_BEAR_FORM};
    CHECK(knownShapeshiftForms(kMage, known).empty());
}

// ── The form's bar, its active flag and its events (WotLK) ──────────────────

using wowee::game::activeShapeshiftIndex;
using wowee::game::bonusBarOffset;
using wowee::game::bonusBarOffsetEvents;
using wowee::game::formChangeEvents;
using wowee::game::formSpellActive;
using wowee::game::ShapeshiftFormRow;

namespace {

ShapeshiftFormRow rowWithBar(uint32_t bar) {
    ShapeshiftFormRow row;
    row.bonusActionBar = bar;
    return row;
}

struct TestAura {
    uint32_t spellId;
    uint8_t flags;
};

std::vector<std::string> names(const std::vector<const char*>& events) {
    return {events.begin(), events.end()};
}

}  // namespace

TEST_CASE("a form's bonus bar offset is its SpellShapeshiftForm row's", "[shapeshift]") {
    // 0x005a83c0: no form, no bar; a stance its BonusActionBar column - the
    // warrior's three are 1, 2 and 3, which FrameXML adds to page 6.
    CHECK(bonusBarOffset(false, nullptr) == 0);
    const auto battle = rowWithBar(1), defensive = rowWithBar(2), berserker = rowWithBar(3);
    CHECK(bonusBarOffset(false, &battle) == 1);
    CHECK(bonusBarOffset(false, &defensive) == 2);
    CHECK(bonusBarOffset(false, &berserker) == 3);
    const auto travel = rowWithBar(0);
    CHECK(bonusBarOffset(false, &travel) == 0);
}

TEST_CASE("the pet's actions or a form's preset spells take the bar to 5", "[shapeshift]") {
    const auto cat = rowWithBar(1);
    CHECK(bonusBarOffset(true, &cat) == 5);
    CHECK(bonusBarOffset(true, nullptr) == 5);
    // 0x005a7c50: any PresetSpellID makes it the pet's page (0x005abac0).
    auto preset = rowWithBar(2);
    preset.presetSpells[3] = 12345;
    CHECK(bonusBarOffset(false, &preset) == 5);
}

TEST_CASE("a form change fires the bar's events, then the form's", "[shapeshift]") {
    // 0x006da130: 0x0053cf10 and 0x0053b480 first, whatever the bar does.
    const std::vector<std::string> head{"SPELL_UPDATE_USABLE", "ACTIONBAR_UPDATE_STATE",
                                        "CURRENT_SPELL_CAST_CHANGED"};
    auto with = [&](std::vector<std::string> tail) {
        std::vector<std::string> all = head;
        all.insert(all.end(), tail.begin(), tail.end());
        return all;
    };
    // 0x005a83c0: Battle Stance from none moves the offset.
    CHECK(names(formChangeEvents(0, 1)) == with({"UPDATE_BONUS_ACTIONBAR", "UPDATE_SHAPESHIFT_FORM"}));
    // Two forms on the same bar: only the form's event.
    CHECK(names(formChangeEvents(1, 1)) == with({"UPDATE_SHAPESHIFT_FORM"}));
    // Leaving the pet's page changes the page as well.
    CHECK(names(formChangeEvents(5, 0)) ==
          with({"ACTIONBAR_PAGE_CHANGED", "UPDATE_BONUS_ACTIONBAR", "UPDATE_SHAPESHIFT_FORM"}));
    CHECK(bonusBarOffsetEvents(3, 3).empty());
    // The list's own event is not one of them: the forms did not change.
    for (const char* event : formChangeEvents(0, 2)) CHECK(std::string(event) != "UPDATE_SHAPESHIFT_FORMS");
}

TEST_CASE("a form is active while its spell's positive aura is on the player", "[shapeshift]") {
    // 0x00802cb0: the aura of the spell, with flag 0x10.
    const std::vector<TestAura> auras{{wowee::game::SPELL_DEFENSIVE_STANCE, 0x10 | 0x01}, {1234, 0x10}};
    CHECK(formSpellActive(auras, wowee::game::SPELL_DEFENSIVE_STANCE));
    CHECK_FALSE(formSpellActive(auras, wowee::game::SPELL_BATTLE_STANCE));
    CHECK_FALSE(formSpellActive(auras, 0));
    const std::vector<TestAura> negative{{wowee::game::SPELL_BATTLE_STANCE, 0x80}};
    CHECK_FALSE(formSpellActive(negative, wowee::game::SPELL_BATTLE_STANCE));
}

TEST_CASE("a form whose cancel is pending is not in effect", "[shapeshift]") {
    // 0x00802cb0: +0x9f4 set and the spell a shapeshift - its aura is still
    // on until the server takes it off.
    const std::vector<TestAura> auras{{wowee::game::SPELL_CAT_FORM, 0x10}};
    CHECK(formSpellActive(auras, wowee::game::SPELL_CAT_FORM, false));
    CHECK_FALSE(formSpellActive(auras, wowee::game::SPELL_CAT_FORM, true));
}

TEST_CASE("cancelling the form's aura leaves the form before the server does", "[shapeshift]") {
    using wowee::game::FormAuraCancel;
    using wowee::game::formAuraCancel;
    // 0x00802f80. Cat Form: aura 36 naming form 1; the second effect is a
    // plain aura.
    const uint32_t catAuras[3] = {36, 4, 0};
    const uint32_t catMisc[3] = {1, 0, 0};
    ShapeshiftFormRow cat = rowWithBar(1);
    ShapeshiftFormRow noCancel = rowWithBar(0);
    noCancel.flags = 0x2;
    auto rows = [&](uint8_t form) -> const ShapeshiftFormRow* {
        if (form == 1) return &cat;
        if (form == 2) return &noCancel;
        if (form == 5) return &cat;
        return nullptr;
    };
    // In Cat Form: left at once.
    CHECK(formAuraCancel(1, catAuras, catMisc, rows) == FormAuraCancel::SendFormPending);
    // No form in effect - none, or a cancel already pending: only the packet.
    CHECK(formAuraCancel(0, catAuras, catMisc, rows) == FormAuraCancel::Send);
    // In another form: the aura goes, the form stays.
    CHECK(formAuraCancel(5, catAuras, catMisc, rows) == FormAuraCancel::Send);
    // A spell that shapeshifts nobody.
    const uint32_t plain[3] = {4, 0, 0};
    CHECK(formAuraCancel(1, plain, catMisc, rows) == FormAuraCancel::Send);
    // A form flagged no-cancel, or with no row: nothing is sent.
    const uint32_t lockedMisc[3] = {2, 0, 0};
    CHECK(formAuraCancel(1, catAuras, lockedMisc, rows) == FormAuraCancel::Refused);
    const uint32_t unknownMisc[3] = {40, 0, 0};
    CHECK(formAuraCancel(1, catAuras, unknownMisc, rows) == FormAuraCancel::Refused);
}

TEST_CASE("leaving a druid form for none or a stance plays its sound", "[shapeshift]") {
    using wowee::game::formLeaveSound;
    using wowee::game::inShapeshiftForm;
    // 0x00721ca0: a form is not none, not a stance (Flags 0x1), and has a row.
    ShapeshiftFormRow cat = rowWithBar(1);
    cat.flags = 0x2f8;
    ShapeshiftFormRow stance = rowWithBar(1);
    stance.flags = 0x7;
    CHECK(inShapeshiftForm(1, &cat));
    CHECK_FALSE(inShapeshiftForm(17, &stance));
    CHECK_FALSE(inShapeshiftForm(0, nullptr));
    CHECK_FALSE(inShapeshiftForm(40, nullptr));
    // 0x006da130: the old form one of the eight, the new one none or a stance.
    for (uint8_t form : {1, 2, 3, 4, 5, 8, 0x1d, 0x1f}) {
        CHECK(formLeaveSound(form, false));
        CHECK_FALSE(formLeaveSound(form, true));
    }
    // Swift Flight Form, a stance, Shadowform, Stealth, none: silent.
    for (uint8_t form : {0, 0x1b, 17, 28, 30}) CHECK_FALSE(formLeaveSound(form, false));
    CHECK(wowee::game::kSoundFormLeave == 10063);
}

TEST_CASE("a form without Flags 0x8 closes the NPC's window", "[shapeshift]") {
    using wowee::game::formClosesInteraction;
    // SpellShapeshiftForm.dbc 3.3.5a: cat 0x2f8, Spirit of Redemption (32)
    // 0x4100, Shadowform 0x9, Battle Stance 0x7.
    ShapeshiftFormRow cat, redemption, shadow, battle;
    cat.flags = 0x2f8;
    redemption.flags = 0x4100;
    shadow.flags = 0x9;
    battle.flags = 0x7;
    CHECK_FALSE(formClosesInteraction(1, &cat));
    CHECK(formClosesInteraction(32, &redemption));
    // A stance is no form to 0x00721ca0, flag or not.
    CHECK_FALSE(formClosesInteraction(28, &shadow));
    CHECK_FALSE(formClosesInteraction(17, &battle));
    CHECK_FALSE(formClosesInteraction(0, nullptr));
}

TEST_CASE("GetShapeshiftForm is the active form's place on the bar", "[shapeshift]") {
    // 0x0053d4b0: searched from the end, 1-based.
    CHECK(activeShapeshiftIndex({false, true, false}, 18) == 2);
    CHECK(activeShapeshiftIndex({false, false, false}, 0) == 0);
    // A form field set but none of the bar's: one past the bar.
    CHECK(activeShapeshiftIndex({false, false, false}, 7) == 4);
}

TEST_CASE("Dire Bear Form takes Bear Form's button", "[shapeshift]") {
    // 0x005424d4: the list's spell is replaced in place.
    const std::set<uint32_t> known{wowee::game::SPELL_BEAR_FORM, wowee::game::SPELL_DIRE_BEAR_FORM,
                                   wowee::game::SPELL_CAT_FORM};
    const auto forms = knownShapeshiftForms(kDruid, known);
    REQUIRE(forms.size() == 2);
    CHECK(forms[0].spellId == wowee::game::SPELL_DIRE_BEAR_FORM);
    CHECK(std::string(forms[0].name) == "Dire Bear Form");
    CHECK(forms[1].spellId == wowee::game::SPELL_CAT_FORM);
}

TEST_CASE("a stance's number keys press its own bar", "[actionbar]") {
    // ActionButtonDown takes the bonus button while the bonus bar is up; on
    // page 1 it reads page 6 + GetBonusBarOffset(). 0-based slots.
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(1, 0, 0) == 0);    // no stance: slot 1
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(1, 1, 0) == 72);   // Battle Stance: slot 73
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(1, 2, 11) == 95);  // Defensive: slot 96
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(1, 3, 0) == 96);   // Berserker: slot 97
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(1, 5, 0) == 120);  // the pet's page: slot 121
    // A page turned by hand is not the bonus bar's.
    CHECK(wowee::ui::ActionBarPanel::mainBarSlot(2, 1, 0) == 12);
}

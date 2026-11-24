#include "game/shapeshift_forms.hpp"

#include "game/protocol_constants.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace wowee::game {
namespace {

// The forms before WotLK, whose spell tables have no stance bar columns here.
// The order is the order the bar shows them and every index refers to.
//
// Dire Bear is not a row of its own. It replaces Bear Form on the same button
// (0x005424d4 swaps the list's spell in place), so the button then holds and
// casts Dire Bear Form and is active while its aura is.
//
// The form ids are SpellShapeshiftForm.dbc's (column 0), which is what each
// form spell's aura 36 names: Cat 1, Tree of Life 2, Travel 3, Aquatic 4,
// Bear 5 (Dire Bear 8), Swift Flight 27, Flight 29, Moonkin 31.
const ShapeshiftForm kDruid[] = {
    {.spellId = SPELL_BEAR_FORM,    .formId = 5,  .name = "Bear Form",         .icon = "Interface\\Icons\\Ability_Racial_BearForm",
     .supersededBy = SPELL_DIRE_BEAR_FORM, .supersededName = "Dire Bear Form"},
    {.spellId = SPELL_AQUATIC_FORM, .formId = 4,  .name = "Aquatic Form",      .icon = "Interface\\Icons\\Ability_Druid_AquaticForm"},
    {.spellId = SPELL_CAT_FORM,     .formId = 1,  .name = "Cat Form",          .icon = "Interface\\Icons\\Ability_Druid_CatForm"},
    {.spellId = SPELL_TRAVEL_FORM,  .formId = 3,  .name = "Travel Form",       .icon = "Interface\\Icons\\Ability_Druid_TravelForm"},
    {.spellId = SPELL_MOONKIN_FORM, .formId = 31, .name = "Moonkin Form",      .icon = "Interface\\Icons\\Spell_Nature_ForceOfNature"},
    {.spellId = SPELL_TREE_OF_LIFE, .formId = 2,  .name = "Tree of Life",      .icon = "Interface\\Icons\\Ability_Druid_TreeofLife"},
    {.spellId = SPELL_FLIGHT_FORM,  .formId = 29, .name = "Flight Form",       .icon = "Interface\\Icons\\Ability_Druid_FlightForm"},
    {.spellId = SPELL_SWIFT_FLIGHT, .formId = 27, .name = "Swift Flight Form", .icon = "Interface\\Icons\\Ability_Druid_FlightForm"},
};

const ShapeshiftForm kWarrior[] = {
    {.spellId = SPELL_BATTLE_STANCE,    .formId = 17, .name = "Battle Stance",    .icon = "Interface\\Icons\\Ability_Warrior_OffensiveStance"},
    {.spellId = SPELL_DEFENSIVE_STANCE, .formId = 18, .name = "Defensive Stance", .icon = "Interface\\Icons\\Ability_Warrior_DefensiveStance"},
    {.spellId = SPELL_BERSERKER_STANCE, .formId = 19, .name = "Berserker Stance", .icon = "Interface\\Icons\\Ability_Racial_Avatar"},
};

// Presences are no form: their spells apply no aura 36 and the DBC has no
// row for them, so the form field stays 0.
const ShapeshiftForm kDeathKnight[] = {
    {.spellId = SPELL_BLOOD_PRESENCE,  .formId = 0,  .name = "Blood Presence",  .icon = "Interface\\Icons\\Spell_Deathknight_BloodPresence"},
    {.spellId = SPELL_FROST_PRESENCE,  .formId = 0,  .name = "Frost Presence",  .icon = "Interface\\Icons\\Spell_Deathknight_FrostPresence"},
    {.spellId = SPELL_UNHOLY_PRESENCE, .formId = 0,  .name = "Unholy Presence", .icon = "Interface\\Icons\\Spell_Deathknight_UnholyPresence"},
};

const ShapeshiftForm kRogue[] = {
    {.spellId = SPELL_STEALTH, .formId = 30, .name = "Stealth", .icon = "Interface\\Icons\\Ability_Stealth"},
};

const ShapeshiftForm kPriest[] = {
    {.spellId = SPELL_SHADOWFORM, .formId = 28, .name = "Shadowform", .icon = "Interface\\Icons\\Spell_Shadow_Shadowform"},
};

}  // namespace

std::vector<ShapeshiftForm> allShapeshiftForms(uint8_t classId) {
    switch (classId) {
        case 1:  return {std::begin(kWarrior), std::end(kWarrior)};
        case 4:  return {std::begin(kRogue), std::end(kRogue)};
        case 5:  return {std::begin(kPriest), std::end(kPriest)};
        case 6:  return {std::begin(kDeathKnight), std::end(kDeathKnight)};
        case 11: return {std::begin(kDruid), std::end(kDruid)};
        default: return {};
    }
}

bool onStanceBar(const StanceBarRow& row) {
    if ((row.attrEx2 & kAttrEx2NoStanceBar) != 0) return false;
    if ((row.attrEx2 & kAttrEx2StanceBar) != 0) return true;
    return std::find(std::begin(row.effectAuras), std::end(row.effectAuras), kAuraModShapeshift) !=
           std::end(row.effectAuras);
}

int StanceBar::compare(const Entry& a, const Entry& b) {
    // 0x0053c680.
    if (a.row.order < 0 && b.row.order < 0) return a.spellId >= b.spellId ? 1 : -1;
    if (a.row.order == -1) return 1;
    if (b.row.order == -1) return -1;
    if (a.row.order == b.row.order) return 0;
    return a.row.order >= b.row.order ? 1 : -1;
}

void StanceBar::sort() {
    // The client's qsort (0x0040be50) sorts up to eight this way (0x0040bdc0):
    // the greatest, the first of equals, to the end. Two of one order - Flight
    // Form and its swift one, both known - therefore change places each time.
    for (size_t end = entries_.size(); end > 1; --end) {
        size_t greatest = 0;
        for (size_t i = 1; i < end; ++i)
            if (compare(entries_[i], entries_[greatest]) > 0) greatest = i;
        std::swap(entries_[greatest], entries_[end - 1]);
    }
}

bool StanceBar::learned(uint32_t spellId, const StanceBarRow& row) {
    if (!onStanceBar(row)) return false;
    for (const Entry& e : entries_)
        if (e.spellId == spellId) return false;
    entries_.push_back({spellId, row});
    sort();
    return true;
}

bool StanceBar::superseded(uint32_t oldSpellId, uint32_t spellId, const StanceBarRow& row) {
    if (!onStanceBar(row)) return false;
    for (Entry& e : entries_) {
        if (e.spellId != oldSpellId) continue;
        e = {spellId, row};
        return true;
    }
    return false;
}

bool StanceBar::removed(uint32_t spellId) {
    for (size_t i = entries_.size(); i > 0; --i) {
        if (entries_[i - 1].spellId != spellId) continue;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return true;
    }
    return false;
}

std::vector<uint32_t> StanceBar::spells() const {
    std::vector<uint32_t> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.spellId);
    return out;
}

bool formHasPresetSpells(const ShapeshiftFormRow& row) {
    for (uint32_t spell : row.presetSpells) {
        if (spell != 0) return true;
    }
    return false;
}

bool inShapeshiftForm(uint8_t form, const ShapeshiftFormRow* row) {
    return form != 0 && row && (row->flags & kFormFlagStance) == 0;
}

bool formClosesInteraction(uint8_t form, const ShapeshiftFormRow* row) {
    return inShapeshiftForm(form, row) && (row->flags & kFormFlagKeepsInteraction) == 0;
}

bool formLeaveSound(uint8_t formBefore, bool inFormAfter) {
    if (inFormAfter) return false;
    switch (formBefore) {
        case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x08: case 0x1d: case 0x1f:
            return true;
        default:
            return false;
    }
}

uint32_t bonusBarOffset(bool petActionsOnBar, const ShapeshiftFormRow* form) {
    // 0x005a83c0: DAT_00c1e5a0 set (the pet's actions, bit 1, or a form's
    // preset spells, bit 2 from 0x005abac0) is the pet's page.
    if (petActionsOnBar) return 5;
    if (!form) return 0;
    if (formHasPresetSpells(*form)) return 5;
    return form->bonusActionBar;
}

std::vector<const char*> bonusBarOffsetEvents(uint32_t offsetBefore, uint32_t offsetAfter) {
    std::vector<const char*> events;
    if (offsetBefore == offsetAfter) return events;
    if (offsetBefore == 5) events.push_back("ACTIONBAR_PAGE_CHANGED");  // event 0xaf
    events.push_back("UPDATE_BONUS_ACTIONBAR");                         // event 0xb4
    return events;
}

std::vector<const char*> formChangeEvents(uint32_t offsetBefore, uint32_t offsetAfter) {
    std::vector<const char*> events{
        "SPELL_UPDATE_USABLE",         // event 0xf5, 0x0053cf10
        "ACTIONBAR_UPDATE_STATE",      // event 0xb1, 0x0053b480
        "CURRENT_SPELL_CAST_CHANGED",  // event 0xf3
    };
    for (const char* event : bonusBarOffsetEvents(offsetBefore, offsetAfter)) events.push_back(event);
    events.push_back("UPDATE_SHAPESHIFT_FORM");  // event 0x179
    return events;
}

uint32_t activeShapeshiftIndex(const std::vector<bool>& activeByIndex, uint8_t formField) {
    for (size_t i = activeByIndex.size(); i > 0; --i) {
        if (activeByIndex[i - 1]) return static_cast<uint32_t>(i);
    }
    return formField != 0 ? static_cast<uint32_t>(activeByIndex.size() + 1) : 0;
}

}  // namespace wowee::game

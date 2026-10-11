#include "game/shapeshift_forms.hpp"

#include "game/protocol_constants.hpp"

namespace wowee::game {
namespace {

// The order is the order the bar shows them, which is the order the client
// has always shown them and the order every index refers to.
//
// Dire Bear is not a row of its own. It replaces Bear Form on the same button
// (0x005424d4 swaps the list's spell in place), so the button then holds and
// casts Dire Bear Form and is active while its aura is.
const ShapeshiftForm kDruid[] = {
    {.spellId = SPELL_BEAR_FORM,    .formId = 1,  .name = "Bear Form",         .icon = "Interface\\Icons\\Ability_Racial_BearForm",
     .supersededBy = SPELL_DIRE_BEAR_FORM, .supersededName = "Dire Bear Form"},
    {.spellId = SPELL_AQUATIC_FORM, .formId = 2,  .name = "Aquatic Form",      .icon = "Interface\\Icons\\Ability_Druid_AquaticForm"},
    {.spellId = SPELL_CAT_FORM,     .formId = 3,  .name = "Cat Form",          .icon = "Interface\\Icons\\Ability_Druid_CatForm"},
    {.spellId = SPELL_TRAVEL_FORM,  .formId = 4,  .name = "Travel Form",       .icon = "Interface\\Icons\\Ability_Druid_TravelForm"},
    {.spellId = SPELL_MOONKIN_FORM, .formId = 31, .name = "Moonkin Form",      .icon = "Interface\\Icons\\Spell_Nature_ForceOfNature"},
    {.spellId = SPELL_TREE_OF_LIFE, .formId = 36, .name = "Tree of Life",      .icon = "Interface\\Icons\\Ability_Druid_TreeofLife"},
    {.spellId = SPELL_FLIGHT_FORM,  .formId = 29, .name = "Flight Form",       .icon = "Interface\\Icons\\Ability_Druid_FlightForm"},
    {.spellId = SPELL_SWIFT_FLIGHT, .formId = 27, .name = "Swift Flight Form", .icon = "Interface\\Icons\\Ability_Druid_FlightForm"},
};

const ShapeshiftForm kWarrior[] = {
    {.spellId = SPELL_BATTLE_STANCE,    .formId = 17, .name = "Battle Stance",    .icon = "Interface\\Icons\\Ability_Warrior_OffensiveStance"},
    {.spellId = SPELL_DEFENSIVE_STANCE, .formId = 18, .name = "Defensive Stance", .icon = "Interface\\Icons\\Ability_Warrior_DefensiveStance"},
    {.spellId = SPELL_BERSERKER_STANCE, .formId = 19, .name = "Berserker Stance", .icon = "Interface\\Icons\\Ability_Racial_Avatar"},
};

const ShapeshiftForm kDeathKnight[] = {
    {.spellId = SPELL_BLOOD_PRESENCE,  .formId = 32, .name = "Blood Presence",  .icon = "Interface\\Icons\\Spell_Deathknight_BloodPresence"},
    {.spellId = SPELL_FROST_PRESENCE,  .formId = 33, .name = "Frost Presence",  .icon = "Interface\\Icons\\Spell_Deathknight_FrostPresence"},
    {.spellId = SPELL_UNHOLY_PRESENCE, .formId = 34, .name = "Unholy Presence", .icon = "Interface\\Icons\\Spell_Deathknight_UnholyPresence"},
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

bool formHasPresetSpells(const ShapeshiftFormRow& row) {
    for (uint32_t spell : row.presetSpells) {
        if (spell != 0) return true;
    }
    return false;
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
    auto events = bonusBarOffsetEvents(offsetBefore, offsetAfter);
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

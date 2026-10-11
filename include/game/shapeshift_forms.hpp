#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wowee::game {

/// One stance, form or presence a class can take.
///
/// FrameXML's stance bar asks three questions about these and they have to
/// agree with each other: how many there are, what the i-th one looks like,
/// and what casting the i-th one does. Each was answered from its own list.
///
///   GetNumShapeshiftForms counted the known spells out of eight druid forms
///   GetShapeshiftFormInfo  described a fixed six, in a different order
///   CastShapeshiftForm     cast that same fixed six
///
/// So a druid who had learned Aquatic Form or Flight Form - both in the count
/// and in neither of the others - was counted a form the bar could not draw,
/// and every index past the first mismatch described one form and cast
/// another. The bar is built by walking 1..GetNumShapeshiftForms() and calling
/// GetShapeshiftFormInfo on each, so the two have to be the same list.
struct ShapeshiftForm {
    uint32_t spellId;   ///< What casting this form actually casts.
    uint8_t formId;     ///< The shapeshift form field's value while it is active.
    const char* name;
    const char* icon;
    /// The spell that takes this button over once learned, 0 for none - the
    /// list's entry is replaced in place (0x005424d4), as Dire Bear Form does
    /// Bear Form's.
    uint32_t supersededBy = 0;
    /// That spell's name, which the button then shows (0x0053de90 names the
    /// list's spell).
    const char* supersededName = nullptr;
};

/// Every form the given class has, in the order the bar shows them.
///
/// Unfiltered: this is what the class can ever have, not what a character has
/// learned. `knownShapeshiftForms` is what a binding wants.
std::vector<ShapeshiftForm> allShapeshiftForms(uint8_t classId);

/// The forms of `classId` whose spell the player knows, in bar order.
///
/// The filter is not cosmetic. A count fixed per class puts a button on the
/// bar for something the character cannot use - a level 14 priest was offered
/// Shadowform, which is learned at 40 - and an unfiltered index makes the
/// second button describe a form the player has never learned.
template <typename KnownSpells>
std::vector<ShapeshiftForm> knownShapeshiftForms(uint8_t classId, const KnownSpells& known) {
    std::vector<ShapeshiftForm> out;
    for (ShapeshiftForm form : allShapeshiftForms(classId)) {
        if (form.supersededBy != 0 && known.count(form.supersededBy)) {
            form.spellId = form.supersededBy;
            if (form.supersededName) form.name = form.supersededName;
            out.push_back(form);
        } else if (known.count(form.spellId)) {
            out.push_back(form);
        }
    }
    return out;
}

/// What the action bar reads of a SpellShapeshiftForm.dbc row (0x008b8c60:
/// BonusActionBar is column 1, PresetSpellID[8] columns 27..34, +0x2c).
struct ShapeshiftFormRow {
    uint32_t bonusActionBar = 0;
    std::array<uint32_t, 8> presetSpells{};
};

/// 0x005a7c50: a form that brings spells of its own.
[[nodiscard]] bool formHasPresetSpells(const ShapeshiftFormRow& row);

/// The bonus action bar offset (0x005a83c0, DAT_00c1e59c): 5 while the bar
/// holds the pet's actions or a form's preset spells, else the form's
/// BonusActionBar, 0 without a form.
[[nodiscard]] uint32_t bonusBarOffset(bool petActionsOnBar, const ShapeshiftFormRow* form);

/// The events the form field's change fires, in order (0x006da130 ->
/// 0x005a83c0): ACTIONBAR_PAGE_CHANGED when the bar leaves the pet's page (5),
/// UPDATE_BONUS_ACTIONBAR when the offset moved, then UPDATE_SHAPESHIFT_FORM.
[[nodiscard]] std::vector<const char*> formChangeEvents(uint32_t offsetBefore, uint32_t offsetAfter);

/// 0x005a83c0 alone: the offset's own events.
[[nodiscard]] std::vector<const char*> bonusBarOffsetEvents(uint32_t offsetBefore, uint32_t offsetAfter);

/// Whether a form's spell is the one in effect (0x00802cb0): the player
/// carries an aura of it with the positive flag (0x10) set.
template <typename Auras>
[[nodiscard]] bool formSpellActive(const Auras& auras, uint32_t spellId) {
    if (spellId == 0) return false;
    for (const auto& aura : auras) {
        if (aura.spellId == spellId && (aura.flags & 0x10) != 0) return true;
    }
    return false;
}

/// GetShapeshiftForm (0x0053d4b0): the 1-based position of the active form,
/// searched from the end of the list; one past the list for a form field
/// that is set but none of the list's; 0 for none.
[[nodiscard]] uint32_t activeShapeshiftIndex(const std::vector<bool>& activeByIndex, uint8_t formField);

}  // namespace wowee::game

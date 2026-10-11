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
    /// The shapeshift form field's value while it is active: the form's
    /// SpellShapeshiftForm.dbc id (column 0), 0 for one that sets none.
    uint8_t formId;
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
/// BonusActionBar is column 1, Flags column 19 (+0xc), PresetSpellID[8]
/// columns 27..34, +0x2c).
struct ShapeshiftFormRow {
    uint32_t bonusActionBar = 0;
    uint32_t flags = 0;
    std::array<uint32_t, 8> presetSpells{};
    /// CreatureType (+0x10): what the form makes its unit, 0 for unchanged
    /// (0x0071f300).
    uint32_t creatureType = 0;
};

/// SPELL_AURA_MOD_SHAPESHIFT, the aura a form's spell applies (0x24).
inline constexpr uint32_t kAuraModShapeshift = 36;
/// SpellShapeshiftForm Flags 0x1: a stance, which 0x00721ca0 does not count
/// as a form.
inline constexpr uint32_t kFormFlagStance = 0x1;
/// SpellShapeshiftForm Flags 0x2: a form whose aura the player may not cancel.
inline constexpr uint32_t kFormFlagNoCancel = 0x2;

/// SpellShapeshiftForm Flags 0x8: a form the player keeps an NPC's window
/// open in; entering one without it closes the window (0x006da130 ->
/// 0x00518d50).
inline constexpr uint32_t kFormFlagKeepsInteraction = 0x8;

/// SoundEntries 10063 (DruidMorph_Impact_Base): leaving a druid form
/// (0x006da130).
inline constexpr uint32_t kSoundFormLeave = 0x274f;

/// 0x00721ca0: the player is in a form - not none, not a stance. A form with
/// no row is read as none (the client reads past its table).
[[nodiscard]] bool inShapeshiftForm(uint8_t form, const ShapeshiftFormRow* row);

/// 0x006da130: in a form (0x00721ca0) without Flags 0x8, the window the
/// player has open with an NPC or object closes (0x00518d50).
[[nodiscard]] bool formClosesInteraction(uint8_t form, const ShapeshiftFormRow* row);

/// 0x006da130: leaving cat (1), tree (2), travel (3), aquatic (4), bear (5),
/// dire bear (8), flight (0x1d) or moonkin (0x1f) form for none or a stance
/// plays kSoundFormLeave at the player.
[[nodiscard]] bool formLeaveSound(uint8_t formBefore, bool inFormAfter);

/// What cancelling one of the player's auras does about its form (0x00802f80).
enum class FormAuraCancel {
    Send,            ///< CMSG_CANCEL_AURA, nothing more.
    SendFormPending, ///< The form it holds is left at once (0x00721cf0), then sent.
    Refused,         ///< Not sent at all.
};

/// 0x00802f80 for the player's own aura: with a form in effect (`form`, as
/// 0x0071af70 reads it), each shapeshift effect (aura 36) of the spell names
/// a form by its EffectMiscValue. A form with no row, or one flagged no-cancel,
/// keeps the aura; the form the player is in is left before the server says.
template <typename RowOf>
[[nodiscard]] FormAuraCancel formAuraCancel(uint8_t form, const uint32_t (&effectAuras)[3],
                                            const uint32_t (&effectMisc)[3], RowOf rowOf) {
    if (form == 0) return FormAuraCancel::Send;
    for (int i = 0; i < 3; ++i) {
        if (effectAuras[i] != kAuraModShapeshift) continue;
        const ShapeshiftFormRow* row = effectMisc[i] <= 0xFF ? rowOf(static_cast<uint8_t>(effectMisc[i])) : nullptr;
        if (!row || (row->flags & kFormFlagNoCancel) != 0) return FormAuraCancel::Refused;
        if (effectMisc[i] == form) return FormAuraCancel::SendFormPending;
    }
    return FormAuraCancel::Send;
}

/// 0x005a7c50: a form that brings spells of its own.
[[nodiscard]] bool formHasPresetSpells(const ShapeshiftFormRow& row);

/// The bonus action bar offset (0x005a83c0, DAT_00c1e59c): 5 while the bar
/// holds the pet's actions or a form's preset spells, else the form's
/// BonusActionBar, 0 without a form.
[[nodiscard]] uint32_t bonusBarOffset(bool petActionsOnBar, const ShapeshiftFormRow* form);

/// The events the form field's change fires, in order (0x006da130):
/// SPELL_UPDATE_USABLE (0x0053cf10), ACTIONBAR_UPDATE_STATE and
/// CURRENT_SPELL_CAST_CHANGED (0x0053b480), then from 0x005a83c0
/// ACTIONBAR_PAGE_CHANGED when the bar leaves the pet's page (5) and
/// UPDATE_BONUS_ACTIONBAR when the offset moved, then UPDATE_SHAPESHIFT_FORM.
[[nodiscard]] std::vector<const char*> formChangeEvents(uint32_t offsetBefore, uint32_t offsetAfter);

/// 0x005a83c0 alone: the offset's own events.
[[nodiscard]] std::vector<const char*> bonusBarOffsetEvents(uint32_t offsetBefore, uint32_t offsetAfter);

/// Whether a form's spell is the one in effect (0x00802cb0): the player
/// carries an aura of it with the positive flag (0x10) set. A shapeshift
/// spell (aura 36) while the form's cancel is pending (+0x9f4) is not, aura
/// or not: `shapeshiftPending`.
template <typename Auras>
[[nodiscard]] bool formSpellActive(const Auras& auras, uint32_t spellId, bool shapeshiftPending = false) {
    if (spellId == 0 || shapeshiftPending) return false;
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

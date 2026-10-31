#pragma once

// The game error a player's failed cast goes through.
//
// Every refusal of a spell the player casts - the server's SMSG_CAST_FAILED
// and the client's own checks before it sends one (0x00739650 and the rest)
// alike - is shown by 0x00808200, which ends in 0x005216f0 with a game error:
// the line it is shown in, and the line the character speaks for it
// (audio/vocal_error.hpp). Which one is 0x00807f10's choice by the result,
// most often ERR_SPELL_FAILED_S ("%s", the SPELL_FAILED_ text as it is);
// for NO_POWER, 0x00808200's own by the spell's power.

#include <cstdint>
#include <string_view>

namespace wowee::game::cast_failure {

/// What the two read off the failed spell's Spell.dbc row.
struct SpellRow {
    uint32_t category = 0;    ///< Category (+0x4): 4 and 9 potions, 10 and 11 food
    uint32_t attributes = 0;  ///< Attributes (+0x10): 0x10 an ability
    uint32_t targets = 0;     ///< Targets (+0x40): 0x10 an item, 0x20000 a glyph slot
    int32_t powerType = 0;    ///< PowerType (+0xa4): -2 health
    bool powerDisplay = false; ///< a PowerDisplay row by PowerDisplayId (+0x290)
};

constexpr uint32_t kAttrAbility = 0x10;
constexpr uint32_t kTargetItem = 0x10;
constexpr uint32_t kTargetGlyphSlot = 0x20000;

/// The game error, by its GlobalStrings key, for a failed cast's result.
constexpr std::string_view gameError(uint8_t result, const SpellRow& spell) {
    const bool potion = spell.category == 4 || spell.category == 9;
    const bool food = spell.category == 10 || spell.category == 11;
    switch (result) {
    case 0x02: return "ERR_SPELL_FAILED_ALREADY_AT_FULL_HEALTH";
    case 0x03: return "ERR_SPELL_FAILED_ALREADY_AT_FULL_MANA";
    case 0x04: return "ERR_SPELL_FAILED_ALREADY_AT_FULL_POWER_S";
    case 0x0b: return "ERR_GENERIC_NO_TARGET";
    case 0x0c:  // BAD_TARGETS: an item's or a glyph slot's own line
        if ((spell.targets & kTargetItem) != 0) return "ERR_INVALID_ITEM_TARGET";
        if ((spell.targets & kTargetGlyphSlot) != 0) return "ERR_INVALID_GLYPH_SLOT";
        break;
    case 0x1c: return "ERR_SPELL_FAILED_EQUIPPED_ITEM";
    case 0x1d:
    case 0x1e:
    case 0x1f: return "ERR_SPELL_FAILED_EQUIPPED_ITEM_CLASS_S";
    case 0x2d:  // ITEM_NOT_READY
        if (potion) return "ERR_POTION_COOLDOWN";
        if (food) return "ERR_FOOD_COOLDOWN";
        return "ERR_ITEM_COOLDOWN";
    case 0x34: return "ERR_NOAMMO_S";
    case 0x43:  // NOT_READY
        if (potion) return "ERR_POTION_COOLDOWN";
        if (food) return "ERR_FOOD_COOLDOWN";
        return (spell.attributes & kAttrAbility) != 0 ? "ERR_ABILITY_COOLDOWN" : "ERR_SPELL_COOLDOWN";
    case 0x48: return "ERR_SPELL_FAILED_NOTUNSHEATHED";
    case 0x55:  // NO_POWER: a displayed power's, health's, else the power's (0x00a439c8)
        if (spell.powerDisplay) return "ERR_OUT_OF_POWER_DISPLAY";
        switch (spell.powerType) {
        case -2: return "ERR_OUT_OF_HEALTH";
        case 0: return "ERR_OUT_OF_MANA";
        case 1: return "ERR_OUT_OF_RAGE";
        case 2: return "ERR_OUT_OF_FOCUS";
        case 3: return "ERR_OUT_OF_ENERGY";
        case 4: return "ERR_NOT_HAPPY_ENOUGH";
        case 5: return "ERR_OUT_OF_RUNES";
        case 6: return "ERR_OUT_OF_RUNIC_POWER";
        default: break;
        }
        break;
    case 0x5e: return "ERR_SPELL_FAILED_SHAPESHIFT_FORM_S";
    case 0x61: return "ERR_SPELL_OUT_OF_RANGE";
    case 0x64: return "ERR_SPELL_FAILED_REAGENTS";
    case 0x83: return "ERR_SPELL_FAILED_TOTEMS";
    case 0x94: return "ERR_PLAY_TIME_EXCEEDED";
    case 0xaa: return "ERR_BAD_ON_USE_ENCHANT";
    case 0xae: return "ERR_TOO_MANY_SOCKETS";
    case 0xb2: return "ERR_GENERIC_NO_VALID_TARGETS";
    case 0xb8: return "ERR_MAX_SOCKETS";
    default: break;
    }
    return "ERR_SPELL_FAILED_S";
}

/// Which of the four spellcast events a failure's result raises (0x007fecc0).
///
/// Every packet that ends a cast - SMSG_CAST_RESULT, SMSG_CAST_FAILED,
/// SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER, SMSG_PET_CAST_FAILED, and
/// SMSG_SPELL_GO when the cast went off - runs through that one routine, and it
/// picks the event by the reason number rather than by which packet arrived:
///
///   0x18 CHARMED, 0x1b DONT_REPORT, 0x69 SPELL_IN_PROGRESS - the three
///   refusals that leave a cast running - raise FAILED_QUIET;
///   0x28 INTERRUPTED, 0x29 INTERRUPTED_COMBAT raise INTERRUPTED;
///   0xbb raises SUCCEEDED;
///   anything else raises plain FAILED.
///
/// 0xbb is no result at all: SMSG_SPELL_GO pushes it as a literal (0x0080e74f)
/// for a cast that went off, and it is one past the last name 0x007fcca0 has -
/// that table stops at 186, SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED - so the
/// client's own number list runs out exactly where its "went off" sentinel
/// begins.  The names of the other five are that same table's.
///
/// The distinction is not cosmetic: CastingBarFrame registers START, STOP,
/// FAILED, INTERRUPTED, DELAYED and the three channel events
/// (castingbarframe.lua:6-13), and not FAILED_QUIET, which is what keeps the
/// bar of a spell still running up when the realm refuses a second attempt at
/// it beside the first.  The chat window's cast-sequence managers do listen for
/// it (chatframe.lua:727,797,942), and it is a failure the combat log and the
/// error line report just the same - 0x00807f10 gives all three of these the
/// default ERR_SPELL_FAILED_S - so the difference is entirely in what the event
/// tells the frames watching the cast.
enum class CastEvent { Failed, FailedQuiet, Interrupted, Succeeded };

constexpr CastEvent castEvent(uint8_t result) {
    switch (result) {
    case 0x18:  // SPELL_FAILED_CHARMED
    case 0x1b:  // SPELL_FAILED_DONT_REPORT
    case 0x69:  // SPELL_FAILED_SPELL_IN_PROGRESS
        return CastEvent::FailedQuiet;
    case 0x28:  // SPELL_FAILED_INTERRUPTED
    case 0x29:  // SPELL_FAILED_INTERRUPTED_COMBAT
        return CastEvent::Interrupted;
    case 0xbb:  // the "it went off" sentinel, past the end of the name table
        return CastEvent::Succeeded;
    default:
        return CastEvent::Failed;
    }
}

/// The interface name that event is raised under.
constexpr std::string_view castEventName(CastEvent event) {
    switch (event) {
    case CastEvent::FailedQuiet: return "UNIT_SPELLCAST_FAILED_QUIET";
    case CastEvent::Interrupted: return "UNIT_SPELLCAST_INTERRUPTED";
    case CastEvent::Succeeded:   return "UNIT_SPELLCAST_SUCCEEDED";
    case CastEvent::Failed: break;
    }
    return "UNIT_SPELLCAST_FAILED";
}

/// The game error a pet's failed cast is shown through (0x00805610, from
/// SMSG_PET_CAST_FAILED): the pet's own ERR_PET_SPELL_* lines, the cooldown
/// line by the ability flag alone, NO_POWER's as the player's, and
/// ERR_SPELL_FAILED_S for the rest.
constexpr std::string_view petGameError(uint8_t result, const SpellRow& spell) {
    switch (result) {
    case 0x01: return "ERR_PET_SPELL_AFFECTING_COMBAT";
    case 0x17: return "ERR_PET_SPELL_DEAD";
    case 0x38: return "ERR_PET_SPELL_NOPATH";
    case 0x39: return "ERR_PET_SPELL_NOT_BEHIND";
    case 0x43: return (spell.attributes & kAttrAbility) != 0 ? "ERR_ABILITY_COOLDOWN" : "ERR_SPELL_COOLDOWN";
    case 0x55: return gameError(result, spell);
    case 0x5e: return "ERR_SPELL_FAILED_SHAPESHIFT_FORM_S";
    case 0x61: return "ERR_PET_SPELL_OUT_OF_RANGE";
    case 0x67: return "ERR_PET_SPELL_ROOTED";
    case 0x6d: return "ERR_PET_SPELL_TARGETS_DEAD";
    default: return "ERR_SPELL_FAILED_S";
    }
}

/// A failed cast is not reported again while it repeats the last one.
///
/// 0x00808200 keeps the spell and the result of the last failure it saw
/// (0x00d397c4, 0x00d397c0) and when (0x00d397bc): the same spell refused
/// for the same reason less than three seconds after that is not shown,
/// spoken or logged - and the time is taken again each time, so a key held
/// on a spell that is not ready says so once, and again only after three
/// seconds without. Any spell going off (SMSG_SPELL_GO without its pending
/// flag, 0x0080e1b0) forgets it.
class RepeatHold {
public:
    static constexpr uint32_t kHoldMs = 3000;

    /// Whether this failure is reported; noted either way.
    bool report(uint32_t spellId, uint8_t result, uint32_t nowMs) {
        if (spellId == spellId_ && result == result_) {
            const bool due = static_cast<int32_t>(nowMs - atMs_ - kHoldMs) >= 0;
            atMs_ = nowMs;
            return due;
        }
        spellId_ = spellId;
        result_ = result;
        atMs_ = nowMs;
        return true;
    }
    void forget() { spellId_ = 0; }

private:
    uint32_t spellId_ = 0;
    uint8_t result_ = 0;
    uint32_t atMs_ = 0;
};

}  // namespace wowee::game::cast_failure

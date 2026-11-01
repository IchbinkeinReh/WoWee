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
#include <iterator>
#include <string>
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

/// SpellCastResult -> its SPELL_FAILED_ key, less the prefix, as 0x007fcca0
/// names them; anything past the end is SPELL_FAILED_UNKNOWN.
///
/// That address is one switch - 187 string literals, cases 0 through 0xba with
/// no hole in between, and SPELL_FAILED_UNKNOWN as the fall-through. The two
/// readers of a refused spell, the player's line in spell_handler and the
/// pet's in combat_handler, each had all 187 rows written out beside it; re-
/// reading the switch against both found no row apart from the other and no
/// spelling of one different, which is what one switch guarantees and what two
/// hand-copied tables do not. The client has one because it asks once; these
/// two asked twice, so the copy is here and each reader is back to being a
/// reader.
inline constexpr const char* kSpellFailedNames[] = {
    "SUCCESS", "AFFECTING_COMBAT", "ALREADY_AT_FULL_HEALTH", "ALREADY_AT_FULL_MANA",
    "ALREADY_AT_FULL_POWER", "ALREADY_BEING_TAMED", "ALREADY_HAVE_CHARM", "ALREADY_HAVE_SUMMON",
    "ALREADY_OPEN", "AURA_BOUNCED", "AUTOTRACK_INTERRUPTED", "BAD_IMPLICIT_TARGETS", "BAD_TARGETS",
    "CANT_BE_CHARMED", "CANT_BE_DISENCHANTED", "CANT_BE_DISENCHANTED_SKILL", "CANT_BE_MILLED",
    "CANT_BE_PROSPECTED", "CANT_CAST_ON_TAPPED", "CANT_DUEL_WHILE_INVISIBLE",
    "CANT_DUEL_WHILE_STEALTHED", "CANT_STEALTH", "CASTER_AURASTATE", "CASTER_DEAD", "CHARMED",
    "CHEST_IN_USE", "CONFUSED", "DONT_REPORT", "EQUIPPED_ITEM", "EQUIPPED_ITEM_CLASS",
    "EQUIPPED_ITEM_CLASS_MAINHAND", "EQUIPPED_ITEM_CLASS_OFFHAND", "ERROR", "FIZZLE", "FLEEING",
    "FOOD_LOWLEVEL", "HIGHLEVEL", "HUNGER_SATIATED", "IMMUNE", "INCORRECT_AREA", "INTERRUPTED",
    "INTERRUPTED_COMBAT", "ITEM_ALREADY_ENCHANTED", "ITEM_GONE", "ITEM_NOT_FOUND",
    "ITEM_NOT_READY", "LEVEL_REQUIREMENT", "LINE_OF_SIGHT", "LOWLEVEL", "LOW_CASTLEVEL",
    "MAINHAND_EMPTY", "MOVING", "NEED_AMMO", "NEED_AMMO_POUCH", "NEED_EXOTIC_AMMO",
    "NEED_MORE_ITEMS", "NOPATH", "NOT_BEHIND", "NOT_FISHABLE", "NOT_FLYING", "NOT_HERE",
    "NOT_INFRONT", "NOT_IN_CONTROL", "NOT_KNOWN", "NOT_MOUNTED", "NOT_ON_TAXI", "NOT_ON_TRANSPORT",
    "NOT_READY", "NOT_SHAPESHIFT", "NOT_STANDING", "NOT_TRADEABLE", "NOT_TRADING",
    "NOT_UNSHEATHED", "NOT_WHILE_GHOST", "NOT_WHILE_LOOTING", "NO_AMMO", "NO_CHARGES_REMAIN",
    "NO_CHAMPION", "NO_COMBO_POINTS", "NO_DUELING", "NO_ENDURANCE", "NO_FISH",
    "NO_ITEMS_WHILE_SHAPESHIFTED", "NO_MOUNTS_ALLOWED", "NO_PET", "NO_POWER", "NOTHING_TO_DISPEL",
    "NOTHING_TO_STEAL", "ONLY_ABOVEWATER", "ONLY_DAYTIME", "ONLY_INDOORS", "ONLY_MOUNTED",
    "ONLY_NIGHTTIME", "ONLY_OUTDOORS", "ONLY_SHAPESHIFT", "ONLY_STEALTHED", "ONLY_UNDERWATER",
    "OUT_OF_RANGE", "PACIFIED", "POSSESSED", "REAGENTS", "REQUIRES_AREA", "REQUIRES_SPELL_FOCUS",
    "ROOTED", "SILENCED", "SPELL_IN_PROGRESS", "SPELL_LEARNED", "SPELL_UNAVAILABLE", "STUNNED",
    "TARGETS_DEAD", "TARGET_AFFECTING_COMBAT", "TARGET_AURASTATE", "TARGET_DUELING",
    "TARGET_ENEMY", "TARGET_ENRAGED", "TARGET_FRIENDLY", "TARGET_IN_COMBAT", "TARGET_IS_PLAYER",
    "TARGET_IS_PLAYER_CONTROLLED", "TARGET_NOT_DEAD", "TARGET_NOT_IN_PARTY", "TARGET_NOT_LOOTED",
    "TARGET_NOT_PLAYER", "TARGET_NO_POCKETS", "TARGET_NO_WEAPONS", "TARGET_NO_RANGED_WEAPONS",
    "TARGET_UNSKINNABLE", "THIRST_SATIATED", "TOO_CLOSE", "TOO_MANY_OF_ITEM", "TOTEM_CATEGORY",
    "TOTEMS", "TRY_AGAIN", "UNIT_NOT_BEHIND", "UNIT_NOT_INFRONT", "WRONG_PET_FOOD",
    "NOT_WHILE_FATIGUED", "TARGET_NOT_IN_INSTANCE", "NOT_WHILE_TRADING", "TARGET_NOT_IN_RAID",
    "TARGET_FREEFORALL", "NO_EDIBLE_CORPSES", "ONLY_BATTLEGROUNDS", "TARGET_NOT_GHOST",
    "TRANSFORM_UNUSABLE", "WRONG_WEATHER", "DAMAGE_IMMUNE", "PREVENTED_BY_MECHANIC", "PLAY_TIME",
    "REPUTATION", "MIN_SKILL", "NOT_IN_ARENA", "NOT_ON_SHAPESHIFT", "NOT_ON_STEALTHED",
    "NOT_ON_DAMAGE_IMMUNE", "NOT_ON_MOUNTED", "TOO_SHALLOW", "TARGET_NOT_IN_SANCTUARY",
    "TARGET_IS_TRIVIAL", "BM_OR_INVISGOD", "EXPERT_RIDING_REQUIREMENT",
    "ARTISAN_RIDING_REQUIREMENT", "NOT_IDLE", "NOT_INACTIVE", "PARTIAL_PLAYTIME", "NO_PLAYTIME",
    "NOT_IN_BATTLEGROUND", "NOT_IN_RAID_INSTANCE", "ONLY_IN_ARENA",
    "TARGET_LOCKED_TO_RAID_INSTANCE", "ON_USE_ENCHANT", "NOT_ON_GROUND", "CUSTOM_ERROR",
    "CANT_DO_THAT_RIGHT_NOW", "TOO_MANY_SOCKETS", "INVALID_GLYPH", "UNIQUE_GLYPH",
    "GLYPH_SOCKET_LOCKED", "NO_VALID_TARGETS", "ITEM_AT_MAX_CHARGES", "NOT_IN_BARBERSHOP",
    "FISHING_TOO_LOW", "ITEM_ENCHANT_TRADE_WINDOW", "SUMMON_PENDING", "MAX_SOCKETS",
    "PET_CAN_RENAME", "TARGET_CANNOT_BE_RESURRECTED",
};

/// The whole GlobalStrings key, spelled as that switch spells it. The prefix is
/// part of what the client returns, so it is part of the answer here rather
/// than something each reader pastes on; and SPELL_FAILED_UNKNOWN past the end
/// is the switch's own fall-through, which suits both readers - neither has a
/// line to show for a result it has no name for, and both said "Unknown
/// reason" in English for one.
inline std::string spellFailedKey(uint8_t result) {
    return std::string("SPELL_FAILED_") +
           (result < std::size(kSpellFailedNames) ? kSpellFailedNames[result] : "UNKNOWN");
}

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

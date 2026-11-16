#pragma once

/**
 * tooltip_setter_rules.hpp - what a GameTooltip setter builds, decided the
 * way the client decides it, free of the game so it can be tested.
 *
 * Which OnTooltipSet* script a setter ends with is not the setter's own
 * choice: each script is fired at the end of one builder, and a setter only
 * picks the builder. The scripts sit at GameTooltip +0x4ec (Unit), +0x4f4
 * (Item), +0x4fc (Spell), +0x504 (Quest), +0x50c (Achievement) and +0x514
 * (EquipmentSet) (0x0061a280), and are fired by
 *
 *   0x00621070  the unit builder           OnTooltipSetUnit
 *   0x006277f0  the item builder           OnTooltipSetItem, at its end
 *   0x00623760  the item not yet known     OnTooltipSetItem too, under the
 *               (RETRIEVING_ITEM_INFO)     red RETRIEVING_ITEM_INFO line
 *   0x006238a0  the spell builder          OnTooltipSetSpell
 *   0x00626e20  the talent builder         OnTooltipSetSpell
 *   0x00622960  the quest builder          OnTooltipSetQuest
 *   0x00627220  the achievement builder    OnTooltipSetAchievement
 *   0x00622dd0  the equipment set builder  OnTooltipSetEquipmentSet
 *
 * and nothing else fires one: the aura builder (0x00625350: SetUnitAura,
 * SetUnitBuff, SetUnitDebuff, SetTracking), the glyph builder (0x00622ba0:
 * SetGlyph, a glyph link), SetTotem (0x006205c0) and SetFrameStack
 * (0x00626560) announce nothing past the OnTooltipCleared every one of them
 * starts with (0x0061c620).
 */

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wowee::addons::tooltip_rules {

/// Spell.dbc effects that make an item: CREATE_ITEM (24), CREATE_RANDOM_ITEM
/// (59) and CREATE_ITEM_2 (157) - the three 0x0062eae0 and 0x006238a0 test.
constexpr bool createsItem(uint32_t effect) {
    return effect == 0x18 || effect == 0x3b || effect == 0x9d;
}

/// Whether a spell's tooltip ends with the tooltip of the item it makes
/// (0x006238a0): a trade spell (Attributes 0x20) whose effects, walked in
/// order, reach an item-making one before an ENCHANT_ITEM (53) - that one
/// stops the walk - and whose EffectItemType[0] names an item.
constexpr bool spellTooltipAppendsItem(uint32_t attributes, const uint32_t (&effects)[3],
                                       uint32_t effectItemType0) {
    if ((attributes & 0x20u) == 0 || effectItemType0 == 0) return false;
    for (const uint32_t e : effects) {
        if (e == 0x35) return false;
        if (createsItem(e)) return true;
    }
    return false;
}

/// A time left as 0x0061a9e0 words it under a GlobalStrings base key
/// (ITEM_COOLDOWN_TIME, SPELL_TIME_REMAINING): the key's suffix and the
/// number for its %d. Whole seconds under a minute, always rounded down;
/// minutes, hours and days rounded up where `roundUp` (its seventh argument)
/// asks and down where not - sixty minutes reading one hour and twenty-four
/// hours one day only on the way up.
struct TimeLeft {
    const char* suffix;  ///< "_SEC", "_MIN", "_HOURS" or "_DAYS"
    uint64_t value;
};
constexpr TimeLeft timeLeft(uint64_t ms, bool roundUp) {
    constexpr uint64_t kMinute = 60000, kHour = kMinute * 60, kDay = kMinute * 1440;
    const auto over = [roundUp](uint64_t v, uint64_t unit) {
        return roundUp ? (v == 0 ? 0 : (v - 1) / unit + 1) : v / unit;
    };
    if (ms >= kDay) return {"_DAYS", over(ms, kDay)};
    if (ms >= kHour) {
        const uint64_t hours = over(ms, kHour);
        return hours == 24 ? TimeLeft{"_DAYS", 1} : TimeLeft{"_HOURS", hours};
    }
    if (ms >= kMinute) {
        const uint64_t minutes = over(ms, kMinute);
        return minutes == 60 ? TimeLeft{"_HOURS", 1} : TimeLeft{"_MIN", minutes};
    }
    return {"_SEC", ms / 1000};
}

/// The stack count an aura's tooltip multiplies its $s, $m, $M and $o by
/// (0x00625350 into 0x00579e50): the aura's count, but one for a spell
/// whose StackAmount is under two - there the count is charges, not doses.
constexpr int auraTooltipStacks(uint32_t stackAmount, int count) {
    return static_cast<int32_t>(stackAmount) < 2 ? 1 : count;
}

/// A ranged spell's cast time with a ranged weapon in hand (0x007ff180's
/// tail, the player's own cast only): the half second Attributes 0x2 adds
/// already in `castMs`, then scaled by UNIT_FIELD_RANGEDATTACKTIME over the
/// weapon's Delay (item +0xf4) and truncated (_ftol2). Unchanged where
/// either is zero.
inline int32_t rangedCastTimeMs(int32_t castMs, uint32_t rangedAttackTime, uint32_t weaponDelay) {
    if (rangedAttackTime == 0 || weaponDelay == 0) return castMs;
    const double scaled = static_cast<double>(rangedAttackTime) /
                          static_cast<double>(static_cast<int32_t>(weaponDelay)) * static_cast<double>(castMs);
    return static_cast<int32_t>(static_cast<int64_t>(scaled));
}

/// How many of an item an effect makes, its least or its most
/// (0x008016c0 over 0x007ff770's two values): doubled, a half added away
/// from zero, rounded to nearest by the x87 store and halved by a shift.
inline int craftedCount(float value) {
    const float twice = value + value;
    const float biased = twice > 0.0f ? twice + 0.5f : twice - 0.5f;
    return static_cast<int>(std::nearbyint(biased)) >> 1;
}

/// What SetTradeSkillItem(index [, reagent]) describes (0x0062eae0).
enum class TradeSkillSubject {
    Invalid,      ///< "Invalid trade skill item in SetTradeSkillItem(index [,reagent])"
    CraftedItem,  ///< the item builder, with the count made (+0x4b8/+0x4bc)
    Spell,        ///< the spell builder: an enchant, or anything not made
    Reagent,      ///< the item builder, with the count needed (+0x4b8)
};

struct TradeSkillTooltip {
    TradeSkillSubject subject = TradeSkillSubject::Invalid;
    uint32_t itemId = 0;
    int minCount = 0;   ///< the count the price is multiplied by
    int maxCount = 0;   ///< more than minCount: a range, priced as one
};

struct Reagent {
    uint32_t itemId = 0;
    uint32_t count = 0;
};

/// The recipe's first effect and its EffectItemType[0], the least and the most
/// that effect makes (0x007ff770), its eight reagent slots, and the reagent
/// asked for - zero when the call gave none.
///
/// Without a reagent: a recipe whose first effect makes an item, an item
/// named, is that item's tooltip, priced for what one craft makes; anything
/// else (an enchant, a smelt that only converts, a recipe whose item is in a
/// later effect) is the spell's. With one: the reagent-th of the slots that
/// name an item, counted over the empty ones; past the last, the error.
inline TradeSkillTooltip tradeSkillTooltip(uint32_t firstEffect, uint32_t firstItemType,
                                           float madeMin, float madeMax,
                                           const Reagent* reagents, std::size_t reagentSlots,
                                           int reagentIndex) {
    TradeSkillTooltip out;
    if (reagentIndex == 0) {
        if (createsItem(firstEffect) && firstItemType != 0) {
            out.subject = TradeSkillSubject::CraftedItem;
            out.itemId = firstItemType;
            out.minCount = craftedCount(madeMin);
            out.maxCount = craftedCount(madeMax);
        } else {
            out.subject = TradeSkillSubject::Spell;
        }
        return out;
    }
    int seen = 0;
    for (std::size_t i = 0; i < reagentSlots && i < 8; ++i) {
        if (reagents[i].itemId == 0) continue;
        if (++seen != reagentIndex) continue;
        out.subject = TradeSkillSubject::Reagent;
        out.itemId = reagents[i].itemId;
        out.minCount = static_cast<int>(reagents[i].count);
        out.maxCount = 0;
        return out;
    }
    return out;
}

/// What a macro on the action bar is hovered to (0x00630d20, reached from
/// SetAction 0x00631000 for a slot holding a macro, 0x005648d0).
///
/// Only a macro whose text has a "#showtooltip" line (+0x570, set as the text
/// is read, 0x00565c40) describes what it shows: its item (+0x568) through
/// the item builder - ending OnTooltipSetItem - else its spell (+0x564)
/// through the spell builder - ending OnTooltipSetSpell. Anything else, a
/// macro without the line or one whose line names nothing, is the macro's
/// name alone in white, and SetAction answers nil.
enum class MacroTooltip {
    Name,   ///< the macro's name (macro +0x20), white, no script
    Item,   ///< the item builder 0x006277f0
    Spell,  ///< the spell builder 0x006238a0
};

/// `spell` as the macro holds it: 0 when no command applied, -1 for one that
/// named nothing (which the spell builder refuses: the tooltip hides).
constexpr MacroTooltip macroActionTooltip(bool showTooltip, uint32_t item, int32_t spell) {
    if (!showTooltip) return MacroTooltip::Name;
    if (item != 0) return MacroTooltip::Item;
    if (spell != 0) return MacroTooltip::Spell;
    return MacroTooltip::Name;
}

/// The aura filter bits 0x00608390 builds for UnitAura, UnitBuff, UnitDebuff,
/// CancelUnitBuff and the tooltip's SetUnitAura / SetUnitBuff / SetUnitDebuff.
enum AuraFilter : uint8_t {
    kAuraHelpful = 0x01,
    kAuraHarmful = 0x02,
    kAuraPlayer = 0x04,
    kAuraRaid = 0x08,
    kAuraCancelable = 0x10,
    kAuraNotCancelable = 0x20,
};

/// 0x00608390's filter: HELPFUL (1) before anything is read - so no filter,
/// nil or "" counts buffs - then the text split on spaces and '|'
/// (0x0076f1e0 with " |", a word cut to 31 characters), each word compared
/// without case (0x0076e780): HELPFUL and HARMFUL replace each other, the
/// last one read winning, PLAYER, RAID, CANCELABLE and NOT_CANCELABLE add
/// their bit, and any other word is passed over.
inline uint8_t auraFilterBits(const char* filter) {
    uint8_t bits = kAuraHelpful;
    if (!filter) return bits;
    const auto is = [](const std::string& word, const char* name) {
        std::size_t i = 0;
        for (; name[i] != '\0'; ++i) {
            if (i >= word.size() ||
                std::toupper(static_cast<unsigned char>(word[i])) != static_cast<unsigned char>(name[i]))
                return false;
        }
        return i == word.size();
    };
    const char* p = filter;
    while (*p != '\0') {
        while (*p == ' ' || *p == '|') ++p;
        if (*p == '\0') break;
        std::string word;
        for (; *p != '\0' && *p != ' ' && *p != '|'; ++p) {
            if (word.size() < 31) word.push_back(*p);
        }
        if (is(word, "HELPFUL")) bits = static_cast<uint8_t>((bits & ~kAuraHarmful) | kAuraHelpful);
        else if (is(word, "HARMFUL")) bits = static_cast<uint8_t>((bits & ~kAuraHelpful) | kAuraHarmful);
        else if (is(word, "PLAYER")) bits |= kAuraPlayer;
        else if (is(word, "RAID")) bits |= kAuraRaid;
        else if (is(word, "CANCELABLE")) bits |= kAuraCancelable;
        else if (is(word, "NOT_CANCELABLE")) bits |= kAuraNotCancelable;
    }
    return bits;
}

}  // namespace wowee::addons::tooltip_rules

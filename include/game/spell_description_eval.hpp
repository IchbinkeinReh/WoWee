#pragma once

/**
 * spell_description_eval.hpp - the little expression language spell
 * descriptions are written in.
 *
 * A description can say "$<percent>% of your normal weapon damage", and what
 * percent is depends on the spell. SpellDescriptionVariables.dbc holds one row
 * per spell that needs it, declaring names in terms of the player. Sinister
 * Strike's row reads
 *
 *     $opportunity1=$?s14057[${110}][${100}]
 *     $percent=$?s14072[${120}][${$<opportunity1>}]
 *
 * - a hundred, or more with the Opportunity talent. Nothing read that table,
 * and the formatter had no branch for "$<": it dropped those two characters as
 * an unknown token and left the rest of the name in the sentence, so the
 * tooltip read "percent>% of your normal weapon damage".
 *
 * The language is the client's (0x005778b0 parses, 0x00578d10 runs):
 *
 *     + - * / % ^ and parentheses, with the usual precedence
 *     $abs( $ceil( $floor( $min( $max( $gt( $lt( $gte( $lte( $eq(
 *     $cond( $clamp(       functions, by any case (0x00576b10)
 *     $<name>              another declaration in the same row
 *     $m1 $AP $SPH ...     the variables of 0x00ace8f8, by exact case,
 *                          another spell's with its number in front
 *
 * with $?s<id>[then][else] besides, which the declarations of a row are
 * written with. A token the table does not name fails the expression, and
 * the formatter then writes "$" where it stood, as 0x0057aaf0 does.
 *
 * Free of the game handler so it can be tested: what a magnitude is and which
 * spells are known are asked of the caller.
 */

#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

#include "game/spell_mods.hpp"

namespace wowee {
namespace game {

/// What the expression may ask about the world around it.
struct SpellDescriptionContext {
    /// $m1 / $M1 / $s1 / $o1 - a spell effect's magnitude, by token letter
    /// and zero-based index. 'm' is the least the effect does and 'M' the
    /// most (0x005782d0's two cases over 0x007ff770). False when there is
    /// none, which fails the expression.
    std::function<bool(char code, int index, double& out)> magnitude;
    /// Everything else a token may stand for inside an expression that the
    /// caller can answer: $d (the duration, in seconds) and $t1 (an effect's
    /// period, in milliseconds), and any token naming another spell -
    /// "$72368m1", "$1510d" (0x005782d0). `spellId` is 0 for the spell's
    /// own. False when it cannot, which fails the expression.
    std::function<bool(uint32_t spellId, char code, int index, double& out)> quantity;
    /// Every other variable of the client's table (0x00ace8f8, 0x005782d0)
    /// - "a1", "h", "AP", "SPH", "MWS", "pl" ... - by its exact name; another
    /// spell's where `spellId` is not 0. False where the caller cannot
    /// answer, which fails the expression.
    std::function<bool(uint32_t spellId, const std::string& name, double& out)> variable;
    /// Whether the player knows a spell, for $?s<id>[then][else].
    std::function<bool(uint32_t spellId)> knowsSpell;
    /// The "$name=expression" lines this spell draws on, or null.
    const std::string* declarations = nullptr;
};

/// The least and the most one spell effect does, as 0x007ff770 works them
/// out from its Spell.dbc row: BasePoints + 1 and BasePoints + DieSides, both
/// raised by RealPointsPerLevel for each level the caster has over the
/// spell's BaseLevel, counting no further than its MaxLevel.
///
/// The least is BasePoints + 1 whatever DieSides says. Frostbolt rank 1's
/// damage is BasePoints 17, DieSides 3, 0.5 a level from level 4 to 8 - 20
/// to 22 for anyone of level 8 or more. Reading every magnitude as
/// BasePoints + 1 made its "$m2 bis $M2" one number twice.
struct SpellEffectPoints {
    float min = 0.0f;
    float max = 0.0f;
};
SpellEffectPoints spellEffectPoints(int32_t basePoints, int32_t dieSides,
                                    float pointsPerLevel, int casterLevel,
                                    int baseLevel, int maxLevel);

/// What 0x007fd440 makes of an effect, by its Effect and EffectApplyAuraName:
/// whether its points end whole (the least down, the most up), whether the
/// talents' damage ops reach it, and whether that is the periodic one.
///
///   direct damage and healing (2 9 10 17 31 58 67 75 121): whole, damage
///   an aura (6 27 35 65 119 128 129 143) of periodic damage, healing or
///   leech (3 8 20 53 62 89): whole, damage, periodic; a damage shield or
///   proc damage (15 43): whole, damage; periodic power (21 24 63 64 162):
///   whole
///   energize and the like (8 30 62 146): whole
struct SpellEffectKind {
    bool whole = false;
    bool damage = false;
    bool periodic = false;
};
SpellEffectKind spellEffectKind(uint32_t effect, uint32_t aura);

/// The talents' modifiers for one op, as 0x007fd970 sums them for a spell.
using SpellModifierLookup = std::function<spell_mods::Sum(uint8_t op)>;

/// The rest of 0x007ff770, after the level: unless `mods` is empty (an
/// aura's tooltip, its eighth argument set), both ends through op 8 (all
/// effects), the effect's own op (3, 12, 23 for the first, second, third),
/// op 22 (periodic damage) or op 0 (damage) where spellEffectKind says
/// damage, and op 2 (threat) for an aura of threat (10, 103, 183); then to
/// the nearest 128th, and for a whole kind the least down and the most up
/// to a whole number - where the client's own ceiling makes 1 of a zero
/// (0x0095d500).
SpellEffectPoints finishSpellEffectPoints(SpellEffectPoints raw, int index, uint32_t effect,
                                          uint32_t aura, const SpellModifierLookup& mods);

/// 0x005773b0: to the nearest whole number, halves away from zero ($F).
long spellRoundHalfAway(float v);

/// "$lsingular:plural;" (0x00576ef0) from `at`, just past the letter: the
/// form the count picks - the first for one, the second for any other, as
/// 0x00817d10 has it for the German and English clients - with the spaces
/// in front of the body and around the form dropped. `at` ends past the
/// ';'. False where there is no ';', and the text goes on as written.
bool pickPluralForm(const std::string& raw, std::size_t& at, long count, std::string& out);

/// "$gmale:female;" (0x00576cd0) from `at`, just past the letter: the first
/// form for a male player (`female` false), the second for a female one - a
/// third, past another ':', is never chosen. Spaces as for the plural.
bool pickGenderForm(const std::string& raw, std::size_t& at, bool female, std::string& out);

/// A "${...}" written into the text (0x00579980): "%.0f", or "%.<n>f"
/// where ".<n>" (1 to 9) follows the brace.
std::string formatSpellExpressionValue(float v, int decimals);

/// A bare $m / $M / $s / $S token as 0x00579e50 writes it into the text:
/// $m the least, $M the most, $s the one number when least and most agree
/// and "least to most" through INT_SPELL_POINTS_SPREAD_TEMPLATE when they do
/// not ($S keeps a fraction, through SPELL_POINTS_SPREAD_TEMPLATE). Signs are
/// dropped, as the client drops them: Frostbolt's -40% slow reads "40%".
/// `shown` is the number a following $l plural counts.
struct SpellPointsText {
    std::string text;
    long shown = 0;
};
SpellPointsText formatSpellPoints(char code, const SpellEffectPoints& points,
                                  const std::string& intSpreadTemplate,
                                  const std::string& spreadTemplate);

/// A GlobalStrings number format ("%1$d bis %2$d", "%.1f to %.1f") filled
/// with numbers, positional or in order.
std::string formatSpellNumbers(const std::string& fmt, std::initializer_list<double> args);

/// How long a spell lasts, in milliseconds, as 0x00800a70 works it out from
/// its SpellDuration.dbc row before talents touch it: the base and PerLevel
/// for each level the caster has over the spell's BaseLevel, never more than
/// the row's maximum. Negative for a spell that lasts until cancelled (-1).
int spellDurationMs(int baseMs, int perLevelMs, int maxMs, int casterLevel, int baseLevel);

/// 0x00800a70's last step, taken only when its fifth argument asks for it -
/// which the description formatter's $d does (0x00579e50 'd', and $d inside
/// an expression, 0x005782d0 0x2c/0x2d) and its $o does not. A periodic spell
/// (an EffectAmplitude set) whose period haste shortens - AttributesEx5
/// 0x2000, or an aura 316 of the player's that reaches it (0x007283a0) - and
/// that does not carry AttributesEx3 0x20000000 lasts its duration (after
/// the talents) times the player's UNIT_MOD_CAST_SPEED, rounded, where that
/// factor is over 0.001.
int hastedSpellDurationMs(int ms, bool periodic, bool hasteShortens, uint32_t attrEx3,
                          float castSpeed);

/// A talent's flat and percent modifier on a whole number, as 0x007fdb50
/// applies it: (value + flat) x pct / 100, cut toward zero.
inline int applySpellModifier(int value, int flat, int pct) {
    return (value + flat) * pct / 100;
}

/// 0x0061a960: whether a time in milliseconds is a whole number of the unit
/// it will be shown in - seconds under a minute, minutes under an hour, hours
/// under a day, then days - to within a hundredth.
bool spellTimeIsWhole(float ms);

/// Where a format comes from: a GlobalStrings key and its enUS wording,
/// answered with the localized text.
using SpellTextSource = std::function<std::string(const char* key, const char* enUS)>;

/// $d as 0x00579e50 writes it. Under a millisecond is SPELL_DURATION_UNTIL_
/// CANCELLED; a whole number of its unit goes through 0x0061a9e0 with the
/// INT_SPELL_DURATION_ keys ("%d Sek.", "%d |4Stunde:Stunden;"), anything
/// else through 0x0061aee0 with the SPELL_DURATION_ ones ("%.2f Min."). The
/// unit is seconds under a minute, minutes under an hour, hours under a day,
/// and days; sixty whole minutes are one hour and twenty-four hours one day.
/// The |4 plural escape is resolved.
std::string formatSpellDuration(int ms, const SpellTextSource& text);

/// $t as 0x00579e50 writes it: the period in whole seconds ("%d", ms / 1000)
/// when spellTimeIsWhole says so, else in seconds with two decimals ("%.2f").
std::string formatSpellPeriod(int ms);

/// $o: what a periodic effect does over the whole of the spell, as 0x00579e50
/// works it out - each tick's least and most times the duration over the
/// period. A period of zero counts as five seconds and a negative one gives
/// nothing; a spell whose first tick comes as it lands (AttributesEx5 0x200)
/// gets one more period's worth. Nothing for a spell without a duration.
///
/// Fireball rank 1's burn is one point every 2 seconds for 4: 2, where taking
/// the tick for the total read "insgesamt 1 zusätzlichen Feuerschaden".
SpellEffectPoints spellPeriodicTotal(const SpellEffectPoints& tick, int amplitudeMs,
                                     int durationMs, bool tickOnApply);

/// Both ends scaled - the "$/1000;" and "$*10;" in front of a token.
inline SpellEffectPoints scaledSpellPoints(const SpellEffectPoints& p, float factor) {
    return {p.min * factor, p.max * factor};
}

/// The level a token of an aura's description is worked at (0x00579e50 with
/// the aura's level, 0x007ff770 then taking it as given): the spell's own
/// tokens at the aura's level, not held to MaxLevel; another spell's
/// ("$12345s1") at the player's level where that spell has AttributesEx2
/// 0x1000000, else the aura's - and those held to its MaxLevel.
struct AuraTokenLevel {
    int level;
    bool heldToMaxLevel;
};
constexpr AuraTokenLevel auraTokenLevel(bool reference, int auraLevel, int playerLevel, uint32_t refAttrEx2) {
    if (!reference) return {auraLevel, false};
    return {(refAttrEx2 & 0x1000000u) != 0 ? playerLevel : auraLevel, true};
}

/// A "$?" in the sentence itself (0x00579ba0): "cond[then][else]", the else
/// part another "?cond[then]..." where the chain goes on. Brackets close at
/// the first ']' after them, as the client's strchr finds it. A condition
/// (0x005797f0) is s<id> (a spell the player knows) or a<id> (an aura on the
/// player), each optionally behind '!', joined left to right by '&' and '|'
/// with parentheses. `at` is just past "$?"; `holds(letter, id)` answers one
/// term. On success `body` is the chosen branch's raw text and `end` the
/// index after the chain's last ']'.
struct DescriptionBranch {
    bool ok = false;
    std::size_t end = 0;
    std::string body;
};
DescriptionBranch pickDescriptionBranch(const std::string& raw, std::size_t at,
                                        const std::function<bool(char, uint32_t)>& holds);

/// Evaluate one expression - a whole declaration, or the body of a ${}.
/// False when any part of it is something this does not model.
bool evaluateSpellExpression(const std::string& text,
                             const SpellDescriptionContext& ctx,
                             double& out);

/// The unevaluated right-hand side of "$name=" in ctx.declarations.
bool spellDeclarationOf(const SpellDescriptionContext& ctx,
                        const std::string& name, std::string& out);

}  // namespace game
}  // namespace wowee

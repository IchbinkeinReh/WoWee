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
 * Three constructs, and only these three:
 *
 *     ${ arithmetic }         + - * / and parentheses
 *     $?s<id>[then][else]     whether the player knows that spell
 *     $<name>                 another declaration in the same row
 *
 * with $m1 $M1 $s1 $o1 for the spell's own magnitudes, $d and $t1 for its
 * duration and period, and any of them with another spell's number in front. Anything else fails the
 * whole expression rather than guessing at it, and the caller drops a token it
 * could not work out rather than leaking the name into the sentence.
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

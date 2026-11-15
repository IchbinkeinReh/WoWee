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
 * with $m1 $M1 $s1 $o1 for the spell's own magnitudes. Anything else fails the
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

// The little expression language spell descriptions are written in.
//
// A description can say "$<percent>% of your normal weapon damage", and what
// percent is depends on the spell. Nothing read SpellDescriptionVariables.dbc,
// and the formatter had no branch for "$<" - it dropped those two characters
// as an unknown token and left the rest of the name in the sentence, so
// Sinister Strike's tooltip read "percent>% of your normal weapon damage".

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "game/spell_description_eval.hpp"

using namespace wowee::game;

namespace {

/// Sinister Strike's row of SpellDescriptionVariables.dbc, verbatim.
const std::string kSinisterStrike =
    "$opportunity1=$?s14057[${110}][${100}]\r\n"
    "$percent=$?s14072[${120}][${$<opportunity1>}]";

SpellDescriptionContext contextFor(const std::string& declarations,
                                   std::vector<uint32_t> known = {},
                                   std::vector<double> magnitudes = {}) {
    static std::string decls;
    static std::vector<uint32_t> knownSpells;
    static std::vector<double> mags;
    decls = declarations;
    knownSpells = std::move(known);
    mags = std::move(magnitudes);

    SpellDescriptionContext ctx;
    ctx.declarations = &decls;
    ctx.knowsSpell = [](uint32_t id) {
        return std::find(knownSpells.begin(), knownSpells.end(), id) != knownSpells.end();
    };
    ctx.magnitude = [](char, int index, double& out) {
        if (index < 0 || index >= static_cast<int>(mags.size())) return false;
        out = mags[static_cast<std::size_t>(index)];
        return true;
    };
    return ctx;
}

double evaluated(const std::string& name, const SpellDescriptionContext& ctx) {
    std::string decl;
    REQUIRE(spellDeclarationOf(ctx, name, decl));
    double out = 0.0;
    REQUIRE(evaluateSpellExpression(decl, ctx, out));
    return out;
}

}  // namespace

TEST_CASE("a rogue with no talents strikes for the base percentage",
          "[spell][description]") {
    const auto ctx = contextFor(kSinisterStrike);
    // Neither Opportunity rank is known, so both conditions take their else
    // branch and the answer is the hundred the tooltip should have shown.
    CHECK(evaluated("percent", ctx) == Catch::Approx(100.0));
}

TEST_CASE("a talent the player has changes the number", "[spell][description]") {
    CHECK(evaluated("percent", contextFor(kSinisterStrike, {14057})) == Catch::Approx(110.0));
    CHECK(evaluated("percent", contextFor(kSinisterStrike, {14072})) == Catch::Approx(120.0));
    // The second rank wins: its branch does not consult the first.
    CHECK(evaluated("percent", contextFor(kSinisterStrike, {14057, 14072})) == Catch::Approx(120.0));
}

TEST_CASE("a declaration can name another one", "[spell][description]") {
    // $percent's else branch is ${$<opportunity1>}, so resolving it at all
    // means the nested lookup worked.
    const auto ctx = contextFor(kSinisterStrike, {14057});
    CHECK(evaluated("opportunity1", ctx) == Catch::Approx(110.0));
}

TEST_CASE("arithmetic, in the order it is written", "[spell][description]") {
    const auto ctx = contextFor("$a=${2+3*4}\r\n$b=${(2+3)*4}\r\n$c=${10/4}");
    // Left to right, which is what these expressions are written for - the
    // parenthesised form is how the data groups when it means to.
    CHECK(evaluated("a", ctx) == Catch::Approx(20.0));
    CHECK(evaluated("b", ctx) == Catch::Approx(20.0));
    CHECK(evaluated("c", ctx) == Catch::Approx(2.5));
}

TEST_CASE("a spell's own magnitudes are asked of the caller", "[spell][description]") {
    const auto ctx = contextFor("$dmg=${$m1*2}", {}, {7.0, 11.0});
    CHECK(evaluated("dmg", ctx) == Catch::Approx(14.0));
}

TEST_CASE("a quantity this client does not keep fails the whole expression",
          "[spell][description]") {
    // $AP is attack power, which is not modelled here. Standing a zero in for
    // it would put a wrong number in a tooltip, which is worse than no number.
    const auto ctx = contextFor("$x=${$m1+$AP*0.03}", {}, {5.0});
    std::string decl;
    REQUIRE(spellDeclarationOf(ctx, "x", decl));
    double out = 0.0;
    CHECK_FALSE(evaluateSpellExpression(decl, ctx, out));
}

TEST_CASE("a name that is not declared is not a number", "[spell][description]") {
    const auto ctx = contextFor(kSinisterStrike);
    std::string decl;
    CHECK_FALSE(spellDeclarationOf(ctx, "nosuchthing", decl));
    double out = 0.0;
    CHECK_FALSE(evaluateSpellExpression("$<nosuchthing>", ctx, out));
}

TEST_CASE("a declaration naming itself stops rather than recurring forever",
          "[spell][description]") {
    const auto ctx = contextFor("$loop=${$<loop>+1}");
    double out = 0.0;
    CHECK_FALSE(evaluateSpellExpression("$<loop>", ctx, out));
}

TEST_CASE("malformed expressions are refused, not guessed at",
          "[spell][description]") {
    const auto ctx = contextFor("$x=1");
    double out = 0.0;
    CHECK_FALSE(evaluateSpellExpression("${1+}", ctx, out));
    CHECK_FALSE(evaluateSpellExpression("${(1+2}", ctx, out));
    CHECK_FALSE(evaluateSpellExpression("$?s123[10]", ctx, out));   // one branch
    CHECK_FALSE(evaluateSpellExpression("${1/0}", ctx, out));
    CHECK_FALSE(evaluateSpellExpression("", ctx, out));
}

// ---------------------------------------------------------------------------
// $m / $M / $s: the least, the most, and both (0x007ff770, 0x00579e50).
// The rows below are Spell.dbc 3.3.5a (12340) as the deDE client ships it:
// BasePoints, DieSides, RealPointsPerLevel, BaseLevel, MaxLevel.

namespace {
const std::string kIntSpreadDe = "%1$d bis %2$d";  // INT_SPELL_POINTS_SPREAD_TEMPLATE
const std::string kSpreadDe = "%.1f bis %.1f";     // SPELL_POINTS_SPREAD_TEMPLATE

std::string shown(char code, const SpellEffectPoints& p) {
    return formatSpellPoints(code, p, kIntSpreadDe, kSpreadDe).text;
}
}  // namespace

TEST_CASE("Frostbolt rank 1 hits for 20 to 22, not 20 to 20", "[spell][description]") {
    // 116 effect 2: BasePoints 17, DieSides 3, 0.5 a level from 4 to 8.
    const SpellEffectPoints dmg = spellEffectPoints(17, 3, 0.5f, 80, 4, 8);
    CHECK(dmg.min == Catch::Approx(20.0f));
    CHECK(dmg.max == Catch::Approx(22.0f));
    CHECK(shown('m', dmg) == "20");
    CHECK(shown('M', dmg) == "22");
    CHECK(shown('s', dmg) == "20 bis 22");

    // Its description: "${$m2*$<mult>} bis ${$M2*$<mult>}" - $m2 and $M2
    // inside an expression are the least and the most too.
    SpellDescriptionContext ctx;
    ctx.magnitude = [dmg](char code, int, double& out) {
        out = (code == 'M') ? dmg.max : dmg.min;
        return true;
    };
    double lo = 0.0, hi = 0.0;
    REQUIRE(evaluateSpellExpression("$m2*1", ctx, lo));
    REQUIRE(evaluateSpellExpression("$M2*1", ctx, hi));
    CHECK(lo == Catch::Approx(20.0));
    CHECK(hi == Catch::Approx(22.0));

    // Effect 1 is the slow: BasePoints -41, DieSides 1 - one number, unsigned.
    const SpellEffectPoints slow = spellEffectPoints(-41, 1, 0.0f, 80, 4, 8);
    CHECK(shown('s', slow) == "40");
    CHECK(shown('m', slow) == "40");
    CHECK(shown('M', slow) == "40");
}

TEST_CASE("a level below MaxLevel scales by the levels over BaseLevel", "[spell][description]") {
    // Frostbolt rank 1 for a level-4 mage: no levels over BaseLevel.
    const SpellEffectPoints at4 = spellEffectPoints(17, 3, 0.5f, 4, 4, 8);
    CHECK(shown('s', at4) == "18 bis 20");
    // Level 6: two levels, one point.
    const SpellEffectPoints at6 = spellEffectPoints(17, 3, 0.5f, 6, 4, 8);
    CHECK(shown('s', at6) == "19 bis 21");
}

TEST_CASE("a Horde shaman's Lightning Bolt reads its dice as a spread", "[spell][description]") {
    // 403 Blitzschlag rank 1: BasePoints 12, DieSides 3, 0.4 a level 1..6.
    const SpellEffectPoints bolt = spellEffectPoints(12, 3, 0.4f, 80, 1, 6);
    CHECK(shown('s', bolt) == "15 bis 17");
    CHECK(shown('s', spellEffectPoints(12, 3, 0.4f, 1, 1, 6)) == "13 bis 15");
}

TEST_CASE("a fraction rounds the least down and the most up in $s", "[spell][description]") {
    // 331 Welle der Heilung rank 1: BasePoints 33, DieSides 11, 0.7 a level
    // 1..5 - 36.8 to 46.8 at level 5 or more.
    const SpellEffectPoints wave = spellEffectPoints(33, 11, 0.7f, 80, 1, 5);
    CHECK(shown('s', wave) == "36 bis 47");
    CHECK(shown('S', wave) == "36.8 bis 46.8");
    CHECK(shown('m', wave) == "36.8");
    CHECK(shown('M', wave) == "46.8");
}

TEST_CASE("one die side is one number, and $l counts it", "[spell][description]") {
    // 133 Feuerball rank 1 effect 2: BasePoints 0, DieSides 1.
    const SpellPointsText one = formatSpellPoints('s', spellEffectPoints(0, 1, 0.0f, 80, 1, 5),
                                                  kIntSpreadDe, kSpreadDe);
    CHECK(one.text == "1");
    CHECK(one.shown == 1);
    // A spread counts its most.
    const SpellPointsText many = formatSpellPoints('s', spellEffectPoints(17, 3, 0.5f, 80, 4, 8),
                                                   kIntSpreadDe, kSpreadDe);
    CHECK(many.shown == 22);
}

TEST_CASE("the enUS spread templates fill in order", "[spell][description]") {
    CHECK(formatSpellNumbers("%d to %d", {20.0, 22.0}) == "20 to 22");
    CHECK(formatSpellNumbers("%2$d - %1$d", {20.0, 22.0}) == "22 - 20");
    CHECK(formatSpellNumbers("%.1f to %.1f", {36.8, 46.8}) == "36.8 to 46.8");
}

TEST_CASE("Frostbolt's talent chain is ten deep and still evaluates", "[spell][description]") {
    // SpellDescriptionVariables.dbc row 167, verbatim: Frostbolt's $<mult>.
    const std::string kFrostbolt =
        "$arctic1=$?s31674[${1.01}][${1}]\r\n"
        "$arctic2=$?s31675[${1.02}][${$<arctic1>}]\r\n"
        "$arctic3=$?s31676[${1.03}][${$<arctic2>}]\r\n"
        "$arctic4=$?s31677[${1.04}][${$<arctic3>}]\r\n"
        "$arctic5=$?s31678[${1.05}][${$<arctic4>}]\r\n"
        "$piercing1=$?s11151[${1.02}][${1}]\r\n"
        "$piercing2=$?s12952[${1.04}][${$<piercing1>}]\r\n"
        "$piercing3=$?s12953[${1.06}][${$<piercing2>}]\r\n"
        "$mult=${$<arctic5>*$<piercing3>}";
    CHECK(evaluated("mult", contextFor(kFrostbolt)) == Catch::Approx(1.0));
    // Arctic Winds rank 1 alone reaches the bottom of the chain.
    CHECK(evaluated("mult", contextFor(kFrostbolt, {31674})) == Catch::Approx(1.01));
    CHECK(evaluated("mult", contextFor(kFrostbolt, {31678, 12953})) == Catch::Approx(1.05 * 1.06));

    // The description's own expressions, rank 1 at level 80: 20 and 22.
    auto ctx = contextFor(kFrostbolt, {}, {40.0, 20.0, 0.0});
    double lo = 0.0;
    REQUIRE(evaluateSpellExpression("$m2*$<mult>", ctx, lo));
    CHECK(lo == Catch::Approx(20.0));
}

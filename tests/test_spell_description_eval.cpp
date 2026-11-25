// The little expression language spell descriptions are written in.
//
// A description can say "$<percent>% of your normal weapon damage", and what
// percent is depends on the spell. Nothing read SpellDescriptionVariables.dbc,
// and the formatter had no branch for "$<" - it dropped those two characters
// as an unknown token and left the rest of the name in the sentence, so
// Sinister Strike's tooltip read "percent>% of your normal weapon damage".

#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>
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

// 0x00577030: the German row of Slice and Dice.
TEST_CASE("what stands between the two brackets is passed over", "[spell][description]") {
    const std::string declared = "$glyph=$?s56810[${3}]$[${0}]";
    CHECK(evaluated("glyph", contextFor(declared)) == Catch::Approx(0.0));
    CHECK(evaluated("glyph", contextFor(declared, {56810})) == Catch::Approx(3.0));
    const std::string raw = "$?s1[A]$[B]y";
    auto b = pickDescriptionBranch(raw, 2, [](char, uint32_t) { return false; });
    REQUIRE(b.ok);
    CHECK(b.body == "B");
    CHECK(raw.substr(b.end) == "y");
    b = pickDescriptionBranch(raw, 2, [](char, uint32_t) { return true; });
    REQUIRE(b.ok);
    CHECK(b.body == "A");
    CHECK(raw.substr(b.end) == "y");
}

TEST_CASE("a declaration can name another one", "[spell][description]") {
    // $percent's else branch is ${$<opportunity1>}, so resolving it at all
    // means the nested lookup worked.
    const auto ctx = contextFor(kSinisterStrike, {14057});
    CHECK(evaluated("opportunity1", ctx) == Catch::Approx(110.0));
}

TEST_CASE("arithmetic, products before sums", "[spell][description]") {
    const auto ctx = contextFor("$a=${2+3*4}\r\n$b=${(2+3)*4}\r\n$c=${10/4}");
    // 0x005778b0 sums terms that 0x005777f0 has multiplied out first: not
    // left to right.
    CHECK(evaluated("a", ctx) == Catch::Approx(14.0));
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

TEST_CASE("a name that is not declared is nought in an expression", "[spell][description]") {
    const auto ctx = contextFor(kSinisterStrike);
    std::string decl;
    CHECK_FALSE(spellDeclarationOf(ctx, "nosuchthing", decl));
    // 0x005774d0 answers 0 for a name the row's table does not hold.
    double out = 1.0;
    REQUIRE(evaluateSpellExpression("$<nosuchthing>+2", ctx, out));
    CHECK(out == Catch::Approx(2.0));
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

// ---------------------------------------------------------------------------
// $o: each tick times the duration over the period (0x00579e50). Rows are
// Spell.dbc / SpellDuration.dbc 3.3.5a (12340), deDE.

namespace {
std::string shownTotal(const SpellEffectPoints& tick, int amplitudeMs, int durationMs,
                       bool tickOnApply = false) {
    return shown('s', spellPeriodicTotal(tick, amplitudeMs, durationMs, tickOnApply));
}
}  // namespace

TEST_CASE("Fireball rank 1 burns for 2 in all, not the tick's 1", "[spell][description]") {
    // 133 Feuerball rank 1: "$d lang insgesamt $o2 zusaetzlichen Feuerschaden".
    // Effect 2 is BasePoints 0, DieSides 1, EffectAmplitude 2000; DurationIndex
    // 35 is 4000 ms.
    const SpellEffectPoints tick = spellEffectPoints(0, 1, 0.0f, 80, 1, 5);
    CHECK(shown('s', tick) == "1");
    CHECK(shownTotal(tick, 2000, 4000) == "2");
}

TEST_CASE("the warlock's and priest's first damage over time add up", "[spell][description]") {
    // 589 Schattenwort: Schmerz rank 1: BasePoints 4, every 3000 ms for 18000.
    CHECK(shownTotal(spellEffectPoints(4, 1, 0.0f, 80, 4, 9), 3000, 18000) == "30");
    // 172 Verderbnis rank 1: BasePoints 9, every 3000 ms for 12000.
    CHECK(shownTotal(spellEffectPoints(9, 1, 0.0f, 80, 4, 9), 3000, 12000) == "40");
    // 348 Feuerbrand rank 1: BasePoints 3, every 3000 ms for 15000.
    CHECK(shownTotal(spellEffectPoints(3, 1, 0.0f, 80, 1, 5), 3000, 15000) == "20");
}

TEST_CASE("a first tick on landing counts one period more", "[spell][description]") {
    // 35387 Gift spucken: AttributesEx5 0x200, BasePoints 0, DieSides 3, 0.2
    // a level from 1, every 2000 ms for 8000 - five ticks, not four.
    const SpellEffectPoints tick = spellEffectPoints(0, 3, 0.2f, 1, 1, 0);
    CHECK(shownTotal(tick, 2000, 8000, true) == "5 bis 15");
    CHECK(shownTotal(tick, 2000, 8000, false) == "4 bis 12");
}

TEST_CASE("no period is five seconds, a negative one or no duration nothing",
          "[spell][description]") {
    const SpellEffectPoints tick{10.0f, 10.0f};
    CHECK(shownTotal(tick, 0, 15000) == "30");
    CHECK(shownTotal(tick, -1, 15000) == "0");
    CHECK(shownTotal(tick, 3000, 0) == "0");
    CHECK(shownTotal(tick, 3000, -1) == "0");
}

// ---------------------------------------------------------------------------
// $d and $t: the interface's own words (0x0061a9e0, 0x0061aee0). The deDE
// GlobalStrings as the client ships them.

namespace {
std::string deText(const char* key, const char* enUS) {
    static const std::pair<const char*, const char*> kDe[] = {
        {"INT_SPELL_DURATION_DAYS", "%d |4Tag:Tage;"},
        {"INT_SPELL_DURATION_HOURS", "%d |4Stunde:Stunden;"},
        {"INT_SPELL_DURATION_MIN", "%d Min."},
        {"INT_SPELL_DURATION_SEC", "%d Sek."},
        {"SPELL_DURATION_DAYS", "%.2f |4Tag:Tage;"},
        {"SPELL_DURATION_HOURS", "%.2f |4Stunde:Stunden;"},
        {"SPELL_DURATION_MIN", "%.2f Min."},
        {"SPELL_DURATION_SEC", "%.2f Sek."},
        {"SPELL_DURATION_UNTIL_CANCELLED", "bis Abbruch"},
    };
    for (const auto& [k, v] : kDe) {
        if (std::string(k) == key) return v;
    }
    return enUS;
}
std::string enText(const char*, const char* enUS) { return enUS; }
}  // namespace

TEST_CASE("Frostbolt slows for 5 Sek. on a German client, not 5 sec", "[spell][description]") {
    // 116 Frostblitz rank 1: DurationIndex 28, 5000 ms.
    CHECK(formatSpellDuration(5000, deText) == "5 Sek.");
    CHECK(formatSpellDuration(5000, enText) == "5 sec");
    // 133 Feuerball rank 1: 4000 ms; 589 Schattenwort: Schmerz: 18000 ms.
    CHECK(formatSpellDuration(4000, deText) == "4 Sek.");
    CHECK(formatSpellDuration(18000, deText) == "18 Sek.");
}

TEST_CASE("minutes, hours and days take their own words", "[spell][description]") {
    CHECK(formatSpellDuration(60000, deText) == "1 Min.");      // 134 Feuerschild
    CHECK(formatSpellDuration(600000, deText) == "10 Min.");    // 131 Wasseratmung
    CHECK(formatSpellDuration(1800000, deText) == "30 Min.");   // 168 Frostruestung
    CHECK(formatSpellDuration(3600000, deText) == "1 Stunde");  // 673 Geringe Ruestung
    CHECK(formatSpellDuration(7200000, deText) == "2 Stunden"); // 24382 Zanzas Geist
    CHECK(formatSpellDuration(86400000, deText) == "1 Tag");
    CHECK(formatSpellDuration(2 * 86400000, deText) == "2 Tage");
    CHECK(formatSpellDuration(7200000, enText) == "2 hrs");
}

TEST_CASE("a duration that is not whole in its unit keeps two decimals", "[spell][description]") {
    CHECK(formatSpellDuration(2500, deText) == "2.50 Sek.");   // 8150 Donnerkracher
    CHECK(formatSpellDuration(90000, deText) == "1.50 Min.");  // 4941 Steinschild
    CHECK(formatSpellDuration(5400000, deText) == "1.50 Stunden");
}

TEST_CASE("no duration, or -1, lasts until cancelled", "[spell][description]") {
    CHECK(formatSpellDuration(-1, deText) == "bis Abbruch");  // 1908 Maechtige Heilung
    CHECK(formatSpellDuration(0, deText) == "bis Abbruch");
    CHECK(formatSpellDuration(0, enText) == "until cancelled");
}

TEST_CASE("SpellDuration grows by level and stops at its maximum", "[spell][description]") {
    CHECK(spellDurationMs(5000, 0, 5000, 80, 4) == 5000);
    CHECK(spellDurationMs(10000, 1000, 15000, 7, 4) == 13000);
    CHECK(spellDurationMs(10000, 1000, 15000, 80, 4) == 15000);
    CHECK(spellDurationMs(-1, 0, -1, 80, 1) == -1);
    // A talent's flat and percent (0x007fdb50): +3000 ms, then 120%.
    CHECK(applySpellModifier(12000, 3000, 120) == 18000);
    CHECK(applySpellModifier(7, 0, 50) == 3);
}

TEST_CASE("$t is whole seconds, or seconds with two decimals", "[spell][description]") {
    CHECK(formatSpellPeriod(3000) == "3");     // 134 Feuerschild
    CHECK(formatSpellPeriod(10000) == "10");   // 8275 Vergifteter Schuss
    CHECK(formatSpellPeriod(250) == "0.25");   // 49981 Maschinengewehr
    CHECK(formatSpellPeriod(1500) == "1.50");  // 52632 Humanoiden verschlingen
}

TEST_CASE("an expression reads $d in seconds, $t in ms and other spells' tokens",
          "[spell][description]") {
    SpellDescriptionContext ctx;
    ctx.quantity = [](uint32_t spellId, char code, int index, double& out) {
        // 1510 Salve: 6000 ms. 72368: m1 7, period 1000 ms, 10000 ms.
        if (spellId == 1510 && code == 'd') { out = 6.0; return true; }
        if (spellId == 72368 && code == 'm' && index == 0) { out = 7.0; return true; }
        if (spellId == 72368 && code == 't' && index == 0) { out = 1000.0; return true; }
        if (spellId == 72368 && code == 'd') { out = 10.0; return true; }
        if (spellId == 0 && code == 'd') { out = 4.0; return true; }
        return false;
    };
    double v = 0.0;
    REQUIRE(evaluateSpellExpression("$1510d/6", ctx, v));
    CHECK(v == Catch::Approx(1.0));
    REQUIRE(evaluateSpellExpression("$72368m1*1000*$72368d/$72368t1", ctx, v));
    CHECK(v == Catch::Approx(70.0));
    REQUIRE(evaluateSpellExpression("$d*2", ctx, v));
    CHECK(v == Catch::Approx(8.0));
    CHECK_FALSE(evaluateSpellExpression("$999m1", ctx, v));
}

TEST_CASE("$/N; and $*N; scale both ends", "[spell][description]") {
    // "$/10;s1" over 100..100, "$/1000;S" keeps the fraction.
    CHECK(shown('s', scaledSpellPoints({100.0f, 100.0f}, 0.1f)) == "10");
    CHECK(shown('S', scaledSpellPoints({1500.0f, 1500.0f}, 0.001f)) == "1.5");
    CHECK(shown('s', scaledSpellPoints({3.0f, 3.0f}, 10.0f)) == "30");
}

TEST_CASE("$d of a periodic spell haste shortens is hasted, as 0x00800a70 does",
          "[spell][description]") {
    // Mind Flay: 3000 ms, AttributesEx5 0x2000, at 20% haste
    // (UNIT_MOD_CAST_SPEED 1/1.2).
    const float hasted = 1.0f / 1.2f;
    CHECK(hastedSpellDurationMs(3000, true, true, 0, hasted) == 2500);
    // Not periodic (no EffectAmplitude): untouched.
    CHECK(hastedSpellDurationMs(3000, false, true, 0, hasted) == 3000);
    // Periodic, but neither the attribute nor an aura 316 reaches it.
    CHECK(hastedSpellDurationMs(18000, true, false, 0, hasted) == 18000);
    // AttributesEx3 0x20000000 keeps it as written.
    CHECK(hastedSpellDurationMs(3000, true, true, 0x20000000u, hasted) == 3000);
    // No haste field yet (0) or a factor of 0.001 and under: untouched.
    CHECK(hastedSpellDurationMs(3000, true, true, 0, 0.0f) == 3000);
    CHECK(hastedSpellDurationMs(3000, true, true, 0, 0.001f) == 3000);
    // Rounded to nearest, after the talents: 5000 at 1/1.15 is 4347.8.
    CHECK(hastedSpellDurationMs(5000, true, true, 0, 1.0f / 1.15f) == 4348);
    // Until cancelled stays until cancelled.
    CHECK(hastedSpellDurationMs(-1, true, true, 0, hasted) == -1);
}

TEST_CASE("an aura's tokens are worked at the aura's level (0x00579e50)", "[spell_desc]") {
    // The spell's own: the aura's level, not held to MaxLevel.
    auto t = auraTokenLevel(false, 70, 80, 0);
    CHECK(t.level == 70);
    CHECK_FALSE(t.heldToMaxLevel);
    // Another spell's: the aura's level held to that spell's MaxLevel...
    t = auraTokenLevel(true, 70, 80, 0);
    CHECK(t.level == 70);
    CHECK(t.heldToMaxLevel);
    // ... or the player's, where it has AttributesEx2 0x1000000.
    t = auraTokenLevel(true, 70, 80, 0x1000000u);
    CHECK(t.level == 80);
    CHECK(t.heldToMaxLevel);
    // Worked through: Frostbolt rank 1 (17 + 3 dice, 0.5 a level from 4 to 8)
    // at level 20 reads 26 to 28 for its own token, 20 to 22 held.
    const auto own = spellEffectPoints(17, 3, 0.5f, 20, 4, 0);
    CHECK(own.min == Catch::Approx(26.0f));
    CHECK(own.max == Catch::Approx(28.0f));
    const auto held = spellEffectPoints(17, 3, 0.5f, 20, 4, 8);
    CHECK(held.min == Catch::Approx(20.0f));
    CHECK(held.max == Catch::Approx(22.0f));
}

TEST_CASE("a choice in the sentence picks its branch (0x00579ba0)", "[spell_desc]") {
    const auto knows = [](char letter, uint32_t id) { return (letter == 's' && id == 58079) || (letter == 'a' && id == 7); };
    // Unending Breath: "$?s58079[ und erhoeht ...][.]"
    std::string raw = "Wasser$?s58079[ und schneller.][.] Ende";
    auto b = pickDescriptionBranch(raw, 8, knows);
    REQUIRE(b.ok);
    CHECK(b.body == " und schneller.");
    CHECK(raw.substr(b.end) == " Ende");
    raw = "x$?s1[A][B]y";
    b = pickDescriptionBranch(raw, 3, knows);
    REQUIRE(b.ok);
    CHECK(b.body == "B");
    CHECK(raw.substr(b.end) == "y");
    // A chain: the else part another condition.
    raw = "$?s1[A]?a7[B][C]z";
    b = pickDescriptionBranch(raw, 2, knows);
    REQUIRE(b.ok);
    CHECK(b.body == "B");
    CHECK(raw.substr(b.end) == "z");
    raw = "$?s58079[A]?a7[B][C]z";
    b = pickDescriptionBranch(raw, 2, knows);
    CHECK(b.body == "A");
    CHECK(raw.substr(b.end) == "z");
    raw = "$?s1[A]?a8[B][C]z";
    CHECK(pickDescriptionBranch(raw, 2, knows).body == "C");
    // Negation, and, or, parentheses.
    CHECK(pickDescriptionBranch("$?!s1[T][F]", 2, knows).body == "T");
    CHECK(pickDescriptionBranch("$?s58079&a8[T][F]", 2, knows).body == "F");
    CHECK(pickDescriptionBranch("$?s1|a7[T][F]", 2, knows).body == "T");
    CHECK(pickDescriptionBranch("$?!(s1|a8)[T][F]", 2, knows).body == "T");
    // Malformed: no else.
    CHECK_FALSE(pickDescriptionBranch("$?s1[T] rest", 2, knows).ok);
}

// ── The client's expression language (0x005778b0, 0x00578d10) ──────────────

namespace {

/// A context whose variables answer from a table, and which records what it
/// was asked.
struct VariableTable {
    std::vector<std::pair<std::string, double>> values;
    std::vector<std::string> asked;
};

SpellDescriptionContext variableContext(VariableTable& t) {
    SpellDescriptionContext ctx;
    ctx.variable = [&t](uint32_t ref, const std::string& name, double& out) {
        t.asked.push_back(ref ? std::to_string(ref) + name : name);
        for (const auto& [n, v] : t.values) {
            if (n == name) { out = v; return true; }
        }
        return false;
    };
    ctx.magnitude = [](char code, int index, double& out) {
        out = (code == 'M' ? 20.0 : 10.0) + index;
        return true;
    };
    return ctx;
}

double value(const std::string& text, const SpellDescriptionContext& ctx) {
    double out = 0.0;
    REQUIRE(evaluateSpellExpression(text, ctx, out));
    return out;
}

}  // namespace

TEST_CASE("products bind tighter than sums, powers tighter still", "[spell][description][expr]") {
    VariableTable t;
    const auto ctx = variableContext(t);
    CHECK(value("2+3*4", ctx) == Catch::Approx(14.0));
    CHECK(value("(2+3)*4", ctx) == Catch::Approx(20.0));
    CHECK(value("2*3^2", ctx) == Catch::Approx(18.0));
    CHECK(value("10-4-3", ctx) == Catch::Approx(3.0));
    // '%' is the dividend less the fraction of the quotient times the
    // divisor (0x00577ad0 case 7): the whole multiples, not the remainder.
    CHECK(value("7%3", ctx) == Catch::Approx(6.0));
    // The sign takes only the operand after it (0x00577530).
    CHECK(value("-2^2", ctx) == Catch::Approx(4.0));
    CHECK(value("2*-3", ctx) == Catch::Approx(-6.0));
    CHECK(value(".5*4", ctx) == Catch::Approx(2.0));
}

TEST_CASE("the twelve functions, by any case", "[spell][description][expr]") {
    VariableTable t;
    const auto ctx = variableContext(t);
    CHECK(value("$abs(-3)", ctx) == Catch::Approx(3.0));
    CHECK(value("$ceil(2.1)", ctx) == Catch::Approx(3.0));
    CHECK(value("$FLOOR(2.9)", ctx) == Catch::Approx(2.0));
    CHECK(value("$min(4,2)", ctx) == Catch::Approx(2.0));
    CHECK(value("$Max(4,2)", ctx) == Catch::Approx(4.0));
    CHECK(value("$gt(4,2)+$lt(4,2)", ctx) == Catch::Approx(1.0));
    CHECK(value("$gte(2,2)+$lte(3,2)", ctx) == Catch::Approx(1.0));
    CHECK(value("$eq(0.33333,1/3)", ctx) == Catch::Approx(1.0));
    CHECK(value("$cond(0,1,2)", ctx) == Catch::Approx(2.0));
    CHECK(value("$COND(5,1,2)", ctx) == Catch::Approx(1.0));
    CHECK(value("$clamp(5,0,3)", ctx) == Catch::Approx(3.0));
    CHECK(value("$clamp(-5,0,3)", ctx) == Catch::Approx(0.0));
    CHECK(value("$clamp(2,0,3)", ctx) == Catch::Approx(2.0));
    double out = 0.0;
    CHECK_FALSE(evaluateSpellExpression("$min(1)", ctx, out));   // one short
    CHECK_FALSE(evaluateSpellExpression("$max 1,2", ctx, out));  // no parenthesis
    CHECK_FALSE(evaluateSpellExpression("$sqrt(4)", ctx, out));  // no such function
}

TEST_CASE("food's level scaling (65418) and Restore Energy's (9512)", "[spell][description][expr]") {
    VariableTable t;
    t.values = {{"pl", 80.0}, {"PL", 80.0}};
    const auto ctx = variableContext(t);
    // ${$FLOOR($COND($GT($pl,15),$pl-15,0)*0.554)+4}: 65 * 0.554 = 36.01
    CHECK(value("$FLOOR($COND($GT($pl,15),$pl-15,0)*0.554)+4", ctx) == Catch::Approx(40.0));
    CHECK(value("100-2*$max(0,$PL-40)", ctx) == Catch::Approx(20.0));
    CHECK(value("60-4*$max(0,$min(15,$PL-60))", ctx) == Catch::Approx(0.0));
}

TEST_CASE("variables are the table's names, by exact case", "[spell][description][expr]") {
    VariableTable t;
    t.values = {{"AP", 1000.0}, {"ap", 900.0}, {"SPH", 50.0}, {"h", 25.0}, {"a1", 8.0}};
    const auto ctx = variableContext(t);
    // Shred's sort of sum: $m1 is the caller's magnitude, $AP a variable.
    CHECK(value("$m1+$AP*0.07", ctx) == Catch::Approx(80.0));
    CHECK(value("$ap", ctx) == Catch::Approx(900.0));
    CHECK(value("$m1+0.15*$SPH", ctx) == Catch::Approx(17.5));
    CHECK(value("$h/2", ctx) == Catch::Approx(12.5));
    CHECK(value("$a1*2", ctx) == Catch::Approx(16.0));
    double out = 0.0;
    // Not in 0x00ace8f8: no "Ap", no "s1", no "o1" - the expression fails.
    CHECK_FALSE(evaluateSpellExpression("$Ap", ctx, out));
    CHECK_FALSE(evaluateSpellExpression("$s1", ctx, out));
    CHECK_FALSE(evaluateSpellExpression("$o1", ctx, out));
    // Another spell's variable goes with its number.
    t.values.push_back({"u", 3.0});
    t.asked.clear();
    CHECK(value("$71432u-1", ctx) == Catch::Approx(2.0));
    REQUIRE(t.asked.size() == 1);
    CHECK(t.asked[0] == "71432u");
}

TEST_CASE("a division by zero is the x87's infinity", "[spell][description][expr]") {
    VariableTable t;
    const auto ctx = variableContext(t);
    CHECK(std::isinf(value("1/0", ctx)));
}

// ── 0x007ff770's rounding and the talents ──────────────────────────────────

TEST_CASE("0x007fd440 sorts effects", "[spell][description][mods]") {
    const auto school = spellEffectKind(2, 0);
    CHECK(school.whole);
    CHECK(school.damage);
    CHECK_FALSE(school.periodic);
    const auto dot = spellEffectKind(6, 3);
    CHECK(dot.whole);
    CHECK(dot.damage);
    CHECK(dot.periodic);
    const auto shield = spellEffectKind(6, 15);
    CHECK(shield.damage);
    CHECK_FALSE(shield.periodic);
    const auto energize = spellEffectKind(30, 0);
    CHECK(energize.whole);
    CHECK_FALSE(energize.damage);
    const auto periodicMana = spellEffectKind(6, 24);
    CHECK(periodicMana.whole);
    CHECK_FALSE(periodicMana.damage);
    const auto stat = spellEffectKind(6, 29);
    CHECK_FALSE(stat.whole);
    CHECK_FALSE(stat.damage);
}

namespace {
/// A talent table: op -> {flat, pct}.
SpellModifierLookup talents(std::vector<std::tuple<uint8_t, int32_t, int32_t>> mods,
                            std::vector<uint8_t>* asked = nullptr) {
    return [mods, asked](uint8_t op) {
        if (asked) asked->push_back(op);
        wowee::game::spell_mods::Sum s;
        for (const auto& [o, flat, pct] : mods) {
            if (o != op) continue;
            s.flat += flat;
            s.pct += pct;
            s.any = true;
        }
        return s;
    };
}
}  // namespace

TEST_CASE("the talents' ops in 0x007ff770's order", "[spell][description][mods]") {
    std::vector<uint8_t> asked;
    // A direct damage first effect: all effects, then its own, then damage.
    finishSpellEffectPoints({100.0f, 100.0f}, 0, 2, 0, talents({}, &asked));
    CHECK(asked == std::vector<uint8_t>{8, 3, 0});
    asked.clear();
    // A periodic damage aura as the second effect: op 22, not 0.
    finishSpellEffectPoints({100.0f, 100.0f}, 1, 6, 3, talents({}, &asked));
    CHECK(asked == std::vector<uint8_t>{8, 12, 22});
    asked.clear();
    // A threat aura as the third: its op, and threat's.
    finishSpellEffectPoints({100.0f, 100.0f}, 2, 6, 10, talents({}, &asked));
    CHECK(asked == std::vector<uint8_t>{8, 23, 2});
}

TEST_CASE("Corruption with ten percent more periodic damage", "[spell][description][mods]") {
    // 47813 rank 10 ticks 1080 at level 80; Improved Corruption 5/5 and
    // Contagion each add their percent through op 22.
    const auto p = finishSpellEffectPoints({1080.0f, 1080.0f}, 0, 6, 3, talents({{22, 0, 5}, {22, 0, 5}}));
    CHECK(p.min == Catch::Approx(1188.0f));
    CHECK(p.max == Catch::Approx(1188.0f));
    // A flat point and a percent: (value + flat) x pct.
    const auto q = finishSpellEffectPoints({10.0f, 10.0f}, 0, 2, 0, talents({{0, 2, 50}}));
    CHECK(q.min == Catch::Approx(18.0f));
}

TEST_CASE("a whole kind ends whole, the rest to a 128th", "[spell][description][mods]") {
    const auto dmg = finishSpellEffectPoints({20.5f, 22.25f}, 0, 2, 0, {});
    CHECK(dmg.min == Catch::Approx(20.0f));
    CHECK(dmg.max == Catch::Approx(23.0f));
    const auto stat = finishSpellEffectPoints({1.3f, 1.3f}, 0, 6, 29, {});
    CHECK(stat.min == Catch::Approx(166.0f / 128.0f));
    // The client's ceiling makes 1 of a zero (0x0095d500 under one).
    const auto zero = finishSpellEffectPoints({0.0f, 0.0f}, 0, 2, 0, {});
    CHECK(zero.min == Catch::Approx(0.0f));
    CHECK(zero.max == Catch::Approx(1.0f));
    const auto negative = finishSpellEffectPoints({-0.5f, -0.5f}, 0, 2, 0, {});
    CHECK(negative.min == Catch::Approx(-1.0f));
    CHECK(negative.max == Catch::Approx(0.0f));
    // An aura's tooltip passes no talents: the values stay as they are.
    const auto aura = finishSpellEffectPoints({1080.0f, 1080.0f}, 0, 6, 3, {});
    CHECK(aura.min == Catch::Approx(1080.0f));
}

TEST_CASE("Seed of Corruption's ticks under 25 percent read as a range", "[spell][description][mods]") {
    // 47836 effect 1: BasePoints 252, DieSides 1 - 253 a tick either way.
    // Op 22 at 125%: 316.25 (316.24997 in the client's own soft floats,
    // 0x0095d230 truncating; a 128th puts it back on 316.25). A periodic
    // damage aura ends whole, the least down and the most up (0x007ff9e0:
    // 0x0095d460 / 0x0095d500), so 316 and 317 - and $o, six ticks of each
    // (0x0057a16d), is "1896 to 1902": 0x0057a3d4 writes the spread for any
    // least and most that differ.
    const auto seed = finishSpellEffectPoints({253.0f, 253.0f}, 0, 6, 3, talents({{22, 0, 25}}));
    CHECK(seed.min == 316.0f);
    CHECK(seed.max == 317.0f);
    const auto total = spellPeriodicTotal(seed, 3000, 18000, false);
    CHECK(total.min == 1896.0f);
    CHECK(total.max == 1902.0f);
    // Corruption's 180 at 135% is 243 exactly: one number.
    const auto corruption = finishSpellEffectPoints({180.0f, 180.0f}, 0, 6, 3, talents({{22, 0, 35}}));
    CHECK(corruption.min == 243.0f);
    CHECK(corruption.max == 243.0f);
}

TEST_CASE("0x007ff770 in the client's own floats", "[spell][description][mods]") {
    // Each pair is what wow12340.exe's own 0x007ff770 returned for the row,
    // its code run under an emulator (Spell.dbc layout, the talent tables
    // 0xd3c658 / 0xd397d8 filled by hand). Its products and sums drop the
    // low bits rather than round them (0x0095d230, 0x0095d2f0).
    struct Case {
        uint32_t effect, aura;
        int32_t basePoints, dieSides;
        float perLevel;
        int level;
        uint8_t op;
        int32_t flat, pct;
        float min, max;
    };
    const Case cases[] = {
        // 189999.98 is no 128th from 190000 in a float that size: down.
        {2, 0, 99999, 1, 0.0f, 80, 3, 0, 90, 189999.0f, 190000.0f},
        {6, 29, 29999, 1, 0.0f, 79, 3, 0, 112, 63599.9921875f, 63599.9921875f},
        {6, 29, 190, 27, 1.9f, 79, 3, 0, -42, 197.8359375f, 212.9140625f},
        {6, 29, -24, 1, -4.4f, 79, 3, 0, -47, -196.4140625f, -196.4140625f},
        {6, 3, 252, 1, 0.0f, 80, 22, 0, 25, 316.0f, 317.0f},
        {6, 3, 252, 1, 0.0f, 80, 22, 0, 0, 253.0f, 253.0f},
        {6, 3, 144, 1, 0.0f, 80, 22, 0, 30, 188.0f, 189.0f},
        {2, 0, 10, 1, 0.0f, 1, 0, 3, -50, 7.0f, 7.0f},
    };
    for (const Case& c : cases) {
        const auto raw = spellEffectPoints(c.basePoints, c.dieSides, c.perLevel, c.level, 0, 0);
        const auto mods = c.pct != 0 || c.flat != 0 ? talents({{c.op, c.flat, c.pct}}) : SpellModifierLookup{};
        const auto p = finishSpellEffectPoints(raw, 0, c.effect, c.aura, mods);
        INFO(c.basePoints << " " << c.dieSides << " " << c.perLevel << " " << c.pct);
        CHECK(p.min == c.min);
        CHECK(p.max == c.max);
    }
}

TEST_CASE("0x008016c0 makes a value whole", "[spell][description][mods]") {
    using wowee::game::spell_mods::wholeEffectPoints;
    CHECK(wholeEffectPoints(3.0f) == 3);
    CHECK(wholeEffectPoints(2.5f) == 2);
    CHECK(wholeEffectPoints(2.99f) == 2);
    CHECK(wholeEffectPoints(0.0f) == 0);
    CHECK(wholeEffectPoints(-3.0f) == -3);
    CHECK(wholeEffectPoints(-2.75f) == -3);
    CHECK(wholeEffectPoints(-2.5f) == -2);
}

// ── Plural, gender and the text's own numbers ──────────────────────────────

TEST_CASE("$l picks the form the last number counts", "[spell][description][text]") {
    const std::string raw = "$lSekunde:Sekunden; lang";
    std::size_t at = 2;
    std::string form;
    REQUIRE(pickPluralForm(raw, at, 1, form));
    CHECK(form == "Sekunde");
    CHECK(raw.substr(at) == " lang");
    at = 2;
    REQUIRE(pickPluralForm(raw, at, 3, form));
    CHECK(form == "Sekunden");
    at = 2;
    REQUIRE(pickPluralForm(raw, at, 0, form));
    CHECK(form == "Sekunden");
    // Spaces in front and around the form go (0x00576ef0).
    const std::string spaced = "$l  Ziel : Ziele ;.";
    at = 2;
    REQUIRE(pickPluralForm(spaced, at, 2, form));
    CHECK(form == "Ziele");
    CHECK(spaced.substr(at) == ".");
    // One form only and a plural asked for: nothing written.
    const std::string single = "$lJahr;!";
    at = 2;
    REQUIRE(pickPluralForm(single, at, 5, form));
    CHECK(form.empty());
    CHECK(single.substr(at) == "!");
    // No ';': nothing taken.
    const std::string open = "$lJahr:Jahre";
    at = 2;
    CHECK_FALSE(pickPluralForm(open, at, 5, form));
}

TEST_CASE("$g picks by the player's sex", "[spell][description][text]") {
    // Earth Elemental Totem (2062): "$gden Zaubernden:die Zaubernde;".
    const std::string raw = "$gden Zaubernden:die Zaubernde; und";
    std::size_t at = 2;
    std::string form;
    REQUIRE(pickGenderForm(raw, at, false, form));
    CHECK(form == "den Zaubernden");
    CHECK(raw.substr(at) == " und");
    at = 2;
    REQUIRE(pickGenderForm(raw, at, true, form));
    CHECK(form == "die Zaubernde");
    // A third form is never the answer.
    const std::string three = "$ger:sie:es;";
    at = 2;
    REQUIRE(pickGenderForm(three, at, true, form));
    CHECK(form == "sie");
    at = 2;
    CHECK_FALSE(pickGenderForm(std::string("$gnone"), at, true, form));
}

TEST_CASE("${...} is written %.0f, or with the decimals after it", "[spell][description][text]") {
    CHECK(formatSpellExpressionValue(36.01f, 0) == "36");
    CHECK(formatSpellExpressionValue(2.5f, 0) == "3");
    CHECK(formatSpellExpressionValue(-2.5f, 0) == "-3");
    // Ferocious Bite (22568): "${$f1+$AP/410}.1".
    CHECK(formatSpellExpressionValue(9.26f, 1) == "9.3");
    CHECK(formatSpellExpressionValue(1.0f, 2) == "1.00");
}

TEST_CASE("$F rounds halves away from zero (0x005773b0)", "[spell][description][text]") {
    CHECK(spellRoundHalfAway(2.5f) == 3);
    CHECK(spellRoundHalfAway(2.4f) == 2);
    CHECK(spellRoundHalfAway(-2.5f) == -3);
    CHECK(spellRoundHalfAway(0.0f) == 0);
}

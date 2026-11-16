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

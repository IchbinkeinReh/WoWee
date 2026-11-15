#include "game/spell_description_eval.hpp"

#include "ui/plural_escape.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace wowee {
namespace game {
namespace {

struct Parser {
    const SpellDescriptionContext& ctx;
    int depth = 0;   ///< a row may name itself; stop rather than recurse forever

    /// How deep a chain of declarations and conditions may go. Not eight:
    /// Frostbolt's row (SpellDescriptionVariables 167) chains five talent
    /// ranks, $arctic5 asking $arctic4 down to $arctic1, and each step is a
    /// declaration and a condition - ten deep before its multiplier is known.
    /// At eight the whole ${$m2*$<mult>} failed and the tooltip read "der bis
    /// Frostschaden verursacht", with no numbers at all.
    static constexpr int kMaxDepth = 64;

    bool value(const std::string& text, std::size_t& i, double& out);
    bool expression(const std::string& text, std::size_t& i, double& out);

    bool whole(const std::string& text, double& out) {
        std::size_t i = 0;
        if (!expression(text, i, out)) return false;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        return i >= text.size();
    }
};

/// One term: a number, a parenthesised expression, or a $ token.
bool Parser::value(const std::string& text, std::size_t& i, double& out) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) return false;

    if (text[i] == '(') {
        ++i;
        if (!expression(text, i, out)) return false;
        if (i >= text.size() || text[i] != ')') return false;
        ++i;
        return true;
    }

    if (text[i] == '-') {
        ++i;
        double v = 0.0;
        if (!value(text, i, v)) return false;
        out = -v;
        return true;
    }

    if (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.') {
        std::size_t used = 0;
        try {
            out = std::stod(text.substr(i), &used);
        } catch (...) {
            return false;
        }
        i += used;
        return true;
    }

    if (text[i] != '$') return false;
    ++i;
    if (i >= text.size()) return false;

    // ${ arithmetic }
    if (text[i] == '{') {
        ++i;
        if (!expression(text, i, out)) return false;
        if (i >= text.size() || text[i] != '}') return false;
        ++i;
        return true;
    }

    // $<name>, another declaration in the same row
    if (text[i] == '<') {
        const std::size_t close = text.find('>', i + 1);
        if (close == std::string::npos) return false;
        const std::string name = text.substr(i + 1, close - i - 1);
        i = close + 1;
        if (depth > kMaxDepth) return false;
        std::string decl;
        if (!spellDeclarationOf(ctx, name, decl)) return false;
        Parser nested{ctx, depth + 1};
        return nested.whole(decl, out);
    }

    // $?s<id>[then][else]
    if (text[i] == '?') {
        ++i;
        if (i >= text.size() || (text[i] != 's' && text[i] != 'S')) return false;
        ++i;
        uint32_t condId = 0;
        bool any = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            condId = condId * 10 + static_cast<uint32_t>(text[i] - '0');
            any = true;
            ++i;
        }
        if (!any) return false;

        // Both branches are read whichever is taken, so a malformed one fails
        // the expression rather than passing unnoticed behind a condition that
        // happened not to choose it.
        auto bracketed = [&](std::string& body) -> bool {
            if (i >= text.size() || text[i] != '[') return false;
            int open = 0;
            const std::size_t start = ++i;
            for (; i < text.size(); ++i) {
                if (text[i] == '[') {
                    ++open;
                } else if (text[i] == ']') {
                    if (open == 0) {
                        body = text.substr(start, i - start);
                        ++i;
                        return true;
                    }
                    --open;
                }
            }
            return false;
        };
        std::string thenBody;
        std::string elseBody;
        if (!bracketed(thenBody)) return false;
        if (!bracketed(elseBody)) return false;
        if (depth > kMaxDepth) return false;

        const bool known = ctx.knowsSpell && ctx.knowsSpell(condId);
        Parser nested{ctx, depth + 1};
        return nested.whole(known ? thenBody : elseBody, out);
    }

    // $m1 / $M1 / $s1 / $o1 - the spell's own magnitudes - with another
    // spell's number in front for that spell's: "${$72368m1*1000*$72368d/
    // $72368t1}". $d is the duration in seconds, $t1 the period in
    // milliseconds (0x005782d0).
    uint32_t refId = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        refId = refId * 10 + static_cast<uint32_t>(text[i] - '0');
        ++i;
    }
    if (i >= text.size()) return false;
    const char code = text[i++];
    int index = 0;
    if (i < text.size() && text[i] >= '1' && text[i] <= '3') {
        index = text[i] - '1';
        ++i;
    }
    switch (code) {
        case 's': case 'S': case 'm': case 'M': case 'o': case 'O':
            if (refId != 0) return ctx.quantity && ctx.quantity(refId, code, index, out);
            return ctx.magnitude && ctx.magnitude(code, index, out);
        case 'd': case 'D': case 't': case 'T':
            return ctx.quantity && ctx.quantity(refId, code, index, out);
        default:
            // $AP, $b1, $PL and the rest are quantities this client does not
            // keep. Refusing is better than standing in a number for them:
            // a tooltip with the wrong figure is worse than one without it.
            return false;
    }
}

bool Parser::expression(const std::string& text, std::size_t& i, double& out) {
    if (!value(text, i, out)) return false;
    while (true) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) return true;
        const char op = text[i];
        if (op != '+' && op != '-' && op != '*' && op != '/') return true;
        ++i;
        double rhs = 0.0;
        if (!value(text, i, rhs)) return false;
        switch (op) {
            case '+': out += rhs; break;
            case '-': out -= rhs; break;
            case '*': out *= rhs; break;
            case '/':
                if (std::abs(rhs) < 1e-9) return false;
                out /= rhs;
                break;
            default: return false;
        }
    }
}

}  // namespace

SpellEffectPoints spellEffectPoints(int32_t basePoints, int32_t dieSides,
                                    float pointsPerLevel, int casterLevel,
                                    int baseLevel, int maxLevel) {
    int level = casterLevel;
    if (maxLevel > 0 && maxLevel <= level) level = maxLevel;
    level -= baseLevel;
    if (level < 0) level = 0;
    const float perLevel = pointsPerLevel * static_cast<float>(level);
    SpellEffectPoints p;
    p.min = static_cast<float>(basePoints + 1) + perLevel;
    p.max = static_cast<float>(basePoints + dieSides) + perLevel;
    return p;
}

std::string formatSpellNumbers(const std::string& fmt, std::initializer_list<double> args) {
    const std::vector<double> values(args);
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
        std::size_t j = i + 1;
        if (fmt[j] == '%') { out += '%'; i = j; continue; }
        std::size_t index = next;
        bool positional = false;
        if (std::isdigit(static_cast<unsigned char>(fmt[j])) && j + 1 < fmt.size() && fmt[j + 1] == '$') {
            index = static_cast<std::size_t>(fmt[j] - '1');
            positional = true;
            j += 2;
        }
        // Flags, width and precision pass through to snprintf as written.
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j]) != nullptr) spec += fmt[j++];
        if (j >= fmt.size()) { out += fmt.substr(i); break; }
        const char conv = fmt[j];
        if (conv != 'd' && conv != 'i' && conv != 'f' && conv != 'g') { out += fmt[i]; continue; }
        if (!positional) ++next;
        const double v = index < values.size() ? values[index] : 0.0;
        char buf[64];
        if (conv == 'd' || conv == 'i') {
            spec += "ld";
            std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<long>(v));
        } else {
            spec += conv;
            std::snprintf(buf, sizeof(buf), spec.c_str(), v);
        }
        out += buf;
        i = j;
    }
    return out;
}

namespace {
/// 0x00577370: to the nearest whole number.
long nearest(float v) { return std::lround(v); }
/// Within a thousandth of a whole number, either side (0x00579e50).
bool isWhole(float v) {
    return v - std::floor(v) < 0.001f || std::ceil(v) - v < 0.001f;
}
std::string oneDecimal(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
    return buf;
}
}  // namespace

SpellPointsText formatSpellPoints(char code, const SpellEffectPoints& points,
                                  const std::string& intSpreadTemplate,
                                  const std::string& spreadTemplate) {
    const float lo = std::fabs(points.min);
    const float hi = std::fabs(points.max);
    const bool loWhole = isWhole(lo);
    const bool hiWhole = isWhole(hi);
    const long loN = nearest(lo);
    long hiN = nearest(hi);

    SpellPointsText r;
    // What a following $l counts: the number shown, or 2 - "more than one" -
    // for a fraction.
    r.shown = (code == 'm') ? (loWhole ? loN : 2) : (hiWhole ? hiN : 2);

    if (code == 'm') {
        r.text = loWhole ? std::to_string(loN) : oneDecimal(lo);
        return r;
    }
    if (code == 'M') {
        r.text = hiWhole ? std::to_string(hiN) : oneDecimal(hi);
        return r;
    }
    // $s / $S: one number when the effect cannot vary, a spread when it can.
    if (points.max == points.min) {
        r.text = (!loWhole && code == 'S') ? oneDecimal(lo) : std::to_string(loN);
        return r;
    }
    if (!loWhole || !hiWhole) {
        if (code == 'S') {
            r.text = formatSpellNumbers(spreadTemplate, {lo, hi});
            return r;
        }
        // Whole numbers, the least cut down and the most rounded up.
        if (!hiWhole) hiN = static_cast<long>(hi) + 1;
        const long loCut = loWhole ? loN : static_cast<long>(lo);
        r.text = formatSpellNumbers(intSpreadTemplate,
                                    {static_cast<double>(loCut), static_cast<double>(hiN)});
        return r;
    }
    r.text = formatSpellNumbers(intSpreadTemplate, {static_cast<double>(loN), static_cast<double>(hiN)});
    return r;
}

int spellDurationMs(int baseMs, int perLevelMs, int maxMs, int casterLevel, int baseLevel) {
    const int grown = (casterLevel - baseLevel) * perLevelMs + baseMs;
    return grown < maxMs ? grown : maxMs;
}

bool spellTimeIsWhole(float ms) {
    float v;
    if (ms >= 8.64e7f) v = ms * 1.1574074e-08f;        // days
    else if (ms >= 3.6e6f) v = ms * 2.777778e-07f;     // hours
    else if (ms >= 60000.0f) v = ms * 1.6666667e-05f;  // minutes
    else v = ms * 0.001f;                              // seconds
    return v - std::floor(v) < 0.01f;
}

namespace {
struct DurationKey {
    const char* key;
    const char* enUS;
};
}  // namespace

std::string formatSpellDuration(int ms, const SpellTextSource& text) {
    if (ms < 1) return text("SPELL_DURATION_UNTIL_CANCELLED", "until cancelled");
    constexpr int kMinute = 60000, kHour = kMinute * 60, kDay = kHour * 24;
    std::string formatted;
    if (spellTimeIsWhole(static_cast<float>(ms))) {
        // 0x0061a9e0, rounding down.
        DurationKey k{"INT_SPELL_DURATION_SEC", "%d sec"};
        int n = ms / 1000;
        if (ms >= kDay) {
            n = ms / kDay; k = {"INT_SPELL_DURATION_DAYS", "%d |4day:days;"};
        } else if (ms >= kHour) {
            n = ms / kHour;
            if (n == 24) { n = 1; k = {"INT_SPELL_DURATION_DAYS", "%d |4day:days;"}; }
            else k = {"INT_SPELL_DURATION_HOURS", "%d |4hour:hrs;"};
        } else if (ms >= kMinute) {
            n = ms / kMinute;
            if (n == 60) { n = 1; k = {"INT_SPELL_DURATION_HOURS", "%d |4hour:hrs;"}; }
            else k = {"INT_SPELL_DURATION_MIN", "%d min"};
        }
        formatted = formatSpellNumbers(text(k.key, k.enUS), {static_cast<double>(n)});
    } else {
        // 0x0061aee0: the same units, with a fraction.
        const float f = static_cast<float>(ms);
        DurationKey k{"SPELL_DURATION_SEC", "%.2f sec"};
        float v = f * 0.001f;
        if (ms >= kDay) { v = f / 8.64e7f; k = {"SPELL_DURATION_DAYS", "%.2f |4day:days;"}; }
        else if (ms >= kHour) { v = f / 3.6e6f; k = {"SPELL_DURATION_HOURS", "%.2f |4hour:hrs;"}; }
        else if (ms >= kMinute) { v = f * 1.6666667e-05f; k = {"SPELL_DURATION_MIN", "%.2f min"}; }
        formatted = formatSpellNumbers(text(k.key, k.enUS), {static_cast<double>(v)});
    }
    return ui::resolvePluralEscapes(formatted);
}

std::string formatSpellPeriod(int ms) {
    char buf[32];
    if (spellTimeIsWhole(static_cast<float>(ms)))
        std::snprintf(buf, sizeof(buf), "%d", ms / 1000);
    else
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(static_cast<float>(ms) * 0.001f));
    return buf;
}

SpellEffectPoints spellPeriodicTotal(const SpellEffectPoints& tick, int amplitudeMs,
                                     int durationMs, bool tickOnApply) {
    int period = amplitudeMs;
    if (period == 0) period = 5000;
    if (period < 0 || durationMs <= 0) return {};
    const float span = static_cast<float>((tickOnApply ? period : 0) + durationMs);
    const float p = static_cast<float>(period);
    return {tick.min * span / p, tick.max * span / p};
}

bool spellDeclarationOf(const SpellDescriptionContext& ctx,
                        const std::string& name, std::string& out) {
    if (ctx.declarations == nullptr || name.empty()) return false;
    const std::string needle = "$" + name + "=";
    const std::size_t at = ctx.declarations->find(needle);
    if (at == std::string::npos) return false;
    const std::size_t from = at + needle.size();
    const std::size_t end = ctx.declarations->find_first_of("\r\n", from);
    out = ctx.declarations->substr(from,
                                   end == std::string::npos ? std::string::npos : end - from);
    return true;
}

bool evaluateSpellExpression(const std::string& text,
                             const SpellDescriptionContext& ctx, double& out) {
    if (text.empty()) return false;
    Parser parser{ctx, 0};
    return parser.whole(text, out);
}

}  // namespace game
}  // namespace wowee

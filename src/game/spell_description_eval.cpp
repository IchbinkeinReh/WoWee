#include "game/spell_description_eval.hpp"

#include "ui/plural_escape.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace wowee {
namespace game {
namespace {

/// 0x005797f0 over [p, end): terms left to right, '&' stopping at the first
/// false and '|' at the first true, '!' turning the next term or group.
bool branchCondition(const std::string& s, std::size_t& p, std::size_t end,
                     const std::function<bool(char, uint32_t)>& holds, int depth) {
    bool value = false;
    while (p < end) {
        const char c = s[p];
        if (c == ')') return value;
        if (c == '&' || c == '|') {
            if (c == '&' && !value) return false;
            if (c == '|' && value) return true;
            ++p;
            value = depth < 16 && branchCondition(s, p, end, holds, depth + 1);
            continue;
        }
        bool negate = false;
        std::size_t q = p;
        while (q < end && s[q] == '!') { negate = !negate; ++q; }
        if (q < end && s[q] == '(') {
            p = q + 1;
            value = depth < 16 && branchCondition(s, p, end, holds, depth + 1);
            if (p < end && s[p] == ')') ++p;
            if (negate) value = !value;
            continue;
        }
        if (q < end && (s[q] == 's' || s[q] == 'S' || s[q] == 'a' || s[q] == 'A')) {
            const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(s[q])));
            ++q;
            uint32_t id = 0;
            while (q < end && std::isdigit(static_cast<unsigned char>(s[q]))) id = id * 10 + static_cast<uint32_t>(s[q++] - '0');
            value = holds && holds(letter, id);
            if (negate) value = !value;
            p = q;
            continue;
        }
        p = q + 1;  // anything else is stepped over, as the client steps over it
    }
    return value;
}

}  // namespace

DescriptionBranch pickDescriptionBranch(const std::string& raw, std::size_t at,
                                        const std::function<bool(char, uint32_t)>& holds) {
    DescriptionBranch out;
    std::size_t condAt = at;
    for (int links = 0; links < 16; ++links) {
        const std::size_t open = raw.find('[', condAt);
        if (open == std::string::npos) return out;
        const std::size_t close = raw.find(']', open);
        if (close == std::string::npos) return out;
        std::size_t p = condAt;
        if (branchCondition(raw, p, open, holds, 0)) {
            // Taken: find where the chain ends, past every later branch.
            std::size_t tail = close;
            while (tail + 1 < raw.size() && raw[tail + 1] == '?') {
                const std::size_t o = raw.find('[', tail + 2);
                if (o == std::string::npos) return out;
                tail = raw.find(']', o);
                if (tail == std::string::npos) return out;
            }
            const std::size_t elseOpen = tail + 1;
            if (elseOpen >= raw.size() || raw[elseOpen] != '[') return out;
            const std::size_t elseClose = raw.find(']', elseOpen);
            if (elseClose == std::string::npos) return out;
            out = {true, elseClose + 1, raw.substr(open + 1, close - open - 1)};
            return out;
        }
        if (close + 1 < raw.size() && raw[close + 1] == '?') {
            condAt = close + 2;
            continue;
        }
        const std::size_t elseOpen = close + 1;
        if (elseOpen >= raw.size() || raw[elseOpen] != '[') return out;
        const std::size_t elseClose = raw.find(']', elseOpen);
        if (elseClose == std::string::npos) return out;
        out = {true, elseClose + 1, raw.substr(elseOpen + 1, elseClose - elseOpen - 1)};
        return out;
    }
    return out;
}

namespace {

/// The variables an expression may name (0x00ace8f8, 140 of them, ids 0x16
/// on in 0x005782d0), compared by exact case (0x00576b50, 0x0076e760).
constexpr const char* kVariables[] = {
    "STR", "AGI", "STA", "INT", "SPI", "str", "agi", "sta", "int", "spi",
    "m1", "m2", "m3", "M1", "M2", "M3", "a1", "a2", "a3", "A1", "A2", "A3",
    "d", "D", "c", "C", "p", "P", "x1", "x2", "x3", "X1", "X2", "X3",
    "t1", "t2", "t3", "T1", "T2", "T3", "h", "H", "n", "N",
    "b1", "b2", "b3", "B1", "B2", "B3", "u", "U", "v", "V",
    "e1", "e2", "e3", "E1", "E2", "E3", "i", "I", "f1", "f2", "f3", "F1", "F2", "F3",
    "q1", "q2", "q3", "Q1", "Q2", "Q3", "ap", "AP", "rap", "RAP",
    "mwb", "MWB", "owb", "OWB", "rwb", "RWB", "mw", "MW", "ow", "OW", "rw", "RW", "ar", "AR",
    "mws", "MWS", "ows", "OWS", "rws", "RWS", "pl", "PL", "hnd", "HND", "sp", "SP",
    "sph", "spfi", "spn", "spfr", "sps", "spa", "SPH", "SPFI", "SPN", "SPFR", "SPS", "SPA",
    "bh", "BH", "ph", "pfi", "pn", "pfr", "ps", "pa", "PH", "PFI", "PN", "PFR", "PS", "PA",
    "pbh", "pBH", "pbhd", "pBHD", "bc1", "bc2", "bc3", "BC1", "BC2", "BC3"};
static_assert(std::size(kVariables) == 0x8c, "0x00576b50 compares 0x8c names");

bool isVariable(const std::string& name) {
    for (const char* v : kVariables)
        if (name == v) return true;
    return false;
}

/// The functions (0x00ace898: name and argument count), by any case
/// (0x00576b10, 0x0076e780), ids 10 to 21 in 0x00577900.
struct Function {
    const char* name;
    int argc;
};
constexpr Function kFunctions[] = {{"abs", 1}, {"ceil", 1}, {"floor", 1}, {"min", 2},  {"max", 2},  {"gt", 2},
                                   {"lt", 2},  {"gte", 2},  {"lte", 2},   {"eq", 2},   {"cond", 3}, {"clamp", 3}};

int functionOf(const std::string& name) {
    for (int k = 0; k < static_cast<int>(std::size(kFunctions)); ++k) {
        const char* f = kFunctions[k].name;
        if (name.size() != std::strlen(f)) continue;
        bool same = true;
        for (std::size_t c = 0; c < name.size() && same; ++c)
            same = std::tolower(static_cast<unsigned char>(name[c])) == f[c];
        if (same) return k;
    }
    return -1;
}

/// 0x00577900: one function over its arguments, in the client's floats.
float callFunction(int k, const float* a) {
    switch (k) {
        case 0: return std::fabs(a[0]);
        case 1: return std::ceil(a[0]);
        case 2: return std::floor(a[0]);
        case 3: return a[0] < a[1] ? a[0] : a[1];
        case 4: return a[1] < a[0] ? a[0] : a[1];
        case 5: return a[1] < a[0] ? 1.0f : 0.0f;
        case 6: return a[0] < a[1] ? 1.0f : 0.0f;
        case 7: return a[0] >= a[1] ? 1.0f : 0.0f;
        case 8: return a[0] <= a[1] ? 1.0f : 0.0f;
        case 9: return std::fabs(a[0] - a[1]) < 0.0001f ? 1.0f : 0.0f;
        case 10: return a[0] == 0.0f ? a[2] : a[1];
        default: {
            // clamp(x, lo, hi): hi where the larger of x and lo reaches it.
            const float larger = a[0] < a[1] ? a[1] : a[0];
            if (larger < a[2]) return larger;
            return a[2];
        }
    }
}

/// What ends a word of an expression: 0x00576bb0's delimiters
/// ("()[]^*/%+-#,> ", 0x00a11544) - and the brace of a nested "${...}".
bool endsWord(char c) {
    return c == '\0' || std::strchr("()[]^*/%+-#,> }\t\r\n", c) != nullptr;
}

void skipSpaces(const std::string& text, std::size_t& i) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
}

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

    bool atom(const std::string& text, std::size_t& i, float& out);
    bool power(const std::string& text, std::size_t& i, float& out);
    bool term(const std::string& text, std::size_t& i, float& out);
    bool expression(const std::string& text, std::size_t& i, float& out);
    bool variable(uint32_t refId, const std::string& name, float& out);

    bool whole(const std::string& text, float& out) {
        std::size_t i = 0;
        if (!expression(text, i, out)) return false;
        skipSpaces(text, i);
        return i >= text.size();
    }
};

bool Parser::variable(uint32_t refId, const std::string& name, float& out) {
    double v = 0.0;
    bool ok = false;
    const char c = name[0];
    const int index = name.size() == 2 && name[1] >= '1' && name[1] <= '3' ? name[1] - '1' : 0;
    if ((c == 'm' || c == 'M') && name.size() == 2) {
        ok = refId != 0 ? (ctx.quantity && ctx.quantity(refId, c, index, v))
                        : (ctx.magnitude && ctx.magnitude(c, index, v));
    } else if ((c == 'd' || c == 'D') && name.size() == 1) {
        ok = ctx.quantity && ctx.quantity(refId, 'd', 0, v);
    } else if ((c == 't' || c == 'T') && name.size() == 2) {
        ok = ctx.quantity && ctx.quantity(refId, 't', index, v);
    } else {
        ok = ctx.variable && ctx.variable(refId, name, v);
    }
    out = static_cast<float>(v);
    return ok;
}

/// One operand (0x00577530): a parenthesised expression, a negated operand,
/// a number, a function call, a variable or a declaration.
bool Parser::atom(const std::string& text, std::size_t& i, float& out) {
    skipSpaces(text, i);
    if (i >= text.size()) return false;

    if (text[i] == '(') {
        ++i;
        if (!expression(text, i, out)) return false;
        skipSpaces(text, i);
        if (i >= text.size() || text[i] != ')') return false;
        ++i;
        return true;
    }
    if (text[i] == '-') {
        // The sign takes only the operand after it: -2^2 is 4.
        ++i;
        float v = 0.0f;
        if (!atom(text, i, v)) return false;
        out = -v;
        return true;
    }
    if (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.') {
        // The word through atof, which stops where the number does.
        std::size_t end = i;
        while (end < text.size() && !endsWord(text[end])) ++end;
        out = static_cast<float>(std::atof(text.substr(i, end - i).c_str()));
        i = end;
        return true;
    }
    if (text[i] != '$') return false;
    ++i;
    if (i >= text.size()) return false;

    // ${ arithmetic } - a declaration's right-hand side.
    if (text[i] == '{') {
        ++i;
        if (!expression(text, i, out)) return false;
        skipSpaces(text, i);
        if (i >= text.size() || text[i] != '}') return false;
        ++i;
        return true;
    }

    // $<name>: a declaration of the row; one it does not have is 0
    // (0x005774d0).
    if (text[i] == '<') {
        const std::size_t close = text.find('>', i + 1);
        if (close == std::string::npos) return false;
        const std::string name = text.substr(i + 1, close - i - 1);
        i = close + 1;
        if (depth > kMaxDepth) return false;
        std::string decl;
        if (!spellDeclarationOf(ctx, name, decl)) {
            out = 0.0f;
            return true;
        }
        Parser nested{ctx, depth + 1};
        return nested.whole(decl, out);
    }

    // $?s<id>[then][else], as a declaration is written.
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

    // The rest of the word: a function, or another spell's number and then
    // a variable.
    std::size_t end = i;
    while (end < text.size() && !endsWord(text[end])) ++end;
    const std::string word = text.substr(i, end - i);
    i = end;
    if (word.empty()) return false;

    if (const int f = functionOf(word); f >= 0) {
        skipSpaces(text, i);
        if (i >= text.size() || text[i] != '(') return false;
        ++i;
        float args[3] = {0.0f, 0.0f, 0.0f};
        for (int a = 0; a < kFunctions[f].argc; ++a) {
            if (a > 0) {
                skipSpaces(text, i);
                if (i >= text.size() || text[i] != ',') return false;
                ++i;
            }
            if (!expression(text, i, args[a])) return false;
        }
        skipSpaces(text, i);
        if (i >= text.size() || text[i] != ')') return false;
        ++i;
        out = callFunction(f, args);
        return true;
    }

    std::size_t d = 0;
    uint32_t refId = 0;
    while (d < word.size() && std::isdigit(static_cast<unsigned char>(word[d]))) {
        refId = refId * 10 + static_cast<uint32_t>(word[d] - '0');
        ++d;
    }
    const std::string name = word.substr(d);
    if (name.empty() || !isVariable(name)) return false;
    return variable(refId, name, out);
}

/// 0x005777a0: operands joined by '^', left to right.
bool Parser::power(const std::string& text, std::size_t& i, float& out) {
    if (!atom(text, i, out)) return false;
    while (true) {
        skipSpaces(text, i);
        if (i >= text.size() || text[i] != '^') return true;
        ++i;
        float rhs = 0.0f;
        if (!atom(text, i, rhs)) return false;
        out = static_cast<float>(std::pow(static_cast<double>(out), static_cast<double>(rhs)));
    }
}

/// 0x005777f0: powers joined by '*', '/' and '%'. A division by nothing is
/// the x87's infinity, not a failure; '%' is the dividend less the fraction
/// of the quotient times the divisor (0x00577ad0 case 7) - 7 % 3 is 6.
bool Parser::term(const std::string& text, std::size_t& i, float& out) {
    if (!power(text, i, out)) return false;
    while (true) {
        skipSpaces(text, i);
        if (i >= text.size()) return true;
        const char op = text[i];
        if (op != '*' && op != '/' && op != '%') return true;
        ++i;
        float rhs = 0.0f;
        if (!power(text, i, rhs)) return false;
        if (op == '*') {
            out = out * rhs;
        } else if (op == '/') {
            out = out / rhs;
        } else {
            const float q = out / rhs;
            out = static_cast<float>(static_cast<double>(out) -
                                     (static_cast<double>(q) - std::floor(static_cast<double>(q))) * rhs);
        }
    }
}

/// 0x005778b0: terms joined by '+' and '-'.
bool Parser::expression(const std::string& text, std::size_t& i, float& out) {
    if (!term(text, i, out)) return false;
    while (true) {
        skipSpaces(text, i);
        if (i >= text.size()) return true;
        const char op = text[i];
        if (op != '+' && op != '-') return true;
        ++i;
        float rhs = 0.0f;
        if (!term(text, i, rhs)) return false;
        out = op == '+' ? out + rhs : out - rhs;
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

int hastedSpellDurationMs(int ms, bool periodic, bool hasteShortens, uint32_t attrEx3,
                          float castSpeed) {
    constexpr uint32_t kAttrEx3NoHaste = 0x20000000u;
    if (!periodic || !hasteShortens || (attrEx3 & kAttrEx3NoHaste) != 0) return ms;
    if (!(castSpeed > 0.001f)) return ms;
    // ROUND: the x87 store, to nearest.
    return static_cast<int>(std::nearbyint(static_cast<float>(ms) * castSpeed));
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
    float v = 0.0f;
    if (!parser.whole(text, v)) return false;
    out = v;
    return true;
}

SpellEffectKind spellEffectKind(uint32_t effect, uint32_t aura) {
    switch (effect) {
        case 2: case 9: case 10: case 17: case 31: case 58: case 67: case 75: case 121:
            return {true, true, false};
        case 8: case 30: case 62: case 146:
            return {true, false, false};
        case 6: case 27: case 35: case 65: case 119: case 128: case 129: case 143:
            switch (aura) {
                case 3: case 8: case 20: case 53: case 62: case 89:
                    return {true, true, true};
                case 15: case 43:
                    return {true, true, false};
                case 21: case 24: case 63: case 64: case 162:
                    return {true, false, false};
                default:
                    return {};
            }
        default:
            return {};
    }
}

namespace {
/// 0x0095d500 on a whole-number kind's most: under one in size its answer
/// is 0 for a negative and 1 otherwise - zero included (0x00dce2a8 holds
/// 1.0).
float clientCeil(float v) {
    if (std::fabs(v) < 1.0f) return (std::signbit(v) && v != 0.0f) ? 0.0f : 1.0f;
    return std::ceil(v);
}
/// 0x007ff770's rounding: to the nearest 128th (the exponent raised by 7,
/// 0.5 added and the floor taken, the exponent lowered again).
float nearest128th(float v) {
    if (v == 0.0f) return v;
    return std::floor(v * 128.0f + 0.5f) / 128.0f;
}
}  // namespace

SpellEffectPoints finishSpellEffectPoints(SpellEffectPoints raw, int index, uint32_t effect,
                                          uint32_t aura, const SpellModifierLookup& mods) {
    const SpellEffectKind kind = spellEffectKind(effect, aura);
    if (mods) {
        auto through = [&](uint8_t op) {
            const spell_mods::Sum s = mods(op);
            raw.min = spell_mods::apply(raw.min, s);
            raw.max = spell_mods::apply(raw.max, s);
        };
        through(8);
        if (index == 0) through(3);
        else if (index == 1) through(12);
        else if (index == 2) through(23);
        if (kind.damage) through(kind.periodic ? 22 : 0);
        if (aura == 10 || aura == 103 || aura == 183) through(2);
    }
    raw.min = nearest128th(raw.min);
    raw.max = nearest128th(raw.max);
    if (kind.whole) {
        raw.min = std::floor(raw.min);  // 0x0095d460
        raw.max = clientCeil(raw.max);
    }
    return raw;
}

long spellRoundHalfAway(float v) {
    // Truncated after the half is added or taken away (the x87 in its
    // chop mode).
    return v > 0.0f ? static_cast<long>(v + 0.5f) : static_cast<long>(v - 0.5f);
}

namespace {
/// [from, to) less its trailing spaces.
std::string trimmedRight(const std::string& raw, std::size_t from, std::size_t to) {
    while (to > from && raw[to - 1] == ' ') --to;
    return raw.substr(from, to - from);
}
}  // namespace

bool pickPluralForm(const std::string& raw, std::size_t& at, long count, std::string& out) {
    out.clear();
    std::size_t p = at;
    while (p < raw.size() && raw[p] == ' ') ++p;
    if (p >= raw.size()) return false;
    const std::size_t semi = raw.find(';', p);
    if (semi == std::string::npos) return false;
    at = semi + 1;
    // 0x00817d10 for the German and English clients: the second form for
    // any count but one.
    const int form = count != 1 ? 1 : 0;
    for (int k = 0; k < form; ++k) {
        while (p < semi && raw[p] != ':') ++p;
        if (p >= semi) return true;  // too few forms: nothing written
        ++p;
    }
    while (p < semi && raw[p] == ' ') ++p;
    std::size_t end = p;
    while (end < semi && raw[end] != ':') ++end;
    out = trimmedRight(raw, p, end);
    return true;
}

bool pickGenderForm(const std::string& raw, std::size_t& at, bool female, std::string& out) {
    out.clear();
    std::size_t p = at;
    while (p < raw.size() && raw[p] == ' ') ++p;
    if (p >= raw.size()) return false;
    const std::size_t colon = raw.find(':', p);
    if (colon == std::string::npos) return false;
    const std::size_t semi = raw.find(';', colon);
    if (semi == std::string::npos) return false;
    std::size_t second = raw.find(':', colon + 1);
    if (second != std::string::npos && second + 1 >= semi) second = std::string::npos;
    if (female) {
        std::size_t from = colon + 1;
        while (from < semi && raw[from] == ' ') ++from;
        const std::size_t to = second != std::string::npos ? second : semi;
        out = trimmedRight(raw, from, std::max(from, to));
    } else {
        out = trimmedRight(raw, p, colon);
    }
    at = semi + 1;
    return true;
}

std::string formatSpellExpressionValue(float v, int decimals) {
    if (decimals < 0 || decimals > 9) decimals = 0;
    char buf[64];
    if (decimals == 0) {
        // The CRT's "%.0f": halves away from zero.
        std::snprintf(buf, sizeof(buf), "%ld", std::lround(static_cast<double>(v)));
    } else {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    }
    return buf;
}

}  // namespace game
}  // namespace wowee

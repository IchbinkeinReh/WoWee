#include "ui/plural_escape.hpp"

#include <atomic>

namespace wowee {
namespace ui {

namespace {
// 0x00d3f7a0.
std::atomic<int> gPluralRule{0};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

int pluralRuleForLocale(const std::string& locale) {
    // 0x00405dd0: table indices 1, 2, 4, 5 (koKR, frFR, zhCN, zhTW) take
    // rule 1, index 8 (ruRU) rule 2, the rest 0.
    if (locale == "koKR" || locale == "frFR" || locale == "zhCN" || locale == "zhTW") return 1;
    if (locale == "ruRU") return 2;
    return 0;
}

int pluralFormIndex(int rule, long count) {
    // 0x00817d10.
    if (rule == 0) return count != 1 ? 1 : 0;
    if (rule == 1) return count > 1 ? 1 : 0;
    if (rule == 2) {
        // C's remainder keeps a negative count's sign, and the unsigned
        // compares then fail as the client's do.
        if (static_cast<unsigned long>(count % 100 - 11) < 4) return 2;
        if (count % 10 != 1) return static_cast<unsigned long>(count % 10 - 2) > 2 ? 2 : 1;
    }
    return 0;
}

void setPluralLocale(const std::string& locale) { gPluralRule.store(pluralRuleForLocale(locale)); }

int pluralFormIndex(long count) { return pluralFormIndex(gPluralRule.load(), count); }

std::string resolvePluralEscapes(const std::string& in) {
    if (in.find("|4") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    // 0x00481fc0: the running number, and whether the character before was
    // one of its digits.
    long count = 0;
    bool inNumber = false;
    size_t i = 0;
    while (i < n) {
        const char c = in[i];
        if (!isDigit(c)) {
            inNumber = false;
        } else if (inNumber) {
            count = count * 10 + (c - '0');
        } else {
            inNumber = true;
            count = c - '0';
        }
        if (c != '|') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (i + 1 >= n) { out.push_back(c); break; }
        // Not an escape numbered 4, or one followed by a digit: the bar and
        // the character after it are written as they are.
        if (in[i + 1] != '4' || (i + 2 < n && isDigit(in[i + 2]))) {
            out.push_back(c);
            out.push_back(in[i + 1]);
            i += 2;
            continue;
        }
        size_t p = i + 2;
        const int form = pluralFormIndex(count);
        // Past `form` colons, never past the ';' that closes the escape.
        for (int k = 0; k < form && p < n;) {
            while (p < n && in[p] != ':' && in[p] != ';') ++p;
            if (p >= n || in[p] == ';') break;
            ++k;
            ++p;
        }
        while (p < n && in[p] == ' ') ++p;
        if (p >= n) return out;
        while (p < n && in[p] != ':' && in[p] != ';') out.push_back(in[p++]);
        if (p >= n) return out;
        while (p < n && in[p] != ';') ++p;
        if (p >= n) return out;
        i = p + 1;
    }
    return out;
}

}  // namespace ui
}  // namespace wowee

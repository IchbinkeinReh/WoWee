#pragma once

// Spells told apart by name and rank the way the client's spellbook does it.
//
// A rank is text in Spell.dbc ("Rank 5", "Rang 5", "Rango 5"), so the number
// in it is read, not the word in front of it: 0x0053b540 takes the first run
// of digits and stops at the first character after it. Reading only an
// English "rank " prefix left every rank of a German client at zero, and the
// spell a name stood for was whichever rank the known-spell set happened to
// yield first - "/cast Schattenblitz" cast rank 2.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace game {
namespace spell_rank {

/// 0x0053b540: the number of a rank text - its first run of digits, 0 for
/// none.
[[nodiscard]] constexpr uint32_t rankNumber(std::string_view rank) {
    uint32_t n = 0;
    bool any = false;
    for (const char c : rank) {
        if (c >= '0' && c <= '9') {
            n = n * 10 + static_cast<uint32_t>(c - '0');
            any = true;
        } else if (any) {
            break;
        }
    }
    return n;
}

/// Byte-wise, ASCII letters folded - names and ranks are compared this way
/// (0x0076ea40 / 0x0076ec80).
[[nodiscard]] inline int compareNoCase(std::string_view a, std::string_view b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        auto fold = [](char c) {
            return static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        };
        const unsigned char x = fold(a[i]);
        const unsigned char y = fold(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

struct Candidate {
    uint32_t id = 0;
    std::string_view name;
    std::string_view rank;
};

/// The spell a name stands for (0x00540200 -> 0x0053f5e0): the whole text
/// as a name, every rank of it, the highest numbered one; failing that, the
/// text before "(" as the name and the text inside up to ")" as the rank,
/// matched as written. Of spells with no number, or the same one, the
/// highest id - a choice that does not change from one run to the next. 0
/// for none.
[[nodiscard]] inline uint32_t spellByName(const std::vector<Candidate>& known, std::string_view text) {
    if (!text.empty() && text.front() == '!') text.remove_prefix(1);
    if (text.empty()) return 0;
    uint32_t best = 0;
    uint32_t bestRank = 0;
    for (const Candidate& c : known) {
        if (compareNoCase(c.name, text) != 0) continue;
        const uint32_t r = rankNumber(c.rank);
        if (best == 0 || r > bestRank || (r == bestRank && c.id > best)) {
            best = c.id;
            bestRank = r;
        }
    }
    if (best != 0) return best;
    const size_t open = text.find('(');
    if (open == std::string_view::npos) return 0;
    std::string_view name = text.substr(0, open);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    std::string_view rank = text.substr(open + 1);
    if (const size_t close = rank.find(')'); close != std::string_view::npos) rank = rank.substr(0, close);
    for (const Candidate& c : known) {
        if (compareNoCase(c.name, name) == 0 && compareNoCase(c.rank, rank) == 0) return c.id;
    }
    return 0;
}

/// The spellbook's order (0x0053c8b0) within one tab: by name, then by the
/// rank's number (the spell's level where it has none), then by the rank
/// text. True when `a` goes before `b`.
[[nodiscard]] inline bool bookBefore(std::string_view nameA, std::string_view rankA, uint32_t levelA,
                                     std::string_view nameB, std::string_view rankB, uint32_t levelB) {
    if (const int c = compareNoCase(nameA, nameB); c != 0) return c < 0;
    uint32_t ra = rankNumber(rankA);
    uint32_t rb = rankNumber(rankB);
    if (ra == 0) ra = levelA;
    if (rb == 0) rb = levelB;
    if (ra != 0 && rb != 0) return ra < rb;
    return compareNoCase(rankA, rankB) < 0;
}

} // namespace spell_rank
} // namespace game
} // namespace wowee

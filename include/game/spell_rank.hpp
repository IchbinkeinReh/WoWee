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
    /// Spell.dbc SpellLevel, which stands in for a rank with no number when
    /// the ranks of one name are put in order (0x00541df0).
    uint32_t level = 0;
};

/// Whether a spell the player knows is one a name can stand for: the
/// client's name list (0x00541df0) is filled from 0x00542030 after it has
/// passed over a spell hidden from the book (Attributes 0x80) and a trade
/// skill's recipe (0x20). A talent's own spell is such a hidden one -
/// Metamorphosis' talent 59672 only teaches the form, 47241 - and as one
/// more "Metamorphose" with no rank it used to win the name, so casting it
/// by name or key sent the teaching spell and the server answered "Ihr
/// beherrscht bereits Metamorphose".
[[nodiscard]] constexpr bool inNameList(uint32_t attributes) {
    return (attributes & 0xA0u) == 0;
}

/// Whether a spell the player knows is in the player's spellbook
/// (0x00542030): in the name list, not a companion or mount (0x008009b0 -
/// the caller asks that), and not AttributesEx4 0x8000.
[[nodiscard]] constexpr bool inPlayerBook(uint32_t attributes, uint32_t attributesEx4) {
    return inNameList(attributes) && (attributesEx4 & 0x8000u) == 0;
}

/// The spell of a name and a rank (0x0053f5e0): the rank matched as written, or
/// with none the highest numbered one (a rank with no number by its SpellLevel,
/// as 0x00541df0 files it). Of spells with the same number, the highest id - a
/// choice that does not change from one run to the next. 0 for none.
/// `known` holds only what is in the name list (inNameList).
[[nodiscard]] inline uint32_t spellByNameAndRank(const std::vector<Candidate>& known, std::string_view name,
                                                 std::string_view rank) {
    uint32_t best = 0;
    uint32_t bestRank = 0;
    for (const Candidate& c : known) {
        if (name.empty() || compareNoCase(c.name, name) != 0) continue;
        if (!rank.empty()) {
            if (compareNoCase(c.rank, rank) == 0) return c.id;
            continue;
        }
        uint32_t r = rankNumber(c.rank);
        if (r == 0) r = c.level;
        if (best == 0 || r > bestRank || (r == bestRank && c.id > best)) {
            best = c.id;
            bestRank = r;
        }
    }
    return best;
}

/// The spell a text stands for (0x00540200): the whole text as a name; failing
/// that, the text before "(" as the name and the text inside up to ")" as the
/// rank.
[[nodiscard]] inline uint32_t spellByName(const std::vector<Candidate>& known, std::string_view text) {
    if (!text.empty() && text.front() == '!') text.remove_prefix(1);
    if (const uint32_t id = spellByNameAndRank(known, text, {})) return id;
    const size_t open = text.find('(');
    if (open == std::string_view::npos) return 0;
    std::string_view name = text.substr(0, open);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    std::string_view rank = text.substr(open + 1);
    if (const size_t close = rank.find(')'); close != std::string_view::npos) rank = rank.substr(0, close);
    return spellByNameAndRank(known, name, rank);
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

/// One entry of a spellbook tab, as the book is ordered.
struct BookEntry {
    std::string_view name;
    std::string_view rank;
};

/// The book's second numbering, the one it shows with ShowAllSpellRanks off
/// (0x0053ca70, read back by GetSpellTabInfo's last pair and
/// GetKnownSlotFromHighestRankSlot): of a tab in book order, the indices
/// kept. An entry is dropped when the one after it has the same name and a
/// rank with a number - so of a run of ranks only the last, the highest,
/// stays, and spells with no rank number all stay.
[[nodiscard]] inline std::vector<size_t> highestRankSlots(const std::vector<BookEntry>& tab) {
    std::vector<size_t> kept;
    kept.reserve(tab.size());
    for (size_t i = 0; i < tab.size(); ++i) {
        const bool last = i + 1 == tab.size();
        if (last || tab[i + 1].name != tab[i].name || rankNumber(tab[i + 1].rank) == 0) kept.push_back(i);
    }
    return kept;
}

} // namespace spell_rank
} // namespace game
} // namespace wowee

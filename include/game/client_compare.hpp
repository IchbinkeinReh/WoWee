#pragma once

#include <cstddef>
#include <string>

namespace wowee::game {

namespace client_compare {

/// 0x0076e7a0: the next character (UTF-8) and its capital, 0 at the end.
inline char32_t next(const std::string& s, size_t& at, char32_t& capital) {
    capital = 0;
    if (at >= s.size()) return 0;
    const auto first = static_cast<unsigned char>(s[at++]);
    const int length = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
    char32_t c = length == 1 ? first : first & (0xFFu >> (length + 1));
    for (int k = 1; k < length && at < s.size(); ++k)
        c = (c << 6) | (static_cast<unsigned char>(s[at++]) & 0x3Fu);
    if (c > 0xFFFF) c = 0xFFFD;
    capital = c;
    if (c == 0x153) capital = 0x152;
    else if (c == 0x451) capital = 0x401;
    else if ((c >= U'a' && c <= U'z') || (c >= 0xE0 && c <= 0xFE) || (c >= 0x430 && c <= 0x44F)) capital = c - 0x20;
    return c;
}

/// 0x0076eab0: the letter a capital is sorted as, an accented one as without.
inline char32_t weight(char32_t capital) {
    if (capital >= 0xC0 && capital <= 0xC6) return U'A';
    if (capital == 0xC7) return U'C';
    if (capital >= 0xC8 && capital <= 0xCB) return U'E';
    if (capital >= 0xCC && capital <= 0xCF) return U'I';
    if (capital == 0xD1) return U'N';
    if ((capital >= 0xD2 && capital <= 0xD6) || capital == 0x152) return U'O';
    if (capital >= 0xD9 && capital <= 0xDC) return U'U';
    if (capital == 0xDF) return U'S';
    return capital == 0x401 ? 0x415 : capital;
}

/// 0x0076eb80: sorted as one letter - where one of them stands for two (ae, ss,
/// oe), the other string having the second as well.
inline bool same(const std::string& a, size_t& i, char32_t ca, const std::string& b, size_t& j, char32_t cb) {
    if (weight(ca) != weight(cb)) return false;
    if (ca == cb) return true;
    const auto second = [](const std::string& s, size_t& at, char32_t wanted) {
        size_t after = at;
        char32_t capital = 0;
        next(s, after, capital);
        if (weight(capital) != wanted) return false;
        at = after;
        return true;
    };
    if (ca == 0xC6) return second(b, j, U'E');
    if (cb == 0xC6) return second(a, i, U'E');
    if (ca == 0xDF) return second(b, j, U'S');
    if (cb == 0xDF) return second(a, i, U'S');
    if (ca == 0x152) return second(b, j, U'E');
    if (cb == 0x152) return second(a, i, U'E');
    return true;
}

}  // namespace client_compare

/// 0x0076ea40 answering 0: the same text but for case - each character's
/// capital (0x0076e8d0) alike, accents kept apart. The macro options compare
/// a pet's or a channelled spell's name with this.
inline bool equalsCapitals(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        char32_t ca = 0, cb = 0;
        client_compare::next(a, i, ca);
        client_compare::next(b, j, cb);
        if (ca != cb) return false;
    }
    return true;
}

/// 0x0076ec80: two strings in order without case or accents, a difference in
/// those alone deciding only when nothing else does. Negative, 0 or positive.
/// The client sorts names this way wherever a list is ordered by name -
/// calendar invites, a trainer's skill lines and services, the map's zones.
inline int compareCaseless(const std::string& a, const std::string& b) {
    int tie = 0;
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        char32_t ca = 0, cb = 0;
        const char32_t ra = client_compare::next(a, i, ca);
        const char32_t rb = client_compare::next(b, j, cb);
        if (!client_compare::same(a, i, ca, b, j, cb))
            return static_cast<int>(client_compare::weight(ca)) - static_cast<int>(client_compare::weight(cb));
        if (tie == 0) tie = static_cast<int>(ra) - static_cast<int>(rb);
    }
    return tie;
}

}  // namespace wowee::game

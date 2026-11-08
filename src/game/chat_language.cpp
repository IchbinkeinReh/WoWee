#include "game/chat_language.hpp"

#include <algorithm>
#include <array>

namespace wowee::game::chat_language {

namespace {

constexpr uint32_t kEnd = 0xffffffffu;
constexpr uint32_t kBad = 0x80000000u;

// The table at 0x00a39538.
constexpr std::array<uint32_t, 16> kHashTable = {
    0x486e26ee, 0xdcaa16b3, 0xe1918eef, 0x202dafdb, 0x341c7dc7, 0x1c365303,
    0x40ef2d37, 0x65fd5e49, 0xd6057177, 0x904ece93, 0x1c38024f, 0x98fd323b,
    0xe3061ae7, 0xa39b0fa1, 0x9797f25f, 0xe4444563};

/// 0x0047bd40: one UTF-8 character at `p` - its code point and byte count;
/// kEnd at the terminator (or one inside a sequence), kBad for a broken one.
uint32_t decode(std::string_view s, size_t p, size_t& len) {
    len = 0;
    if (p >= s.size() || s[p] == '\0') return kEnd;
    const auto b = static_cast<uint8_t>(s[p]);
    len = 1;
    uint32_t cp = b;
    int more;
    if ((b & 0xfe) == 0xfc)      { cp &= 1;    more = 5; }
    else if ((b & 0xfc) == 0xf8) { cp &= 3;    more = 4; }
    else if ((b & 0xf8) == 0xf0) { cp &= 7;    more = 3; }
    else if ((b & 0xf0) == 0xe0) { cp &= 0xf;  more = 2; }
    else if ((b & 0xe0) == 0xc0) { cp &= 0x1f; more = 1; }
    else return (b & 0x80) ? kBad : cp;
    for (int i = 0; i < more; ++i) {
        const size_t q = p + 1 + static_cast<size_t>(i);
        if (q >= s.size() || s[q] == '\0') return kEnd;
        const auto c = static_cast<uint8_t>(s[q]);
        ++len;
        if ((c & 0xc0) != 0x80) return kBad;
        cp = (cp << 6) | (c & 0x3f);
    }
    return cp;
}

/// 0x007e0f30, the letters: ASCII and Latin-1's, less x, the divide sign and
/// thorn's lower case.
bool isLetter(uint32_t c) {
    const uint16_t u = static_cast<uint16_t>(c);
    if ((u >= 0x41 && u <= 0x5a) || (u >= 0x61 && u <= 0x7a)) return true;
    if (static_cast<uint16_t>(u - 0xc0) <= 0x1d && u != 0xd7) return true;
    if (u == 0xdf) return true;
    if (static_cast<uint16_t>(u - 0xe0) <= 0x1f && u != 0xf7 && u != 0xfe) return true;
    return false;
}

/// What a word is made of in 0x00505930: letters, digits (0x007e0f10), the
/// apostrophe and anything past Latin-1.
bool isWordChar(uint32_t c) {
    return isLetter(c) || static_cast<uint16_t>(static_cast<uint16_t>(c) - 0x30) < 10 ||
           c == 0x27 || c >= 0x100;
}

/// 0x007e10f0: anything that is not a lower-case letter.
bool notLower(uint32_t c) {
    const uint16_t u = static_cast<uint16_t>(c);
    return static_cast<uint16_t>(u - 0x61) > 0x19 && static_cast<uint16_t>(u - 0xe0) > 0x1e &&
           static_cast<uint16_t>(u - 0x430) > 0x1f && u != 0x451;
}

/// 0x007e1130 / 0x007e1180 on one byte of a LanguageWords word.
char upper(char ch) {
    const auto u = static_cast<int16_t>(ch);
    if (u >= 0x61 && u <= 0x7a) return static_cast<char>(u - 0x20);
    return ch;
}
char lower(char ch) {
    const auto u = static_cast<int16_t>(ch);
    if (u >= 0x41 && u <= 0x5a) return static_cast<char>(u + 0x20);
    return ch;
}

/// 0x004fb410 outside the Asian locales: the replacement, cut to the
/// original's length in characters, each letter upper-cased where the
/// original's is not lower case and lower-cased where it is.
void appendInCaseOf(std::string& out, const std::string& word, std::string_view original) {
    size_t o = 0;
    for (char ch : word) {
        size_t len = 0;
        const uint32_t cp = decode(original, o, len);
        if (cp == kEnd) break;
        out.push_back(notLower(cp) ? upper(ch) : lower(ch));
        o += len;
    }
}

}  // namespace

uint32_t stringHash(std::string_view s) {
    uint32_t seed = 0x7fed7fedu;
    uint32_t shift = 0xeeeeeeeeu;
    for (char ch : s) {
        uint32_t c = static_cast<uint8_t>(ch);
        if (c == 0) break;
        if (c - 0x61u < 0x1a) c -= 0x20;
        if (c == 0x2f) c = 0x5c;
        seed = (seed + shift) ^ (kHashTable[c >> 4] - kHashTable[c & 0xf]);
        shift = shift * 33 + c + 3 + seed;
    }
    return seed == 0 ? 1u : seed;
}

std::string stripEscapes(std::string_view in) {
    std::string out;
    bool inLink = false;
    size_t i = 0;
    while (i < in.size() && in[i] != '\0') {
        const char c = in[i];
        if (c != '|') {
            if (!(inLink && (c == '[' || c == ']'))) out.push_back(c);
            ++i;
            continue;
        }
        const char k = i + 1 < in.size() ? in[i + 1] : '\0';
        switch (k) {
            case 'c': case 'C': {
                size_t j = i + 2, n = 0;
                while (n < 8 && j < in.size() && in[j] != '\0') { ++j; ++n; }
                if (n < 8) return out;
                i = j;
                break;
            }
            case 'H': {
                size_t j = i + 1;
                while (j < in.size() && in[j] != '\0' &&
                       !(in[j] == '|' && j + 1 < in.size() && in[j + 1] == 'h'))
                    ++j;
                if (j < in.size() && in[j] == '|') j += 2;
                i = j;
                inLink = true;
                break;
            }
            case 'T': {
                size_t j = i + 1;
                while (j < in.size() && in[j] != '\0' &&
                       !(in[j] == '|' && j + 1 < in.size() && in[j + 1] == 't'))
                    ++j;
                if (j >= in.size() || in[j] == '\0') return out;
                i = j + 2;
                break;
            }
            case 'h':
                i += 2;
                inLink = false;
                break;
            case 'r':
                i += 2;
                break;
            case '\0':
                out.push_back('|');
                return out;
            default:
                out.push_back('|');
                out.push_back(k);
                i += 2;
                break;
        }
    }
    return out;
}

std::string garble(std::string_view text, uint32_t comprehension, const WordsOfLength& words) {
    if (comprehension > 299) return std::string(text);
    const std::string s = stripEscapes(text);
    std::string out;
    size_t p = 0;
    for (;;) {
        // The run between words: one space for all of it.
        bool spaced = false;
        size_t len = 0;
        uint32_t cp;
        while ((cp = decode(s, p, len)) != kEnd && !isWordChar(cp)) {
            if (!spaced) { out.push_back(' '); spaced = true; }
            p += len;
        }
        if (cp == kEnd) return out;
        const size_t start = p;
        while ((cp = decode(s, p, len)) != kEnd && isWordChar(cp)) p += len;
        std::string_view word(s.data() + start, std::min<size_t>(p - start, 0x400));
        const uint32_t hash = stringHash(word);
        if (comprehension > hash % 300) {
            out.append(word);
            continue;
        }
        uint32_t want = static_cast<uint32_t>(word.size() > 0x11 ? 0x12 : word.size());
        const std::vector<std::string>* bucket = words ? words(want) : nullptr;
        while (!bucket || bucket->empty()) {
            if (want <= 1) break;
            --want;
            bucket = words ? words(want) : nullptr;
        }
        if (!bucket || bucket->empty()) continue;   // nothing to say it with: dropped
        appendInCaseOf(out, (*bucket)[hash % bucket->size()], word);
    }
}

}  // namespace wowee::game::chat_language

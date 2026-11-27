#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::name_rules {

/// The client's name check (0x007e18c0), as the arena charter purchase calls
/// it through 0x007e1f60: the result is the reason code that 0x005218c0
/// words as an ERR_NAME_* line, 0 for a name that passes.
///
/// Every letter must belong to the alphabet of the first one - Latin with the
/// Latin-1 letters (0x007e0f30), plain ASCII (0x007e0f90), Cyrillic
/// (0x007e1050), Hangul (0x007e0fc0) or CJK (0x007e0ff0) - and anything else
/// must be one of `allowed`. Unless `relaxed`, a name may not start or end
/// with an apostrophe or a space, hold two apostrophes or two spaces in a
/// row, three of one letter in a row, or misplace the Cyrillic hard and soft
/// signs. The reserved and profane name lists (NamesReserved.dbc,
/// NamesProfanity.dbc; reason 8) are not applied here.
enum : uint32_t {
    kOk = 0, kInvalid = 1, kNoName = 2, kTooShort = 3, kTooLong = 4, kInvalidCharacter = 5,
    kApostropheEnd = 9, kTwoApostrophes = 10, kThreeConsecutive = 11, kInvalidSpace = 12,
    kConsecutiveSpaces = 13, kRussianConsecutiveSilent = 14, kRussianSilentAtEnds = 15,
};

namespace detail {

/// UTF-8 to UTF-16 code units; false on a malformed sequence (0x00775eb0).
inline bool toUtf16(const std::string& in, std::vector<uint16_t>& out) {
    for (size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        uint32_t cp;
        size_t n;
        if (c < 0x80)              { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > in.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const auto cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
        i += n;
    }
    return true;
}

inline bool inScript(uint16_t c, int script) {
    switch (script) {
        case 0:  // 0x007e0f30
            return (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) ||
                   (c >= 0xC0 && c <= 0xDD && c != 0xD7) || c == 0xDF ||
                   (c >= 0xE0 && c <= 0xFF && c != 0xF7 && c != 0xFE);
        case 1:  // 0x007e0f90
            return (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A);
        case 2:  // 0x007e1050
            return (c >= 0x410 && c <= 0x44F) || c == 0x401 || c == 0x451;
        case 3:  // 0x007e0fc0 keeps a table of the Hangul syllables it takes
            return c >= 0xAC00 && c <= 0xD7A3;
        case 4:  // 0x007e0ff0
            return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DFF) ||
                   (c >= 0xF900 && c <= 0xFAFF);
        default:
            return false;
    }
}

/// The case fold the repeated-letter and Cyrillic tests use.
inline uint16_t fold(uint16_t c) {
    if ((c >= 0x41 && c <= 0x5A) || (c >= 0xC0 && c <= 0xDE)) return static_cast<uint16_t>(c + 0x20);
    if (c == 0x152) return 0x153;
    if (c >= 0x410 && c <= 0x42F) return static_cast<uint16_t>(c + 0x20);
    if (c == 0x401) return 0x451;
    return c;
}

}  // namespace detail

/// The reason code, and in `length` how many characters the name has.
inline uint32_t check(const std::string& name, const std::u16string& allowed, bool relaxed,
                      size_t* length = nullptr) {
    if (length) *length = 0;
    // An empty text is not a missing one (only a null pointer answers 2 at
    // once); it ends the walk at once, and only the strict rules' length
    // test (len < 2 -> 2 or 3) refuses it.
    if (name.empty()) return relaxed ? kOk : kNoName;
    std::vector<uint16_t> s;
    if (!detail::toUtf16(name, s)) return kInvalid;
    int script = -1;
    long apostrophe = -1, space = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint16_t c = s[i];
        const size_t count = i + 1;
        if (length) *length = count;
        bool letter = false;
        if (script == -1) {
            for (int k = 0; k < 5; ++k) {
                if (detail::inScript(c, k)) { script = k; letter = true; break; }
            }
        } else {
            letter = detail::inScript(c, script);
        }
        if (!letter) {
            if (allowed.find(static_cast<char16_t>(c)) == std::u16string::npos) return kInvalidCharacter;
            if (relaxed) continue;
            if (c == u'\'') {
                if (count == 1) return kApostropheEnd;
                if (apostrophe >= 0) return kTwoApostrophes;
                apostrophe = static_cast<long>(count) - 1;
            } else if (c == u' ') {
                if (count == 1) return kInvalidSpace;
                if (count > 1 && space == static_cast<long>(count) - 2) return kConsecutiveSpaces;
                space = static_cast<long>(count) - 1;
            }
            continue;
        }
        if (!relaxed && count > 2 && detail::fold(s[i - 2]) == detail::fold(s[i - 1]) &&
            detail::fold(s[i - 1]) == detail::fold(c)) {
            return kThreeConsecutive;
        }
        if (script == 2) {
            constexpr uint16_t kHard = 0x44A, kSoft = 0x44C;
            const uint16_t cur = detail::fold(c);
            const uint16_t next = i + 1 < s.size() ? s[i + 1] : 0;
            if ((count == 1 && (cur == kHard || cur == kSoft)) || ((next == 0 || next == u' ') && cur == kHard))
                return kRussianSilentAtEnds;
            if (count > 1 && (cur == kHard || cur == kSoft)) {
                const uint16_t prev = detail::fold(s[i - 1]);
                if (prev == kHard || prev == kSoft) return kRussianConsecutiveSilent;
            }
        }
    }
    if (!relaxed) {
        const long len = static_cast<long>(s.size());
        if (len < 2) return len != 0 ? kTooShort : kNoName;
        if (apostrophe == len - 1) return kApostropheEnd;
        if (space == len - 1) return kInvalidSpace;
    }
    return kOk;
}

/// An arena team's name as BuyPetition checks it (0x007e1f60): spaces the
/// only punctuation, and no more than 24 characters.
inline uint32_t checkCharterName(const std::string& name) {
    size_t length = 0;
    const uint32_t r = check(name, u" ", false, &length);
    if (r == kOk && length > 24) return kTooLong;
    return r;
}

/// A guild bank tab's name as SetGuildBankTabInfo checks it (0x007e1fb0):
/// spaces and digits besides the letters, none of the apostrophe, space and
/// repeated-letter rules (its ninth argument set) - so an empty name passes -
/// and no more than 16 characters.
inline uint32_t checkGuildBankTabName(const std::string& name) {
    size_t length = 0;
    const uint32_t r = check(name, u" 1234567890", true, &length);
    if (r == kOk && length > 16) return kTooLong;
    return r;
}

}  // namespace wowee::game::name_rules

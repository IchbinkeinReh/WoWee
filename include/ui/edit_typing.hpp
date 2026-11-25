#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace wowee::ui {

/// The keys an edit box knows, as SDL names them.
namespace edit_key {
constexpr int kBackspace = 0x08, kTab = 0x09, kReturn = 0x0D, kEscape = 0x1B, kDelete = 0x7F;
constexpr int kInsert = 0x40000049, kHome = 0x4000004A, kEnd = 0x4000004D;
constexpr int kRight = 0x4000004F, kLeft = 0x40000050, kDown = 0x40000051, kUp = 0x40000052;
}  // namespace edit_key

/// What is typed or pasted as an edit box takes it (0x00965ce0, 0x00965ed0): a
/// character at a time, a tab as four blanks, a bar doubled - the player types what
/// is seen and cannot type an escape - and no control character but a line break in
/// a multi-line box.
[[nodiscard]] inline std::vector<std::string> typedPieces(const std::string& utf8, bool multiLine) {
    std::vector<std::string> pieces;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        const size_t n = std::min<size_t>(c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, utf8.size() - i);
        const std::string one = utf8.substr(i, n);
        i += n;
        if (c == 0x09) pieces.emplace_back("    ");
        else if (c == '|') pieces.emplace_back("||");
        else if ((c >= 0x20 && c != 0x7f) || (c == 0x0a && multiLine)) pieces.push_back(one);
    }
    return pieces;
}

/// What is copied or cut as the clipboard has it (0x009632d0 -> 0x006bdc50 with 0x1500): what
/// is seen - no colour codes, no link's or picture's, a doubled bar as one, a line break
/// of either kind as one. An escape that is none is as it stands (0x006bd5a0).
[[nodiscard]] inline std::string copiedText(const std::string& marked) {
    const size_t size = marked.size();
    const auto at = [&](size_t i) { return i < size ? marked[i] : '\0'; };
    const auto hex = [&](size_t i) { return std::isxdigit(static_cast<unsigned char>(at(i))) != 0; };
    // How long the escape at `i` is, 0 for none.
    const auto escape = [&](size_t i) -> size_t {
        switch (at(i + 1)) {
            case 'c':
            case 'C':
                for (size_t k = 2; k < 10; ++k)
                    if (!hex(i + k)) return 0;
                return 10;
            case 'r':
            case 'R':
            case 'h':
            case 't': return 2;
            case 'H': {
                const size_t bar = marked.find('|', i + 2);
                // With something for it to be, and something behind it.
                if (bar == std::string::npos || bar == i + 2 || at(bar + 1) != 'h' || at(bar + 2) == '\0') return 0;
                if (at(bar + 2) == '|' && at(bar + 3) == 'h') return 0;
                return bar + 2 - i;
            }
            case 'T': {
                const size_t bar = marked.find('|', i + 2);
                if (bar == std::string::npos || bar == i + 2 || at(bar + 1) != 't') return 0;
                return bar + 2 - i;
            }
            default: return 0;
        }
    };
    std::string out;
    for (size_t i = 0; i < size;) {
        const char c = marked[i];
        if (c == '\r') {
            out += '\n';
            i += at(i + 1) == '\n' ? 2 : 1;
        } else if (c != '|') {
            out += c;
            ++i;
        } else if (at(i + 1) == '|') {
            out += '|';
            i += 2;
        } else if (at(i + 1) == 'n' || at(i + 1) == 'N') {
            out += '\n';
            i += 2;
        } else if (const size_t n = escape(i); n != 0) {
            i += n;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

/// Whether a numeric box refuses a piece (0x00965960): whole, if any of it is no
/// digit.
[[nodiscard]] inline bool numericRefuses(const std::string& piece) {
    return std::any_of(piece.begin(), piece.end(), [](char c) { return c < '0' || c > '9'; });
}

}  // namespace wowee::ui

#pragma once

#include <algorithm>
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

/// Whether a numeric box refuses a piece (0x00965960): whole, if any of it is no
/// digit.
[[nodiscard]] inline bool numericRefuses(const std::string& piece) {
    return std::any_of(piece.begin(), piece.end(), [](char c) { return c < '0' || c > '9'; });
}

}  // namespace wowee::ui

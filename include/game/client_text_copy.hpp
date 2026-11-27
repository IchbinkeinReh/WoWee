#pragma once

#include <cstddef>
#include <string>

namespace wowee::game {

/// A text taken into one of the client's fixed buffers as 0x007e1830 takes
/// it: 0x007e17f0 copies at most `bufferBytes - 1` bytes leaving every '|'
/// behind (the hyperlink and colour marker), with `oneLine` (flag bit 0) the
/// copy ends at the first line break - "\r", "\n", or an escaped "\\n" -
/// (0x007e1640), and it is cut where its `maxChars`th UTF-8 character starts.
/// The guild bank keeps its tab names this way (16 characters, 64 bytes, one
/// line: the list 0x005a7250, the rename 0x005a4220, SetGuildBankTabInfo
/// 0x005a5a00) and its tab texts (2048, 8192: the text 0x005a4ae0,
/// SetGuildBankText 0x005a7180).
inline std::string clientTextCopy(const std::string& text, size_t maxChars, size_t bufferBytes,
                                  bool oneLine) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (bufferBytes == 0 || out.size() + 1 >= bufferBytes) break;
        if (c != '|') out.push_back(c);
    }
    if (oneLine) {
        for (size_t i = 0; i < out.size(); ++i) {
            const char c = out[i];
            const bool brk = c == '\r' || c == '\n' ||
                             (c == '\\' && i + 1 < out.size() && out[i + 1] == 'n');
            if (brk) { out.resize(i); break; }
        }
    }
    size_t chars = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if ((static_cast<unsigned char>(out[i]) & 0xC0u) == 0x80u) continue;
        if (++chars == maxChars) { out.resize(i); break; }
    }
    return out;
}

}  // namespace wowee::game

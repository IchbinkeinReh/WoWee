#pragma once

// What SendChatMessage (0x0050d170) does to a line before it is sent: the
// %t and %f tokens become names (0x00577d00), and a drunk speaker's line is
// slurred (0x00577f60). Both write into a 256-byte buffer, so a line comes
// out of each no longer than 255 bytes.

#include <cstddef>
#include <functional>
#include <string>

namespace wowee::game::chat_outgoing {

/// The buffer both steps write into (0x100), less its terminator.
inline constexpr size_t kMaxLine = 255;

/// strlcat into a 256-byte buffer (0x0076ef70): append what still fits.
inline void appendBounded(std::string& out, const std::string& text) {
    if (out.size() >= kMaxLine) return;
    out.append(text, 0, kMaxLine - out.size());
}

/// 0x00577d00: %t, %T, %n and %N become `target` and %f and %F become
/// `focus` - each the name, or the not-found text where there is none. Any
/// other % is kept as it is, and the character after it read as text.
inline std::string replaceTokens(const std::string& text, const std::string& target, const std::string& focus) {
    std::string out;
    size_t from = 0;
    for (;;) {
        const size_t pct = text.find('%', from);
        if (pct == std::string::npos) {
            appendBounded(out, text.substr(from));
            return out;
        }
        appendBounded(out, text.substr(from, pct - from));
        const char c = pct + 1 < text.size() ? text[pct + 1] : '\0';
        switch (c) {
            case 'f': case 'F':
                appendBounded(out, focus);
                from = pct + 2;
                break;
            case 't': case 'T': case 'n': case 'N':
                appendBounded(out, target);
                from = pct + 2;
                break;
            default:
                appendBounded(out, "%");
                from = pct + 1;
                break;
        }
    }
}

/// 0x00577f60: a drunk speaker's line. `factor` is how drunk (0..1,
/// 0x004f7290); `chance()` draws a number in [0, 1) each time the client
/// rolls; `profane` asks the mature-language filter about a word (0x00509a50
/// with its forced flag); `hicFormat` is SLURRED_SPEECH, applied to the line
/// with its one %s.
///
///  * an s or S that starts a word, where the next character is not a space
///    or another s, gains an h with probability `factor` - unless the word
///    spelled with "sh" in front of its remaining letters is profane;
///  * an s or S that ends a word before a space gains an h with the same
///    probability;
///  * a |c...|r run is copied untouched;
///  * then, with probability factor / 4, the whole line goes into
///    SLURRED_SPEECH.
inline std::string slur(const std::string& text, float factor, const std::function<float()>& chance,
                        const std::function<bool(const std::string&)>& profane, const std::string& hicFormat) {
    std::string out;
    size_t room = kMaxLine + 1;   // the buffer, terminator included
    bool wordStart = true;
    size_t i = 0;
    while (i < text.size() && room >= 2) {
        const char c = text[i];
        out.push_back(c);
        --room;
        ++i;
        const char next = i < text.size() ? text[i] : '\0';
        if (c == ' ') {
            wordStart = true;
            continue;
        }
        if (c == 'S' || c == 's') {
            if (wordStart) {
                if (room > 1 && next != ' ' && next != 'S' && next != 's' && chance() < factor) {
                    std::string word = "sh";
                    for (size_t k = i; k < text.size() && text[k] != ' '; ++k) word.push_back(text[k]);
                    if (!profane || !profane(word)) {
                        out.push_back('h');
                        --room;
                    }
                }
            } else if (room > 1 && next == ' ' && chance() < factor) {
                out.push_back('h');
                --room;
            }
        } else if (c == '|' && next == 'c') {
            const size_t end = text.find("|r", i);
            if (end != std::string::npos) {
                size_t run = end - i + 2;
                if (room <= run) run = room - 1;
                out.append(text, i, run);
                room -= run;
                i = end + 2;
            }
        }
        wordStart = false;
    }
    if (chance() < factor * 0.25f) {
        std::string formatted;
        const size_t at = hicFormat.find("%s");
        if (at == std::string::npos) formatted = hicFormat;
        else formatted = hicFormat.substr(0, at) + out + hicFormat.substr(at + 2);
        if (formatted.size() > kMaxLine) formatted.resize(kMaxLine);
        return formatted;
    }
    return out;
}

}  // namespace wowee::game::chat_outgoing

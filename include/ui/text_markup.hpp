#pragma once

// Parsing WoW's inline text markup out of a label.
//
// The interface writes colour into the string itself - "|cffffd200(M)|r" is
// the world map's keybinding hint in gold - and draws it with the same call as
// any other label. Drawn without parsing, the escape is what appears on
// screen, which is what the map button's tooltip was showing.
//
// Also handled: |H...|h link markers, which wrap the display text of an item
// or spell link, and |T...|t inline textures, which name a file to draw in
// the middle of a line. Those used to be dropped, so a price came out as
// bare numbers with no coin beside them. "||" is a literal bar.
// The wrap works on these too, so there is one definition rather than two
// that have to agree - see ui/text_wrap.hpp.
//
// Header-only and free of ImGui so it can be tested: the drawing side only
// consumes the runs this hands back. It lived inside widget_renderer.cpp and
// could not be, which is how a link's display text came to be dropped for
// months without anything noticing.

#include "ui/text_wrap.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace ui {

inline std::vector<WrapRun> parseMarkup(const std::string& in) {
    std::vector<WrapRun> runs;
    WrapRun cur;
    auto flush = [&] {
        if (!cur.text.empty()) runs.push_back(cur);
        cur.text.clear();
    };
    auto hexPair = [](const std::string& s, size_t at) {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = v(s[at]), lo = v(s[at + 1]);
        return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
    };

    size_t colorAt = std::string::npos;   // the colour that holds
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '|') { cur.text += in[i++]; continue; }
        if (i + 1 >= in.size()) { cur.text += in[i++]; continue; }
        const char tag = in[i + 1];
        if (tag == '|') { cur.text += '|'; i += 2; continue; }
        if ((tag == 'c' || tag == 'C') && i + 9 < in.size()) {
            // |cAARRGGBB - alpha first, and the interface almost always sends
            // ff for it. Ignored rather than applied: a label's own alpha
            // already governs it, and multiplying the two fades text that the
            // real client draws solid.
            const int r = hexPair(in, i + 4), g = hexPair(in, i + 6),
                      b = hexPair(in, i + 8);
            if (r >= 0 && g >= 0 && b >= 0) {
                flush();
                colorAt = i;
                cur.hasColor = true;
                cur.rgba[0] = r / 255.0f; cur.rgba[1] = g / 255.0f;
                cur.rgba[2] = b / 255.0f; cur.rgba[3] = 1.0f;
                i += 10;
                continue;
            }
        }
        if (tag == 'r' || tag == 'R') {
            flush();
            colorAt = std::string::npos;
            cur.hasColor = false;
            i += 2;
            continue;
        }
        // |n is WoW's line break, and it was not handled at all - so every one
        // of them drew as a literal bar and an n. globalstrings.lua alone has a
        // hundred and thirty-eight.
        if (tag == 'n' || tag == 'N') { cur.text += '\n'; i += 2; continue; }
        // |4singular:plural; - the word chosen by the number written before it.
        // SecondsToTime and every duration in the interface is built on it
        // ("%d |4Sec:Sec;"), and it drew raw: "Time in queue: 13 |4Sec:Sec;".
        if (tag == '4') {
            const size_t semi = in.find(';', i + 2);
            if (semi != std::string::npos) {
                const std::string body = in.substr(i + 2, semi - (i + 2));
                const size_t colon = body.find(':');
                // The number is the last one before the escape, spaces aside.
                size_t j = i;
                while (j > 0 && in[j - 1] == ' ') --j;
                size_t digitsEnd = j;
                while (j > 0 && ((in[j - 1] >= '0' && in[j - 1] <= '9') || in[j - 1] == '.')) --j;
                const bool one = digitsEnd > j && in.substr(j, digitsEnd - j) == "1";
                if (colon == std::string::npos) cur.text += body;
                else cur.text += one ? body.substr(0, colon) : body.substr(colon + 1);
                i = semi + 1;
                continue;
            }
        }
        if (tag == 'H') {
            // |Hitem:3299|h[Fractured Canine]|h - the payload runs to the
            // first |h, the display text follows it, and a second |h closes.
            //
            // This skipped to the next bar, which is the |h *before* the
            // display text, and then the 'h' branch skipped again to the |h
            // after it - so the name between them was thrown away with the
            // payload. "You receive loot: [Fractured Canine]." drew as "You
            // receive loot: ." on every FrameXML surface that renders a link.
            const size_t close = in.find("|h", i + 2);
            if (close == std::string::npos) { i = in.size(); continue; }
            flush();
            cur.link = in.substr(i + 2, close - (i + 2));
            cur.linkText.clear();
            if (const size_t end = in.find("|h", close + 2); end != std::string::npos) {
                size_t from = i, stop = end + 2;
                if (colorAt != std::string::npos && in.compare(stop, 2, "|r") == 0) {
                    from = std::exchange(colorAt, std::string::npos);
                    stop += 2;
                }
                cur.linkText = in.substr(from, stop - from);
            }
            i = close + 2;
            continue;
        }
        if (tag == 'h') {          // the closing marker; the link ends here
            flush();
            cur.link.clear();
            cur.linkText.clear();
            i += 2;
            continue;
        }
        if (tag == 'T') {
            // |Tpath:height:width:offsetX:offsetY:texWidth:texHeight:left:
            // right:top:bottom|t. Everything after the path is optional and a
            // zero size means "the height of the line", which is what all
            // three coin escapes ask for; the last six cut a part of the file
            // out, in its pixels.
            const size_t end = in.find("|t", i + 2);
            const size_t stop = (end == std::string::npos) ? in.size() : end;
            const std::string payload = in.substr(i + 2, stop - (i + 2));
            i = (end == std::string::npos) ? in.size() : end + 2;

            const size_t firstColon = payload.find(':');
            std::string path = payload.substr(0, firstColon);
            if (!path.empty()) {
                flush();
                WrapRun tex = cur;          // keeps the colour and link context
                tex.text.clear();
                tex.texture = std::move(path);
                if (firstColon != std::string::npos) {
                    // Height then width. Anything unparsable stays zero, which
                    // is the same as the escape asking for the line height.
                    size_t at = firstColon + 1;
                    float fileW = 0.0f, fileH = 0.0f;
                    float cut[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
                    float* fields[10] = {&tex.texHeight, &tex.texWidth, &tex.texX, &tex.texY, &fileW, &fileH,
                                         &cut[0], &cut[1], &cut[2], &cut[3]};
                    for (float* field : fields) {
                        if (at > payload.size()) break;
                        const size_t next = payload.find(':', at);
                        const std::string part = payload.substr(
                            at, next == std::string::npos ? std::string::npos : next - at);
                        *field = static_cast<float>(std::atof(part.c_str()));
                        if (next == std::string::npos) break;
                        at = next + 1;
                    }
                    tex.texWidthGiven = payload.find(':', firstColon + 1) != std::string::npos;
                    if (fileW > 0.0f && fileH > 0.0f && cut[3] >= 0.0f) {
                        tex.uv[0] = cut[0] / fileW;
                        tex.uv[1] = cut[1] / fileW;
                        tex.uv[2] = cut[2] / fileH;
                        tex.uv[3] = cut[3] / fileH;
                    }
                }
                runs.push_back(std::move(tex));
            }
            continue;
        }
        if (tag == 't') { i += 2; continue; }   // a closing marker with no opener
        cur.text += in[i++];   // a bar that means nothing in particular
    }
    flush();
    return runs;
}

/// The screen texts are drawn on, which the pictures in them go by.
struct TextScreen {
    float aspect = 4.0f / 3.0f;
    float uiScale = 1.0f;   ///< UIParent's, which the tree holds apart from a frame's
};
inline TextScreen& textScreen() {
    static TextScreen screen;
    return screen;
}

/// What the numbers of a picture come to in a text whose letters are `size` high, in the
/// same units, on a frame of that scale (0x006c0e80): they are for letters 14 high, get
/// the scale of the frame once more than the letters do, and the client's own aspect
/// factor (0x00ac0cb4) on top. A number of none is the height of a line.
struct PictureUnits {
    float number = 1.0f, line = 0.0f, scale = 1.0f;
};
[[nodiscard]] inline PictureUnits pictureUnits(float size, float frameScale) {
    const TextScreen& screen = textScreen();
    const float scale = (frameScale > 0.0f ? frameScale : 1.0f) * screen.uiScale;
    return {size / 14.0f * scale * std::sqrt(screen.aspect * screen.aspect + 1.0f) / screen.aspect, size * scale, scale};
}
/// A picture's numbers turned into sizes: with one it is as wide as high, and without a
/// height the width is so many times that of the line.
inline void sizePicture(WrapRun& run, const PictureUnits& units) {
    const bool high = run.texHeight > 0.0f;
    const float height = high ? run.texHeight * units.number : units.line;
    if (!run.texWidthGiven) run.texWidth = height;
    else if (run.texWidth <= 0.0f) run.texWidth = units.line;
    else run.texWidth *= high ? units.number : units.scale * height;
    run.texHeight = height;
    run.texX *= units.scale;
    run.texY *= units.scale;
}
[[nodiscard]] inline std::vector<WrapRun> parseMarkup(const std::string& in, const PictureUnits& units) {
    auto runs = parseMarkup(in);
    for (WrapRun& run : runs)
        if (!run.texture.empty()) sizePicture(run, units);
    return runs;
}

/// How wide a text is drawn (0x00483e80 -> 0x004bdcd0): the widest of its lines, as it
/// breaks them itself, the pictures among them; nothing for what draws nothing.
template <class Measure>
[[nodiscard]] float markupWidth(const std::string& text, const PictureUnits& units, Measure&& measure) {
    float widest = 0.0f;
    for (const auto& line : wrapText(parseMarkup(text, units), 0.0f, false, measure)) {
        float wide = 0.0f;
        for (const WrapRun& run : line) wide += run.texture.empty() ? measure(run.text) : run.texWidth;
        widest = std::max(widest, wide);
    }
    return widest;
}

/// What an edit box's text is of (0x006bd5a0), one after the other.
struct EditToken {
    enum Type { Plain = 0, Color = 1, ColorEnd = 2, Break = 3, Bar = 4, Link = 5, LinkEnd = 6, Picture = 7, PictureEnd = 8 };
    size_t at = 0, size = 0;
    Type type = Plain;
    bool linked = false;  ///< of a link, its begin and end too (0x00962cc0)
};

/// A character's bytes at `at` (0x0047bd40).
inline size_t utf8Size(const std::string& s, size_t at) {
    const auto lead = static_cast<unsigned char>(s[at]);
    const size_t more = (lead & 0xE0) == 0xC0   ? 1
                        : (lead & 0xF0) == 0xE0 ? 2
                        : (lead & 0xF8) == 0xF0 ? 3
                        : (lead & 0xFC) == 0xF8 ? 4
                        : (lead & 0xFE) == 0xFC ? 5
                                                : 0;
    size_t n = 1;
    while (n <= more && at + n < s.size() && (static_cast<unsigned char>(s[at + n]) & 0xC0) == 0x80) ++n;
    return n;
}

inline EditToken editTokenAt(const std::string& s, size_t at) {
    const auto token = [at](EditToken::Type type, size_t size) { return EditToken{at, size, type, false}; };
    const auto is = [&s](size_t i, char c) { return i < s.size() && s[i] == c; };
    if (s[at] == '\r') return token(EditToken::Break, is(at + 1, '\n') ? 2 : 1);
    if (s[at] == '\n') return token(EditToken::Break, 1);
    if (s[at] != '|' || at + 1 >= s.size()) return token(EditToken::Plain, utf8Size(s, at));
    switch (s[at + 1]) {
        case 'c':
        case 'C':
            if (at + 10 > s.size()) break;
            for (size_t i = at + 2; i < at + 10; ++i)
                if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return token(EditToken::Plain, 1);
            return token(EditToken::Color, 10);
        case 'H': {
            // A target, and words that are not empty.
            const size_t bar = s.find('|', at + 2);
            if (bar == std::string::npos || bar == at + 2 || !is(bar + 1, 'h') || bar + 2 >= s.size() ||
                (is(bar + 2, '|') && is(bar + 3, 'h')))
                break;
            return token(EditToken::Link, bar + 2 - at);
        }
        case 'n':
        case 'N': return token(EditToken::Break, 2);
        case 'r':
        case 'R': return token(EditToken::ColorEnd, 2);
        case 'T': {
            const size_t bar = s.find('|', at + 2);
            if (bar == std::string::npos || bar == at + 2 || !is(bar + 1, 't')) break;
            return token(EditToken::Picture, bar + 2 - at);
        }
        case 'h': return token(EditToken::LinkEnd, 2);
        case 't': return token(EditToken::PictureEnd, 2);
        case '|': return token(EditToken::Bar, 2);
        default: break;
    }
    return token(EditToken::Plain, 1);
}

inline std::vector<EditToken> editTokens(const std::string& s) {
    std::vector<EditToken> out;
    bool linked = false;
    for (size_t at = 0; at < s.size(); at += out.back().size) {
        out.push_back(editTokenAt(s, at));
        if (out.back().type == EditToken::Link) linked = true;
        out.back().linked = linked;
        if (out.back().type == EditToken::LinkEnd) linked = false;
    }
    return out;
}

/// The next caret position after `at`, treating an escape as one step (0x00962d50).
///
/// A caret walks what is drawn, not what is held. "|Hitem:3299|h[Fractured
/// Canine]|h" is fifty-odd bytes and eighteen characters on screen, so
/// stepping one byte at a time leaves the caret apparently frozen for forty
/// keypresses while it crawls through the payload - and then jumping a whole
/// word when it reaches the display text.
///
/// A colour and a picture are passed with the character after them, the ends of
/// colours, links and pictures with the one before. A link is one step whole - the
/// caret cannot rest inside it - and a backspace therefore removes the link, its
/// colour with it, rather than a byte of its payload.
inline size_t caretStepRight(const std::string& s, size_t at) {
    const auto tokens = editTokens(s);
    size_t i = 0;
    while (i < tokens.size() && tokens[i].at < at) ++i;
    for (; i < tokens.size(); ++i) {
        const EditToken& t = tokens[i];
        if (t.type == EditToken::Color || t.type == EditToken::Link || t.type == EditToken::Picture) continue;
        if (!t.linked || t.type == EditToken::LinkEnd) break;
    }
    if (i < tokens.size()) ++i;
    while (i < tokens.size() && (tokens[i].type == EditToken::ColorEnd || tokens[i].type == EditToken::LinkEnd ||
                                 tokens[i].type == EditToken::PictureEnd))
        ++i;
    return i < tokens.size() ? tokens[i].at : s.size();
}

/// The previous caret position, by the same rule. Walked forward from the
/// start rather than backward: the escapes are only unambiguous read in the
/// direction they were written, and a box holds a line of chat, not a book.
inline size_t caretStepLeft(const std::string& s, size_t at) {
    const auto tokens = editTokens(s);
    size_t i = 0;
    while (i < tokens.size() && tokens[i].at < at) ++i;
    const auto trailing = [](const EditToken& t) {
        return t.type == EditToken::ColorEnd || t.type == EditToken::LinkEnd || t.type == EditToken::PictureEnd ||
               (t.linked && t.type != EditToken::Link);
    };
    const auto leading = [](const EditToken& t) {
        return t.type == EditToken::Color || t.type == EditToken::Link || t.type == EditToken::Picture;
    };
    while (i > 0 && trailing(tokens[i - 1])) --i;
    if (i > 0) --i;
    while (i > 0 && leading(tokens[i - 1])) --i;
    return i < tokens.size() ? tokens[i].at : 0;
}

/// The nearest position at or before `at` that the caret can actually occupy.
///
/// Everything that moves the caret by stepping keeps it on a boundary already.
/// This is for the one that does not: SetCursorPosition takes a number from
/// Lua and, unguarded, will put the caret inside a link - after which a
/// backspace splits the escape, which is the whole thing the stepping was
/// arranged to prevent. FrameXML only ever asks for 0 or the end, so today
/// this changes nothing; an invariant that holds because no caller happens to
/// break it is not one.
inline size_t caretSnap(const std::string& s, size_t at) {
    if (at >= s.size()) return s.size();
    size_t p = 0;
    while (p < at) {
        const size_t next = caretStepRight(s, p);
        if (next <= p) break;          // no progress: stop rather than spin
        if (next > at) return p;       // `at` fell inside this step
        p = next;
    }
    return p;
}

/// How many characters a string shows (0x00962ea0), ignoring the markup that draws
/// none: those of a link and breaks of the line among them, of UTF-8 each once.
///
/// An edit box's letter limit counts these, unless the box asked for
/// countInvisibleLetters (0x0076eea0: every character, the markup's too). The chat
/// box does not ask: it declares letters="255" and nothing else, so the colour codes
/// and hyperlink escapes that a shift-clicked item link brings with it must not be
/// charged against the limit.
inline size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}
inline size_t visibleLength(const std::string& s) {
    size_t n = 0;
    for (const EditToken& t : editTokens(s))
        if (t.type == EditToken::Plain || t.type == EditToken::Break || t.type == EditToken::Bar) ++n;
    return n;
}

/// An edit box's text after `add` at `cursor`, within its limits (0x00965960): what
/// is over them - the bytes, then the letters; none for a limit under 0 and 1 - goes
/// off the end of the text, whatever was put in and where. Returns the cursor.
inline size_t insertIntoEditBox(std::string& text, size_t cursor, const std::string& add, int maxBytes, int maxLetters,
                                bool countInvisible) {
    cursor = std::min(cursor, text.size());
    text.insert(cursor, add);
    cursor += add.size();
    const auto trimEnd = [&text](auto over) {
        while (!text.empty() && over()) text.resize(caretStepLeft(text, text.size()));
    };
    if (maxBytes >= 0) trimEnd([&] { return text.size() > static_cast<size_t>(maxBytes); });
    if (maxLetters > 0) {
        trimEnd([&] {
            return (countInvisible ? codepoints(text) : visibleLength(text)) > static_cast<size_t>(maxLetters);
        });
    }
    return std::min(cursor, text.size());
}

/// What an edit box holds after a selected run is replaced.
///
/// The whole of it, because the pieces are where the mistakes are: a selection
/// is a byte range and a caret is a byte offset, and getting either end wrong
/// leaves a character behind or eats one that was not selected.
///
/// Returns the new caret. The text is edited in place. A range that is empty,
/// inverted, or reaching past the end is treated as no selection at all -
/// FrameXML clears a highlight by passing equal offsets, and a zero-width run
/// that still counted would swallow the next character typed.
struct EditSelection {
    bool   active = false;
    size_t start = 0;
    size_t stop = 0;
};

inline size_t replaceSelection(std::string& text, size_t cursor,
                               const EditSelection& sel) {
    if (!sel.active) return cursor;
    if (sel.stop <= sel.start) return cursor;
    if (sel.stop > text.size()) return cursor;
    text.erase(sel.start, sel.stop - sel.start);
    return sel.start;
}

/// The caret a word to the left, as Ctrl+Left moves it: back over the spaces,
/// then over the word to its first character. A link is one step, as
/// everywhere the caret goes.
inline size_t caretWordLeft(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    while (p > 0) {
        const size_t q = caretStepLeft(s, p);
        if (s[q] != ' ') break;
        p = q;
    }
    while (p > 0) {
        const size_t q = caretStepLeft(s, p);
        if (s[q] == ' ') break;
        p = q;
    }
    return p;
}

/// The caret a word to the right, as Ctrl+Right moves it: over the rest of
/// the word and the spaces after it, to the start of the next.
inline size_t caretWordRight(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    while (p < s.size() && s[p] != ' ') p = caretStepRight(s, p);
    while (p < s.size() && s[p] == ' ') p = caretStepRight(s, p);
    return p;
}

/// Where the caret and the selection stand after the caret goes to `target`.
/// With `extend` (Shift held) the selection runs from its anchor - the end
/// the caret is not at, or the caret itself when nothing is selected - to the
/// target; without it the selection is let go. Kept in order, start before
/// stop, as replaceSelection takes it.
struct EditCaret {
    size_t caret = 0;
    EditSelection sel;
};
inline EditCaret moveCaret(size_t caret, const EditSelection& sel, size_t target, bool extend) {
    EditCaret out;
    out.caret = target;
    if (!extend) return out;
    const bool selected = sel.active && sel.stop > sel.start;
    const size_t anchor = selected ? (caret == sel.start ? sel.stop : sel.start) : caret;
    if (anchor != target) {
        out.sel.active = true;
        out.sel.start = (anchor < target) ? anchor : target;
        out.sel.stop = (anchor < target) ? target : anchor;
    }
    return out;
}

/// The start of the line the caret is on: back until the character before it
/// is a line break (0x00964200, plain Home).
inline size_t caretLineStart(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    while (p > 0 && s[p - 1] != '\n') {
        const size_t q = caretStepLeft(s, p);
        if (q >= p) break;
        p = q;
    }
    return p;
}

/// The end of the caret's line: forward until the character at it is a line
/// break or the text ends (0x009642e0, plain End).
inline size_t caretLineEnd(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    while (p < s.size() && s[p] != '\n') {
        const size_t q = caretStepRight(s, p);
        if (q <= p) break;
        p = q;
    }
    return p;
}

/// The four arrow keys (0x204 to 0x207), as SDL numbers them.
inline bool isArrowKey(int sdlKeycode) {
    return sdlKeycode >= 0x4000004F && sdlKeycode <= 0x40000052;
}

/// Whether an edit box leaves the arrows to the game (0x00967760): one with
/// ignoreArrows does while Alt is not held.
inline bool leavesArrows(bool ignoreArrows, bool altHeld) {
    return ignoreArrows && !altHeld;
}

/// Whitespace as the word deletes see it (iswspace): the ASCII kind; a byte
/// of a multi-byte character never is.
inline bool editIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/// Where Ctrl+Backspace / Ctrl+W deletes back to (0x00963b60): over the
/// whitespace before the caret, then over the word before that.
inline size_t wordDeleteBackFrom(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    const auto tokens = editTokens(s);
    // The last of what is before the caret, whatever the step over it takes along.
    const auto spaceBefore = [&] {
        char last = 0;
        for (const EditToken& t : tokens) {
            if (t.at >= p) break;
            last = s[t.at];
        }
        return editIsSpace(last);
    };
    while (p > 0 && spaceBefore()) p = caretStepLeft(s, p);
    while (p > 0 && !spaceBefore()) p = caretStepLeft(s, p);
    return p;
}

/// Where Ctrl+Delete deletes forward to (0x00963a10): over the whitespace
/// after the caret, then over the word after that.
inline size_t wordDeleteForwardTo(const std::string& s, size_t at) {
    size_t p = (at < s.size()) ? at : s.size();
    while (p < s.size() && editIsSpace(s[p])) p = caretStepRight(s, p);
    while (p < s.size() && !editIsSpace(s[p])) p = caretStepRight(s, p);
    return p;
}

}  // namespace ui
}  // namespace wowee

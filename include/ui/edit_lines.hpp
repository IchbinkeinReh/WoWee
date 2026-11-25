#pragma once

// The lines of a multi-line edit box (CSimpleEditBox with flag 0x2): where
// each starts in the text (+0x308, filled by 0x00482450 at the box's text
// width), which one the caret is on, and what the box's height becomes
// (0x00966710). Header-only and free of ImGui, as text_markup.hpp is.

#include "ui/text_markup.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace wowee::ui {

/// What of a piece of text is drawn.
inline std::string editDrawnText(const std::string& piece) {
    std::string out;
    for (const WrapRun& run : parseMarkup(piece)) out += run.text;
    return out;
}

/// The byte each drawn line starts at: after a line break, and where a line
/// runs out of `width` after the last space in it, or inside a word that has
/// a line to itself. A width of zero breaks at line breaks alone.
template <typename Measure>
std::vector<std::size_t> editLineStarts(const std::string& s, float width, Measure&& measure) {
    std::vector<std::size_t> starts{0};
    std::size_t line = 0;
    std::size_t afterSpace = 0;
    for (std::size_t p = 0; p < s.size();) {
        if (s[p] == '\n') {
            line = afterSpace = ++p;
            starts.push_back(line);
            continue;
        }
        const std::size_t q = std::max(caretStepRight(s, p), p + 1);
        // A space hangs over the edge rather than starting the next line.
        if (width > 0.0f && p > line && s[p] != ' ' && measure(editDrawnText(s.substr(line, q - line))) > width) {
            line = afterSpace > line ? afterSpace : p;
            afterSpace = line;
            p = line;
            starts.push_back(line);
            continue;
        }
        if (s[p] == ' ') afterSpace = q;
        p = q;
    }
    return starts;
}

struct Widget;
/// The lines of a box as it stands: its own text, face and width between the
/// side insets.
std::vector<std::size_t> editLinesOf(const Widget& box);

/// A line's height for a text of `size`, as drawMarkupText sets its lines.
inline constexpr float kEditLineSpacing = 1.2f;

/// The line a place in the text is drawn on (0x00964a90).
inline std::size_t editLineOf(const std::vector<std::size_t>& starts, std::size_t at) {
    std::size_t line = 0;
    while (line + 1 < starts.size() && starts[line + 1] <= at) ++line;
    return line;
}

/// The text of a line, its line break left off.
inline std::string editLineText(const std::string& s, const std::vector<std::size_t>& starts, std::size_t line) {
    const std::size_t from = std::min(starts[line], s.size());
    std::size_t to = line + 1 < starts.size() ? std::min(starts[line + 1], s.size()) : s.size();
    if (to > from && s[to - 1] == '\n') --to;
    return s.substr(from, to - from);
}

/// The height the box takes of its text (0x00966710): its lines, one at
/// least, between the insets.
inline float editBoxHeight(std::size_t lines, float lineHeight, float insetTop, float insetBottom) {
    return static_cast<float>(std::max<std::size_t>(lines, 1)) * lineHeight + insetTop + insetBottom;
}

/// The caret one line up (dir < 0) or down (dir > 0) in a multi-line box, as
/// 0x00964550 moves it: as many characters into the new line as it was into
/// its own, and no further than that line's end. `moved` is false when there
/// is no line that way.
inline std::size_t caretLineMove(const std::string& s, const std::vector<std::size_t>& starts, std::size_t at,
                                 int dir, bool& moved) {
    const std::size_t caret = std::min(at, s.size());
    const std::size_t line = editLineOf(starts, caret);
    moved = dir < 0 ? line > 0 : line + 1 < starts.size();
    if (!moved) return caret;
    std::size_t column = 0;
    for (std::size_t p = starts[line]; p < caret; p = std::max(caretStepRight(s, p), p + 1)) ++column;
    const std::size_t to = dir < 0 ? line - 1 : line + 1;
    std::size_t target = starts[to];
    for (; column > 0 && target < s.size(); --column) target = std::max(caretStepRight(s, target), target + 1);
    // Past the line's end it is before the last character of it, a line break
    // as a rule.
    if (to + 1 < starts.size() && target >= starts[to + 1]) target = caretStepLeft(s, starts[to + 1]);
    return std::min(target, s.size());
}

}  // namespace wowee::ui

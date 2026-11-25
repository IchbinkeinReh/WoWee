#pragma once

/// The lines an edit box draws its text in (CSimpleEditBox, 0x00966710): its
/// own breaks, and in a multi-line box those its width asks for.

#include "ui/text_markup.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace wowee::ui {

/// Where each line starts in the text, the first at 0. Broken where the words
/// no longer fit `width` - after the last space, or before the letter that does
/// not fit where the line has none -, 0 for not at all. The spaces a line ends
/// in are its own. `measure` answers how wide a piece of the text, markup and
/// all, is drawn.
template <typename Measure>
std::vector<size_t> editLines(const std::string& s, float width, Measure measure) {
    std::vector<size_t> starts{0};
    size_t line = 0, afterSpace = 0, at = 0;
    while (at < s.size()) {
        if (s[at] == '\n') {
            starts.push_back(++at);
            line = afterSpace = at;
            continue;
        }
        const size_t next = std::max(caretStepRight(s, at), at + 1);
        if (editIsSpace(s[at])) {
            afterSpace = at = next;
            continue;
        }
        if (width > 0.0f && at > line && measure(s.substr(line, next - line)) > width) {
            line = afterSpace > line ? afterSpace : at;
            afterSpace = line;
            starts.push_back(line);
            continue;
        }
        at = next;
    }
    return starts;
}

/// The line a place in the text is on: the one it starts, at its start.
[[nodiscard]] inline size_t editLineOf(const std::vector<size_t>& starts, size_t at) {
    const auto after = std::upper_bound(starts.begin(), starts.end(), at);
    return after == starts.begin() ? 0 : static_cast<size_t>(after - starts.begin()) - 1;
}

/// Where a line's letters end: before its break, where it has one.
[[nodiscard]] inline size_t editLineEnd(const std::string& s, const std::vector<size_t>& starts, size_t line) {
    if (line + 1 >= starts.size()) return s.size();
    const size_t next = starts[line + 1];
    return next > starts[line] && s[next - 1] == '\n' ? next - 1 : next;
}

/// The caret one drawn line up (dir < 0) or down (dir > 0), as 0x00964550 moves
/// it: as many characters into the new line as it was into its own, and no
/// further than that line's end. `moved` is false where there is no such line.
[[nodiscard]] inline size_t editLineMove(const std::string& s, const std::vector<size_t>& starts, size_t at, int dir,
                                         bool& moved) {
    const size_t caret = std::min(at, s.size());
    const size_t line = editLineOf(starts, caret);
    moved = dir < 0 ? line > 0 : line + 1 < starts.size();
    if (!moved) return caret;
    size_t column = 0;
    for (size_t p = starts[line]; p < caret; ++column) {
        const size_t q = caretStepRight(s, p);
        if (q <= p) break;
        p = q;
    }
    const size_t to = dir < 0 ? line - 1 : line + 1;
    // A wrapped line's end is the next one's start: the caret stays before it.
    const bool wrapped = to + 1 < starts.size() && editLineEnd(s, starts, to) == starts[to + 1];
    const size_t end = editLineEnd(s, starts, to);
    size_t target = starts[to];
    for (; column > 0 && target < end; --column) {
        const size_t q = caretStepRight(s, target);
        if (q <= target || q > end || (wrapped && q == end)) break;
        target = q;
    }
    return target;
}

/// The place in a line `x` from its start (0x00482510): after the letters that
/// fit before it; to the left of it, the text's start (0x00964790).
template <typename Measure>
size_t editPlaceIn(const std::string& s, const std::vector<size_t>& starts, size_t line, float x, Measure measure) {
    if (x < 0.0f) return 0;
    const size_t start = starts[std::min(line, starts.size() - 1)];
    const size_t end = editLineEnd(s, starts, std::min(line, starts.size() - 1));
    const bool wrapped = line + 1 < starts.size() && end == starts[line + 1];
    size_t at = start;
    while (at < end) {
        const size_t next = std::max(caretStepRight(s, at), at + 1);
        if (next > end || (wrapped && next == end) || measure(s.substr(start, next - start)) > x) break;
        at = next;
    }
    return at;
}

}  // namespace wowee::ui

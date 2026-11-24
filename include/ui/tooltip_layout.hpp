#pragma once

/// How wide a tooltip is, and where its wrapped lines break (0x0061caf0).

#include <algorithm>
#include <array>
#include <vector>

namespace wowee::ui::tooltip_layout {

/// Between a line's two sides, about the lines, between one and the next, and
/// the most a line that wraps asks for.
inline constexpr float kSides = 38.4f;
inline constexpr float kPad = 10.24f;
inline constexpr float kGap = 2.0f;
inline constexpr float kWrap = 230.4f;
/// Of the screen's width: between a texture on the line and its text.
inline constexpr float kTextureGap = 0.005f;

/// A line's colour: where the first of the three given is a number, all of them
/// (held between 0 and 1: 0x0061be50, 0x00960420), else the gold of 0x00ad2d2c.
[[nodiscard]] inline std::array<float, 3> lineColour(bool given, std::array<float, 3> colour) {
    if (!given) return {1.0f, 210.0f / 255.0f, 0.0f};
    for (float& part : colour) part = std::clamp(part, 0.0f, 1.0f);
    return colour;
}

struct Line {
    bool wrap = false;
    bool leftShown = true;
    bool rightShown = false;
    float left = 0.0f;       ///< as wide as its text in one line
    float right = 0.0f;
    float texture = 0.0f;    ///< a texture's width with its gap, 0 for none
};

struct Widths {
    float lines = 0.0f;              ///< the room of the lines
    std::vector<float> wrapAt;       ///< each line's, 0 for one that does not wrap
};

/// `widestRow(line, width)` is how wide the widest row of a line is, broken at
/// that width. `forced` is SetMinimumWidth's: every line wraps.
template <typename WidestRow>
Widths widths(const std::vector<Line>& lines, float minimum, bool forced, WidestRow&& widestRow) {
    Widths out;
    out.lines = minimum;
    out.wrapAt.assign(lines.size(), 0.0f);
    if (!forced) {
        for (const Line& line : lines) {
            if (line.wrap) continue;
            const float wide = (line.leftShown && line.rightShown ? kSides : 0.0f) + (line.leftShown ? line.left : 0.0f) +
                               (line.rightShown ? line.right : 0.0f) + line.texture;
            out.lines = std::max(out.lines, wide);
        }
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].wrap && !forced) continue;
        float wide = std::min(lines[i].left, kWrap);
        if (out.lines < wide) wide = widestRow(i, wide);
        out.lines = std::max(out.lines, wide);
        out.wrapAt[i] = out.lines;
    }
    return out;
}

}  // namespace wowee::ui::tooltip_layout

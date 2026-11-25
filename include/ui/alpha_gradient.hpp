#pragma once

namespace wowee::ui {

/// Whether a font string's SetAlphaGradient fades anything (0x00482230): not
/// before one was set (start -1), and not with no length - the quest windows
/// take it off with (0, 0).
[[nodiscard]] constexpr bool alphaGradientOn(int start, int length) { return start >= 0 && length > 0; }

/// How much of its own alpha the k-th glyph is drawn with under one (0x006c78f0):
/// whole before the start, then two steps of alpha / length down a glyph, none
/// past the ramp.
[[nodiscard]] constexpr float gradientAlpha(int start, int length, int glyph) {
    if (!alphaGradientOn(start, length) || glyph < start) return 1.0f;
    if (glyph >= start + length) return 0.0f;
    const float left = 1.0f - static_cast<float>(2 * (glyph - start) + 1) / static_cast<float>(length);
    return left > 0.0f ? left : 0.0f;
}

}  // namespace wowee::ui

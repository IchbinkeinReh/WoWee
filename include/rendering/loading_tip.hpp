#pragma once

#include <string>

namespace wowee::rendering::loading_tip {

/// Where the loading screen's tip stands (0x0040ab70), in the screen's pixels: a
/// column of 515/1024 of the picture's 4:3 in its middle, its text from the
/// column's left, the last line's foot a tenth of the screen up. The letters are
/// 0.018 of the screen's height, the lines 0.005 apart.
struct Place {
    float left = 0.0f, foot = 0.0f, width = 0.0f, size = 0.0f, spacing = 0.0f, shadowX = 0.0f, shadowY = 0.0f;
};
[[nodiscard]] constexpr Place place(float screenW, float screenH) {
    if (screenW <= 0.0f || screenH <= 0.0f) return {};
    const float wider = screenW / screenH * 0.75f;  // 0x0047bfe0
    const float width = 515.0f / (wider * 1024.0f);
    const float lift = wider >= 1.0f ? 0.0f : (1.0f - wider) * 0.5f;
    return {.left = (0.5f - width * 0.5f) * screenW,
            .foot = (1.0f - lift - 0.1f) * screenH,
            .width = width * screenW,
            .size = 0.018f * screenH,
            .spacing = 0.005f * screenH,
            .shadowX = 0.001f * screenW,
            .shadowY = 0.001f * screenH};
}
/// Its colour, 0xd7c8c8c8, over a black shadow.
constexpr unsigned char kGrey = 0xc8, kAlpha = 0xd7;

/// The row shown of `rows` GameTips rows, by the CVar gameTip (0x004d9c00): the
/// first for one that is none of them; the CVar is one more then. -1 without rows.
[[nodiscard]] constexpr int row(int cvar, int rows) {
    if (rows <= 0) return -1;
    return cvar < 0 || cvar >= rows ? 0 : cvar;
}

/// Without the line breaks at its end.
[[nodiscard]] inline std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

}  // namespace wowee::rendering::loading_tip

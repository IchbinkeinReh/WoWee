#pragma once

namespace wowee::ui {

/// Whether the mouse is in a region's rect, its four sides moved by as much, in
/// its own units (0x00488570): top, bottom, left, right, as IsMouseOver has them.
struct MouseOverSides {
    float top = 0.0f, bottom = 0.0f, left = 0.0f, right = 0.0f;
};
[[nodiscard]] constexpr bool mouseOver(float left, float bottom, float width, float height, float scale, float x, float y,
                                       const MouseOverSides& by) {
    const float s = scale > 0.0f ? scale : 1.0f;
    const float mx = x / s, my = y / s;
    return left / s + by.left < mx && mx < (left + width) / s + by.right && bottom / s + by.bottom < my &&
           my < (bottom + height) / s + by.top;
}

}  // namespace wowee::ui

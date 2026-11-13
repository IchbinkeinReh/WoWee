#pragma once

// The arithmetic of the two touch sticks, apart from the events, the camera
// and ImGui that TouchControls drives - so which half a thumb landed in, and
// how far a held look stick turns the view, can be asked without any of them.

#include <cmath>

namespace wowee::ui::touch {

/// The part of the window the sticks share, in window pixels from the top-left
/// corner: the display's safe area, clear of a cutout, rounded corners and the
/// gesture bar. The whole window where there is none.
struct Area {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

/// The window less what the display covers at each edge. Insets that leave
/// nothing are ignored: a phone reporting nonsense still gets two sticks.
inline Area usableArea(float windowW, float windowH, float insetLeft, float insetRight,
                       float insetTop, float insetBottom) {
    const float w = windowW - insetLeft - insetRight;
    const float h = windowH - insetTop - insetBottom;
    if (w <= 1.0f || h <= 1.0f) return Area{0.0f, 0.0f, windowW, windowH};
    return Area{insetLeft, insetTop, w, h};
}

/// Whether a press landed in the area at all. One outside it is on the
/// gesture bar or in the cutout's corner, and belongs to neither stick.
inline bool inside(float x, float y, const Area& area) {
    return x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h;
}

/// Where the halves meet: the middle of the usable area, not of the window, so
/// a notch on one side does not make that side's stick narrower.
inline float splitX(const Area& area) { return area.x + area.w * 0.5f; }

/// Which thumb a press belongs to. The left half walks, the right half turns,
/// each the full height; a press beyond the area's edge still counts for the
/// half on its side.
enum class Half { Move, Look };
inline Half halfAt(float x, const Area& area) {
    return x < splitX(area) ? Half::Move : Half::Look;
}

/// How far a held look stick turns the view in one update, in the pixels a
/// mouse drag would have moved - the camera takes those through the player's
/// own sensitivity, so the stick respects it.
///
/// A stick held at a deflection turns at a rate rather than one-to-one: a stick
/// that had to keep moving to keep turning would be a drag with a ring drawn on
/// it. Inside the deadzone nothing turns, and past it the turn starts from
/// nothing, so the ring's edge is not a jump.
struct Turn {
    float dx = 0.0f;
    float dy = 0.0f;
};
inline Turn lookStickTurn(float stickX, float stickY, float deadzone, float pixelsPerSecond,
                          float seconds) {
    const float mag = std::hypot(stickX, stickY);
    if (mag <= deadzone || seconds <= 0.0f || deadzone >= 1.0f) return {};
    const float gain = (std::fmin(mag, 1.0f) - deadzone) / (1.0f - deadzone);
    const float step = pixelsPerSecond * seconds * gain;
    return Turn{stickX / mag * step, stickY / mag * step};
}

}  // namespace wowee::ui::touch

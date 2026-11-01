#pragma once

// Where an interface panel can go: the display less what the device covers -
// an iPhone's notch and rounded corners, its home indicator, an Android
// phone's cutout. UIManager sets it each frame from SDL_GetWindowSafeArea; on
// a desktop the safe area is the whole window and nothing moves.
//
// In ImGui's display units (io.DisplaySize), which are the window's.

#include <imgui.h>

#include <algorithm>

namespace wowee::ui::safe_area {

struct Insets {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

inline Insets& current() {
    static Insets insets;
    return insets;
}

inline void set(const Insets& insets) { current() = insets; }

/// The safe part of the display: its top left corner and its size.
struct Rect {
    ImVec2 min;
    ImVec2 size;
    [[nodiscard]] ImVec2 max() const { return ImVec2(min.x + size.x, min.y + size.y); }
    /// The top left of a w x h box centred in it.
    [[nodiscard]] ImVec2 centered(float w, float h) const {
        return ImVec2(min.x + (size.x - w) * 0.5f, min.y + (size.y - h) * 0.5f);
    }
};

inline Rect rect() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Insets& in = current();
    return Rect{ImVec2(in.left, in.top),
                ImVec2(std::max(1.0f, display.x - in.left - in.right),
                       std::max(1.0f, display.y - in.top - in.bottom))};
}

}  // namespace wowee::ui::safe_area

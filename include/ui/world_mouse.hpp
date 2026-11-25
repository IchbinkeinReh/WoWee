#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee::ui {

/// The WorldFrame is the frame under everything that takes the mouse (the
/// client's CGWorldFrame): what the mouse is on where nothing of the interface
/// is (0x00516bf0), and what a button pressed there is pressed on.
[[nodiscard]] constexpr uint32_t mouseFocus(uint32_t hovered, uint32_t world, bool worldShown) {
    return hovered != 0 ? hovered : worldShown ? world : 0;
}

template <size_t Buttons>
class WorldPresses {
public:
    /// A button went down on `hit`: whether that is the world's.
    bool down(size_t button, uint32_t hit) {
        on_[button] = hit == 0;
        return on_[button];
    }
    /// It went up, wherever: whether it had gone down on the world.
    bool up(size_t button) {
        const bool was = on_[button];
        on_[button] = false;
        return was;
    }

private:
    bool on_[Buttons] = {};
};

}  // namespace wowee::ui

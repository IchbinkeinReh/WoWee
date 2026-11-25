#pragma once

#include <array>

namespace wowee::ui {

/// Which of a button's pictures is shown (+0x2d0): its state's, where the state
/// has one; else the one that was, unless the button is up (0x0096fe30). So a
/// button without a DisabledTexture keeps its normal picture as it is disabled,
/// and one disabled while it is pressed its pushed one.
struct ButtonArtShown {
    enum { Disabled, Normal, Pushed, None };
    int state = Normal;
    int shown = None;

    /// The state it is in now, and which of the three pictures there are.
    void note(int now, const std::array<bool, 3>& there) {
        if (now != state && now == Normal) shown = None;
        if (there[static_cast<size_t>(now)]) shown = now;
        state = now;
    }
};

}  // namespace wowee::ui

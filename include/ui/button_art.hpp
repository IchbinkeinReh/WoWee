#pragma once

#include <array>
#include <cstddef>

namespace wowee::ui {

/// Which of a button's pictures is shown (+0x2d0): its state's, where the state
/// has one; else the one that was, unless the button is up (0x0096fe30). So a
/// button without a DisabledTexture keeps its normal picture as it is disabled,
/// and one disabled while it is pressed its pushed one.
class ButtonArtShown {
public:
    enum Art { Disabled, Normal, Pushed, None };
    [[nodiscard]] Art shown() const { return shown_; }

    /// It has a picture for that state (0x0096f920): shown where it is the one
    /// it is in.
    void give(Art art) {
        there_[static_cast<size_t>(art)] = true;
        if (art == state_) shown_ = art;
    }
    /// The state it is in from here on.
    void note(Art state) {
        if (state == state_) return;
        if (state == Normal) shown_ = None;
        if (there_[static_cast<size_t>(state)]) shown_ = state;
        state_ = state;
    }

private:
    Art state_ = Normal;
    Art shown_ = None;
    std::array<bool, 3> there_{};
};

}  // namespace wowee::ui

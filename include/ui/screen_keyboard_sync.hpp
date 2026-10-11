#pragma once

namespace wowee {
namespace ui {

/// Keeps an on-screen keyboard and the interface's edit focus saying the same
/// thing.
///
/// A phone has one way to type: the keyboard the system draws over the game.
/// Tapping the chat box focuses it and SDL_StartTextInput raises the keyboard;
/// that half always worked. The other half did not. The player closes the
/// keyboard - the back gesture, the keyboard's own hide key - and the chat box
/// kept the focus, so every tap on the world after it was still being typed
/// into a box with nothing to type with. And the main loop, seeing a focused
/// box without text input, started text input again and the keyboard came
/// straight back.
///
/// The rule is the one a real client on a desktop never needed: a keyboard
/// that goes away takes the focus with it, the way Escape does - the edit box
/// is told to ClearFocus, and FrameXML's ChatEdit_OnEditFocusLost runs
/// ChatEdit_DeactivateChat from there. And the other way: an edit box that
/// lets the focus go puts the keyboard away.
///
/// "Goes away" is a keyboard that was seen up during this focus and is no
/// longer up. One that has not come up yet is still on its way - it animates
/// in over a few frames - and one that never comes up is a hardware keyboard,
/// which is no reason to drop anything.
///
/// Pure: the caller reads SDL and the interface, feeds update(), and carries
/// out what it answers. Where there is no screen keyboard at all the only
/// answer is the one the desktop has always had - text input started for a
/// focused box that lost it.
class ScreenKeyboardSync {
public:
    struct Inputs {
        bool hasScreenKeyboard = false;  ///< SDL_HasScreenKeyboardSupport()
        bool editFocused = false;        ///< an interface edit box has the focus
        bool keyboardShown = false;      ///< the system says the keyboard is up
        bool textInputActive = false;    ///< SDL_TextInputActive()
        bool otherWantsText = false;     ///< an ImGui field of the client's own
    };
    struct Actions {
        bool startTextInput = false;  ///< SDL_StartTextInput (raises the keyboard)
        bool stopTextInput = false;   ///< SDL_StopTextInput (puts it away)
        bool clearFocus = false;      ///< the edit box's ClearFocus
    };

    Actions update(const Inputs& in) {
        Actions out;
        const bool gained = in.editFocused && !wasFocused_;
        const bool lost = !in.editFocused && wasFocused_;
        wasFocused_ = in.editFocused;

        if (!in.hasScreenKeyboard) {
            keyboardSeenUp_ = false;
            if (in.editFocused && !in.textInputActive) out.startTextInput = true;
            return out;
        }

        if (gained) keyboardSeenUp_ = false;

        if (in.editFocused) {
            if (in.keyboardShown) {
                keyboardSeenUp_ = true;
            } else if (keyboardSeenUp_) {
                // Up during this focus and down now: the player closed it.
                keyboardSeenUp_ = false;
                wasFocused_ = false;
                out.clearFocus = true;
                if (in.textInputActive && !in.otherWantsText) out.stopTextInput = true;
                return out;
            }
            if (!in.textInputActive) out.startTextInput = true;
            return out;
        }

        keyboardSeenUp_ = false;
        // The box let the focus go. The keyboard goes with it, unless one of
        // the client's own fields has asked for it in the meantime.
        if (lost && (in.textInputActive || in.keyboardShown) && !in.otherWantsText) {
            out.stopTextInput = true;
        }
        return out;
    }

    [[nodiscard]] bool keyboardSeenUp() const { return keyboardSeenUp_; }

private:
    bool wasFocused_ = false;
    bool keyboardSeenUp_ = false;
};

} // namespace ui
} // namespace wowee

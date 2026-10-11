// The Android lifecycle and the on-screen keyboard, as decisions.
//
// Neither can be exercised here: there is no activity to send to the
// background and no keyboard to close. What can be is what the client decides
// when they happen - SurfaceLifecycle for the Vulkan surface, and
// ScreenKeyboardSync for the chat box's focus - which is where both bugs were:
// a surface kept after its window was gone, and a focus kept after its
// keyboard was.
#include <catch_amalgamated.hpp>

#include "rendering/surface_lifecycle.hpp"
#include "ui/android_ime.hpp"
#include "ui/screen_keyboard_sync.hpp"

using wowee::rendering::SurfaceLifecycle;
using Action = SurfaceLifecycle::Action;
using wowee::ui::ScreenKeyboardSync;

TEST_CASE("the surface is released in the background and rebuilt in front", "[lifecycle]") {
    SurfaceLifecycle s;
    CHECK(s.canRender());
    CHECK(s.next(true) == Action::None);

    // WILL and DID both arrive; the second is no change.
    CHECK(s.enterBackground());
    CHECK_FALSE(s.enterBackground());
    CHECK_FALSE(s.canRender());                 // nothing drawn in the background
    CHECK(s.next(true) == Action::Release);
    s.surfaceReleased();
    // Released, and still away: nothing to build on and no reason to.
    CHECK(s.next(true) == Action::None);
    CHECK(s.next(false) == Action::None);
    CHECK_FALSE(s.canRender());

    CHECK(s.enterForeground());
    CHECK(s.next(true) == Action::Restore);
    s.surfaceRestored();
    CHECK(s.canRender());
    CHECK(s.next(true) == Action::None);
}

TEST_CASE("a resume before the native window waits for it", "[lifecycle]") {
    SurfaceLifecycle s;
    s.enterBackground();
    s.surfaceReleased();
    s.enterForeground();
    // SDL can say the app is back before surfaceCreated has handed over a
    // window. Building on none is undefined; this waits and keeps asking.
    CHECK(s.next(false) == Action::None);
    CHECK_FALSE(s.canRender());
    CHECK(s.next(true) == Action::Restore);

    // A restore the driver refuses is tried again, and said only now and then.
    s.restoreFailed();
    CHECK(s.shouldLogFailedRestore());
    s.restoreFailed();
    CHECK_FALSE(s.shouldLogFailedRestore());
    CHECK(s.next(true) == Action::Restore);
    s.surfaceRestored();
    CHECK(s.failedRestores() == 0);
    CHECK(s.canRender());
}

TEST_CASE("a surface the driver lost is rebuilt with no lifecycle event", "[lifecycle]") {
    // The window went before the event saying so was read - or the event was
    // read by a loading loop that dropped it. VK_ERROR_SURFACE_LOST_KHR is
    // then all there is, and it used to be logged and nothing else, forever.
    SurfaceLifecycle s;
    s.surfaceLostByDriver();
    CHECK_FALSE(s.canRender());
    CHECK(s.next(true) == Action::Release);
    s.surfaceReleased();
    CHECK(s.next(true) == Action::Restore);
    s.surfaceRestored();
    CHECK(s.canRender());
}

TEST_CASE("a surface lost on the way to the background waits for the foreground", "[lifecycle]") {
    SurfaceLifecycle s;
    s.surfaceLostByDriver();
    s.enterBackground();
    CHECK(s.next(true) == Action::Release);
    s.surfaceReleased();
    CHECK(s.next(true) == Action::None);
    s.enterForeground();
    CHECK(s.next(true) == Action::Restore);
}

namespace {
ScreenKeyboardSync::Inputs phone(bool focused, bool shown, bool active) {
    ScreenKeyboardSync::Inputs in;
    in.hasScreenKeyboard = true;
    in.editFocused = focused;
    in.keyboardShown = shown;
    in.textInputActive = active;
    return in;
}
}  // namespace

TEST_CASE("closing the on-screen keyboard takes the chat box's focus", "[keyboard]") {
    ScreenKeyboardSync sync;
    // Tapped: focused, text input not on yet - start it.
    auto a = sync.update(phone(true, false, false));
    CHECK(a.startTextInput);
    CHECK_FALSE(a.clearFocus);
    // The keyboard animates in over a few frames. Not up yet is not closed.
    a = sync.update(phone(true, false, true));
    CHECK_FALSE(a.clearFocus);
    CHECK_FALSE(a.startTextInput);
    a = sync.update(phone(true, true, true));
    CHECK_FALSE(a.clearFocus);
    // Put away with its own hide key: text input still on, keyboard gone.
    a = sync.update(phone(true, false, true));
    CHECK(a.clearFocus);
    CHECK(a.stopTextInput);
    CHECK_FALSE(a.startTextInput);
    // Focus gone: nothing more to do, and in particular no keyboard raised.
    a = sync.update(phone(false, false, false));
    CHECK_FALSE(a.startTextInput);
    CHECK_FALSE(a.stopTextInput);
    CHECK_FALSE(a.clearFocus);
}

TEST_CASE("the back key closing the keyboard does not raise it again", "[keyboard]") {
    ScreenKeyboardSync sync;
    sync.update(phone(true, false, false));
    sync.update(phone(true, true, true));
    // SDL's onNativeKeyboardFocusLost stops text input itself. The loop used
    // to see a focused box without text input and start it again.
    const auto a = sync.update(phone(true, false, false));
    CHECK(a.clearFocus);
    CHECK_FALSE(a.startTextInput);
}

TEST_CASE("an edit box letting the focus go puts the keyboard away", "[keyboard]") {
    ScreenKeyboardSync sync;
    sync.update(phone(true, false, false));
    sync.update(phone(true, true, true));
    // Enter sent the line, or Escape: the box let go and the keyboard is up.
    auto a = sync.update(phone(false, true, true));
    CHECK(a.stopTextInput);
    CHECK_FALSE(a.clearFocus);
    // Not while one of the client's own fields wants it.
    ScreenKeyboardSync other;
    other.update(phone(true, true, true));
    auto in = phone(false, true, true);
    in.otherWantsText = true;
    CHECK_FALSE(other.update(in).stopTextInput);
}

TEST_CASE("a keyboard that never comes up is not a keyboard closed", "[keyboard]") {
    // A hardware keyboard: SDL raises nothing on the screen and the box keeps
    // the focus for as long as the player types.
    ScreenKeyboardSync sync;
    for (int i = 0; i < 10; ++i) {
        const auto a = sync.update(phone(true, false, true));
        CHECK_FALSE(a.clearFocus);
    }
}

TEST_CASE("a new focus starts afresh", "[keyboard]") {
    ScreenKeyboardSync sync;
    sync.update(phone(true, true, true));
    sync.update(phone(false, false, false));
    // Focused again, keyboard on its way: the last one being seen up says
    // nothing about this one.
    const auto a = sync.update(phone(true, false, false));
    CHECK_FALSE(a.clearFocus);
    CHECK(a.startTextInput);
}

TEST_CASE("without a screen keyboard only text input is kept on", "[keyboard]") {
    // The desktop, unchanged: a focused box gets text input back whenever
    // something turned it off, and losing focus leaves it alone.
    ScreenKeyboardSync sync;
    ScreenKeyboardSync::Inputs in;
    in.editFocused = true;
    CHECK(sync.update(in).startTextInput);
    in.textInputActive = true;
    auto a = sync.update(in);
    CHECK_FALSE(a.startTextInput);
    in.editFocused = false;
    a = sync.update(in);
    CHECK_FALSE(a.stopTextInput);
    CHECK_FALSE(a.clearFocus);
}

TEST_CASE("the activity's keyboard reports are kept", "[keyboard]") {
    CHECK(wowee::ui::reportedImeVisibility() == -1);
    wowee::ui::noteImeVisibility(true);
    CHECK(wowee::ui::reportedImeVisibility() == 1);
    wowee::ui::noteImeVisibility(false);
    CHECK(wowee::ui::reportedImeVisibility() == 0);
}

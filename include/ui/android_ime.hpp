#pragma once

namespace wowee {
namespace ui {

/// Whether the system's on-screen keyboard is up, as the activity sees it.
///
/// SDL 3.2 has no event for the keyboard going away, and what it answers
/// through SDL_ScreenKeyboardShown is InputMethodManager.isAcceptingText(),
/// which stays true after the keyboard's own hide key has put it away - the
/// input connection is still open. The window's IME inset is what actually
/// says whether a keyboard is on the screen, so WoweeActivity reports that
/// (nativeImeVisibilityChanged) and this keeps the last report.
///
///   -1  nothing reported yet: not Android, or no inset has arrived
///    0  hidden
///    1  shown
[[nodiscard]] int reportedImeVisibility();

/// What WoweeActivity's JNI entry calls. Any thread; the Java UI thread in
/// practice. Exposed so the bookkeeping can be driven without a JVM.
void noteImeVisibility(bool visible);

} // namespace ui
} // namespace wowee

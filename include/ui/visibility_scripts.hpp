#pragma once

// When a frame's OnShow and OnHide run.
//
// The client runs both inside the call that changes it (0x0048f620 -> 0x00491830
// for Hide: the frame is off, its children's OnHide, its own; 0x0048f660 ->
// 0x004918f0 for Show). Here OnShow runs inside Show where that Show alone
// changed it, and the rest is announced once a frame, by comparing what is
// visible with what was last said (LuaEngine::updateVisibility) - a Hide and a
// Show in one frame as both, in that order, after everything else the frame
// did.
//
// A tooltip's OnHide runs inside Hide as the client's does. It is hidden by
// what the pointer leaves and filled and shown by what it enters, in one frame:
// announced afterwards, GameTooltip_OnHide took down the money line just put
// there (the flight map's price). Other frames keep the pass: the panels'
// handlers have not been proven against the order.

#include "ui/widget_tree.hpp"

namespace wowee::ui {

/// Whether a frame about to be hidden runs its OnHide within the call.
/// `ancestorsShown`: it is on screen now, as the client's +0xe0.
[[nodiscard]] inline bool hideRunsOnHide(const Widget& w, bool ancestorsShown) {
    return w.objectType == "GameTooltip" && w.shown && ancestorsShown;
}

/// Its OnHide has run: nothing of what came before is left to say.
inline void noteHideSaid(Widget& w) {
    w.shownToggles = 0;
    w.reportedVisible = false;
    w.onShowFired = false;
}

/// What the pass has to say of a frame.
struct VisibilityOwed {
    bool changed = false;  ///< visible or no longer, since it was last said
    bool hide = false;     ///< OnHide is to run
    bool show = false;     ///< and then OnShow
};

/// Takes it, leaving nothing owed.
inline VisibilityOwed takeVisibilityOwed(Widget& w) {
    VisibilityOwed owed;
    const uint8_t toggles = w.shownToggles;
    w.shownToggles = 0;
    if (w.visible == w.reportedVisible) {
        // Nothing changed overall - but if Lua hid this and showed it again
        // before anything looked, something *did* happen and both handlers
        // are owed.
        if (toggles >= 2 && w.visibleChain) owed.hide = owed.show = true;
        return owed;
    }
    owed.changed = true;
    w.reportedVisible = w.visible;
    // Show() runs OnShow itself when the frame became shown under shown
    // ancestors, so the change is recorded here without being announced
    // twice. The flag is cleared either way: a frame that was shown and
    // then anchored later arrives here once, and only once.
    const bool already = w.onShowFired;
    w.onShowFired = false;
    owed.hide = !w.visible;
    owed.show = w.visible && !already;
    return owed;
}

}  // namespace wowee::ui

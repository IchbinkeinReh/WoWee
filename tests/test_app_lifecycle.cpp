// The Android lifecycle, as decisions.
//
// It cannot be exercised here: there is no activity to send to the
// background. What can be is what the client decides when it happens -
// SurfaceLifecycle for the Vulkan surface - which is where the bug was: a
// surface kept after its window was gone.
#include <catch_amalgamated.hpp>

#include "rendering/surface_lifecycle.hpp"

using wowee::rendering::SurfaceLifecycle;
using Action = SurfaceLifecycle::Action;

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

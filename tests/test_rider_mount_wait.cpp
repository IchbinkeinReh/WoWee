// A rider held back until its mount is drawn (0x0073d5d0 hangs the rider on
// its mount's model; the unit's loaded callback, 0x008251b0 -> 0x00743110,
// waits for both).
#include <catch_amalgamated.hpp>

#include "rendering/rider_mount_wait.hpp"

namespace rmw = wowee::rendering::rider_mount_wait;
using rmw::Decision;

TEST_CASE("a unit on foot is shown at once", "[rider_mount_wait]") {
    CHECK(rmw::decide({.wantedMount = 0}) == Decision::Show);
    // Got down while its old mount is still drawn: on foot, now.
    CHECK(rmw::decide({.wantedMount = 0, .drawnMount = 14338}) == Decision::Show);
}

TEST_CASE("a new rider waits for its mount", "[rider_mount_wait]") {
    CHECK(rmw::decide({.wantedMount = 14338, .drawnMount = 0, .waitedSeconds = 0.0f}) == Decision::Hold);
    CHECK(rmw::decide({.wantedMount = 14338, .drawnMount = 0, .waitedSeconds = 1.5f}) == Decision::Hold);
    CHECK_FALSE(rmw::shown(rmw::decide({.wantedMount = 14338})));
    // Its mount drawn under it: shown, mounted.
    CHECK(rmw::decide({.wantedMount = 14338, .drawnMount = 14338}) == Decision::Show);
}

TEST_CASE("a mount that never comes cannot hide a unit for ever", "[rider_mount_wait]") {
    CHECK(rmw::decide({.wantedMount = 14338, .waitedSeconds = rmw::kTimeoutSeconds}) ==
          Decision::ShowTimedOut);
    CHECK(rmw::decide({.wantedMount = 14338, .waitedSeconds = 60.0f}) == Decision::ShowTimedOut);
    CHECK(rmw::shown(Decision::ShowTimedOut));
    CHECK(rmw::kTimeoutSeconds > 0.0f);
    CHECK(rmw::kTimeoutSeconds <= 10.0f);
}

TEST_CASE("a unit already drawn keeps what it shows until the new mount", "[rider_mount_wait]") {
    // From one mount to another: the old one stays.
    CHECK(rmw::decide({.wantedMount = 2410, .drawnMount = 14338, .revealed = true}) == Decision::Show);
    // Mounting up in sight: on foot until the mount is there.
    CHECK(rmw::decide({.wantedMount = 2410, .drawnMount = 0, .revealed = true}) == Decision::Show);
    // Not yet drawn, its stale mount is not the one it wants: it waits.
    CHECK(rmw::decide({.wantedMount = 2410, .drawnMount = 14338, .revealed = false}) == Decision::Hold);
}

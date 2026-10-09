// A unit on its mount (0x0073d5d0): the mount drawn at the unit's size times
// its display's (0x0071c0e0), the seat by that size, and the rider's pose
// (+0xb7c) a mount aura's kit sets (0x00724820, 0x0071e930).
#include <catch_amalgamated.hpp>

#include "rendering/mount_seat.hpp"

namespace ms = wowee::rendering::mount_seat;

TEST_CASE("the mount is the unit's size times its display's (0x0071c0e0)", "[mount_seat]") {
    CHECK(ms::mountModelScale(1.0f, 1.5f) == Catch::Approx(1.5f));
    CHECK(ms::mountModelScale(2.0f, 0.8f) == Catch::Approx(1.6f));
    // Not positive reads 1.
    CHECK(ms::mountModelScale(0.0f, 1.2f) == Catch::Approx(1.2f));
    CHECK(ms::mountModelScale(1.3f, 0.0f) == Catch::Approx(1.3f));
}

TEST_CASE("the seat rises with the mount's size", "[mount_seat]") {
    CHECK(ms::seatHeight(1.6f, 1.5f) == Catch::Approx(2.4f));
    CHECK(ms::seatHeight(1.6f, 0.0f) == Catch::Approx(1.6f));
}

TEST_CASE("a mount aura's state kit sets the rider's pose (0x00724820)", "[mount_seat]") {
    CHECK(ms::kRiderPoseMount == 91u);
    CHECK(ms::riderPoseOnMountAura(ms::kRiderPoseMount, 139) == 139u);
    // A kit with no animation (-1) leaves the pose as it is.
    CHECK(ms::riderPoseOnMountAura(ms::kRiderPoseMount, -1) == ms::kRiderPoseMount);
    CHECK(ms::riderPoseOnMountAura(139, -1) == 139u);
    // 0 is an animation, Stand.
    CHECK(ms::riderPoseOnMountAura(ms::kRiderPoseMount, 0) == 0u);
}

TEST_CASE("the aura going puts the active player's pose back (0x0071e930)", "[mount_seat]") {
    CHECK(ms::riderPoseOnMountAuraGone(139, 139, true) == ms::kRiderPoseMount);
    // Another kit's pose stays.
    CHECK(ms::riderPoseOnMountAuraGone(140, 139, true) == 140u);
    // Another unit keeps its pose.
    CHECK(ms::riderPoseOnMountAuraGone(139, 139, false) == 139u);
    CHECK(ms::riderPoseOnMountAuraGone(ms::kRiderPoseMount, -1, true) == ms::kRiderPoseMount);
}

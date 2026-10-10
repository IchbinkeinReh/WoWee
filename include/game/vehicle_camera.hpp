#pragma once

// The camera of a player in a seat, as the vehicle camera (VehicleCamera_C,
// CGUnit_C +0xf64) sets it: when its change of seat moves the camera and over
// how long (0x0075aac0), and the zoom the seat then asks for (0x0074c0e0).

#include "game/vehicle.hpp"
#include "game/vehicle_transition.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::game::vehicle {

/// VehicleSeat Flags (+4) and FlagsB (+0xb4) the camera reads.
inline constexpr uint32_t kSeatFlagCanControl = 0x800;             ///< 0x0074c0e0, 0x00613700
inline constexpr uint32_t kSeatFlagFixedCamera = 0x1000000;        ///< 0x0074c0e0: no zoom
inline constexpr uint32_t kSeatFlagBCameraZooms = 0x400;
inline constexpr uint32_t kSeatFlagBEnterCameraTimes = 0x40;      ///< 0x0075aac0
inline constexpr uint32_t kSeatFlagBExitCameraTimes = 0x80;
inline constexpr uint32_t kSeatFlagBKeepCloserZoom = 0x1000;       ///< not drawn out to the entering zoom
inline constexpr uint32_t kSeatFlagBKeepFartherZoom = 0x2000;      ///< not drawn in to it
inline constexpr uint32_t kSeatFlagBZoomMin = 0x4000;
inline constexpr uint32_t kSeatFlagBZoomMax = 0x8000;

/// When a change of seat moves the camera, and over how long (0x0075aac0):
/// after the wait the seat begins with (its pre-delay), or the seat's
/// camera delay where FlagsB 0x40 (entering) or 0x80 (leaving) give one and
/// its camera duration with it; a change not moved through, at once. Else
/// the move's MaxDuration held to 0.5..3 s - the time of the flight that
/// follows, once it is known (0x0075af00 from 0x0074a200), where there is
/// one; half a second with no seat.
struct CameraTiming {
    uint32_t delayMs = 0;
    uint32_t durationMs = 0;
    bool fixed = false;  ///< +0x8 0x1: the duration is final
};
[[nodiscard]] inline CameraTiming cameraTiming(SeatState state, const SeatRecord* seat, bool animated) {
    const bool leaving = state == SeatState::None || state == SeatState::ExitDelay || state == SeatState::Exiting;
    CameraTiming t;
    float delay = 0.0f;
    if (seat && state == SeatState::EnterDelay) delay = seat->enterPreDelay;
    if (seat && state == SeatState::ExitDelay) delay = seat->exitPreDelay;
    if (!animated) {
        t.fixed = true;
    } else if (!seat) {
        t.durationMs = 500;
    } else if (leaving ? (seat->flagsB & kSeatFlagBExitCameraTimes) != 0
                       : (seat->flagsB & kSeatFlagBEnterCameraTimes) != 0) {
        delay = leaving ? seat->cameraExitingDelay : seat->cameraEnteringDelay;
        const float seconds = leaving ? seat->cameraExitingDuration : seat->cameraEnteringDuration;
        t.durationMs = static_cast<uint32_t>(std::max(0L, std::lround(seconds * 1000.0f)));
        t.fixed = true;
    } else {
        const float seconds = std::clamp(leaving ? seat->exitMaxDuration : seat->enterMaxDuration, 0.5f, 3.0f);
        t.durationMs = static_cast<uint32_t>(std::lround(seconds * 1000.0f));
    }
    t.delayMs = delay > 0.0f ? static_cast<uint32_t>(std::lround(delay * 1000.0f)) : 0u;
    return t;
}

/// The zoom a seat sets (0x0074c0e0), with the player in it (states 1 to
/// 3) or not. Out of one, or in a seat that zooms nothing (no FlagsB
/// 0x400), the distance remembered on the way in comes back. In a seat with
/// Flags 0x1000000 the camera is left be. Otherwise its limits are FlagsB
/// 0x4000's CameraSeatZoomMin and 0x8000's CameraSeatZoomMax; with Flags
/// 0x800 it is drawn to the CameraEnteringZoom - out unless FlagsB 0x1000,
/// in unless 0x2000 - and the limits widened to take it; and held in them.
/// A change is zoomed to over the camera's time, the distance before it
/// remembered unless one already is.
struct CameraZoom {
    std::optional<float> minDistance;
    std::optional<float> maxDistance;
    std::optional<float> zoomTo;
    bool remember = false;  ///< 0x005fe920: the distance before is kept
    bool restore = false;   ///< zoom back to it and forget it (0x005fe940)
};
[[nodiscard]] inline CameraZoom seatCameraZoom(bool seated, const SeatRecord* seat, float distance,
                                               bool remembering) {
    CameraZoom z;
    if (!seated || !seat) {
        z.restore = remembering;
        return z;
    }
    if (seat->flags & kSeatFlagFixedCamera) return z;
    if ((seat->flagsB & kSeatFlagBCameraZooms) == 0) {
        z.restore = remembering;
        return z;
    }
    bool hasMin = (seat->flagsB & kSeatFlagBZoomMin) != 0;
    bool hasMax = (seat->flagsB & kSeatFlagBZoomMax) != 0;
    float minDistance = hasMin ? seat->cameraSeatZoomMin : 0.0f;   // 0x004d5ee0
    float maxDistance = hasMax ? seat->cameraSeatZoomMax : 50.0f;  // 0x005fe570
    float target = distance;
    if (seat->flags & kSeatFlagCanControl) {
        const float entering = seat->cameraEnteringZoom;
        if (target < entering && (seat->flagsB & kSeatFlagBKeepCloserZoom) == 0) target = entering;
        if (entering < target && (seat->flagsB & kSeatFlagBKeepFartherZoom) == 0) target = entering;
        if (!hasMin && entering < minDistance) {
            minDistance = entering;
            hasMin = true;
        }
        if (!hasMax && maxDistance < entering) {
            maxDistance = entering;
            hasMax = true;
        }
    }
    if (hasMin) {
        z.minDistance = minDistance;
        target = std::max(target, minDistance);
    }
    if (hasMax) {
        z.maxDistance = maxDistance;
        target = std::min(target, maxDistance);
    }
    if (std::fabs(distance - target) > 0.001f) {  // 0x004828c0
        z.remember = !remembering;
        z.zoomTo = target;
    }
    return z;
}

}  // namespace wowee::game::vehicle

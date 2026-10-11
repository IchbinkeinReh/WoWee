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
inline constexpr uint32_t kSeatFlagFixedCamera = 0x1000000;        ///< 0x0074c0e0: the vehicle distance mode
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
/// Flags 0x1000000 the zoom is the vehicle distance mode's (0x00600590).
/// Otherwise its limits are FlagsB 0x4000's CameraSeatZoomMin and 0x8000's
/// CameraSeatZoomMax; with Flags 0x800 it is drawn to the CameraEnteringZoom
/// - out unless FlagsB 0x1000, in unless 0x2000 - and the limits widened to
/// take it; and held in them.
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

/// The vehicle camera's chase of a value toward its target (0x007160b0 for
/// one, 0x006ff4c0 for a point): a spring that settles at `rate` without
/// overshooting, its velocity kept between steps.
template <class T>
inline void chaseStep(T& value, T& velocity, const T& target, float rate, float dt) {
    const float x = rate * dt;
    const float d = 1.0f / (x * x * 0.48f + x * x * x * 0.235f + x + 1.0f);
    const T off = value - target;
    const T push = (off * rate + velocity) * dt;
    value = (off + push) * d + target;
    velocity = (velocity - push * rate) * d;
}

/// The rate a seat chases at: 20 over its CameraPosChaseRate or
/// CameraFacingChaseRate, none for one of 0.0001 or less (0x00759d80,
/// 0x0075a1c0).
[[nodiscard]] inline std::optional<float> seatChaseRate(float column) {
    if (!(0.0001f < column)) return std::nullopt;
    return 20.0f / column;
}

/// A seat's CameraOffset about the unit (0x00759200): (Y, -X, Z) turned by
/// its facing - Y ahead, X to its right - here on the render axes, whose
/// yaw `renderYaw` is a quarter turn less the game's facing taken the
/// other way.
[[nodiscard]] inline glm::vec3 seatCameraOffset(const SeatRecord& seat, float renderYaw) {
    const float c = std::cos(renderYaw);
    const float s = std::sin(renderYaw);
    const glm::vec3& o = seat.cameraOffset;
    return {o.y * c - o.x * s, o.y * s + o.x * c, o.z};
}

/// The camera's ease over a move's time (0x005fffd0): its cosine of pi*t
/// taken by a cubic on t less its nearest whole number (0x005fe800),
/// turned to 0..1.
[[nodiscard]] inline float cameraEase(float t) {
    // 0x005fe800: the nearest whole number below a non-positive t.
    float n = std::nearbyint(t);
    if (!(t > 0.0f)) n -= 1.0f;
    const float f = t - n;
    float c = 1.0f - (6.0f - 4.0f * f) * f * f;
    if (static_cast<long>(n) & 1L) c = -c;
    return 0.5f - c * 0.5f;
}

/// A camera offset eased from where it is to where it is asked to be
/// between two times (0x005fe890 / 0x005fe8d0 set it, 0x006006a0 /
/// 0x006007b0 step it): the Vehicle.dbc CameraYawOffset (+0x2dc..+0x2e4)
/// and CameraPitchOffset (+0x2f0..+0x2f8) of the vehicle a seat belongs to.
struct EasedCameraOffset {
    uint32_t startMs = 0;
    uint32_t endMs = 0;
    float from = 0.0f;
    float to = 0.0f;
    float value = 0.0f;

    void set(float target, uint32_t start, uint32_t end) {
        startMs = start;
        endMs = end;
        if (start != end) {
            from = value;
            to = target;
        } else {
            value = target;
        }
    }
    void step(uint32_t nowMs) {
        if (startMs == endMs) return;
        const float t = static_cast<float>(nowMs - startMs) / static_cast<float>(endMs - startMs);
        if (1.0f < t) {
            startMs = endMs;
            value = to;
            return;
        }
        value = (to - from) * cameraEase(t) + from;
    }
};

/// The camera's distance mode (0x00600590): 0 the ordinary, 1 a seat with
/// Flags 0x1000000's. A change zooms to the new mode's saved distance -
/// cameraSavedDistance held to 0..50, cameraSavedVehicleDistance or 50 where
/// it is below 0 - over the time given, half a second at least, where it is
/// asked to and the distance differs by 0.001 or more.
[[nodiscard]] inline float savedModeDistance(int mode, float savedDistance, float savedVehicleDistance) {
    if (mode == 0) return savedDistance < 0.0f ? 0.0f : (savedDistance < 50.0f ? savedDistance : 50.0f);
    return savedVehicleDistance < 0.0f ? 50.0f : savedVehicleDistance;
}

/// What a change of seat asks of the camera's distance mode (0x0074c0e0):
/// a seat with Flags 0x1000000 the vehicle mode, zoomed to; a seat that
/// zooms (FlagsB 0x400) the ordinary one, not zoomed to; anything else the
/// ordinary one, zoomed to unless a distance before is remembered.
struct CameraModeChange {
    int mode = 0;
    bool animate = false;
};
[[nodiscard]] inline CameraModeChange seatCameraMode(bool seated, const SeatRecord* seat, bool remembering) {
    if (seated && seat) {
        if (seat->flags & kSeatFlagFixedCamera) return {1, true};
        if (seat->flagsB & kSeatFlagBCameraZooms) return {0, false};
    }
    return {0, !remembering};
}

}  // namespace wowee::game::vehicle

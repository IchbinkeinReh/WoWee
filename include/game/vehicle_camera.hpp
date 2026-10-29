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
        // Not begun yet: one set to start after a delay holds where it is
        // until then (camera_smooth::Plan::delay).
        if (static_cast<int32_t>(nowMs - startMs) < 0) {
            value = from;
            return;
        }
        const float t = static_cast<float>(nowMs - startMs) / static_cast<float>(endMs - startMs);
        if (1.0f < t) {
            startMs = endMs;
            value = to;
            return;
        }
        value = (to - from) * cameraEase(t) + from;
    }
    /// Where it is going: what it eases to, or is at once it has.
    [[nodiscard]] float target() const { return startMs == endMs ? value : to; }
    [[nodiscard]] bool easing() const { return startMs != endMs; }
    /// Turned by the mouse while it eases (0x005ffc20 the pitch: +0x234,
    /// +0x230, +0x120): where it eased from, where it goes and where it is
    /// each moved by the turn and held to lo..hi. It keeps easing - the turn
    /// is added to the ease, neither lost to it nor ending it.
    void shift(float delta, float lo, float hi) {
        const auto held = [lo, hi](float v) { return v < lo ? lo : (v < hi ? v : hi); };
        value = held(value + delta);
        if (!easing()) return;
        from = held(from + delta);
        to = held(to + delta);
    }
    /// Zoomed by the wheel while it eases (0x006000e0 cases 0/1 move +0x1e8
    /// alone while +0x98 0x4000000 is set): only where it goes moves, and it
    /// eases there from where it set out in the time left. At rest it is put
    /// there.
    void retarget(float target) {
        if (easing()) to = target;
        else value = target;
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

/// How far the camera's own model shows as the camera closes on it
/// (Camera.cpp 0x00606f90 -> 0x004f8660): opaque from `farDistance` out,
/// gone at `nearDistance` and in, evenly between (0x008ca080 over 0..255).
/// `distance` is the camera's from its unit less its near clip. The
/// ordinary bounds are 0.0027778 and 1.8315 yards; a vehicle row with
/// Flags 0x80000 gives its own, CameraFadeDistScalarMin/Max times its
/// model's radius, measured from the model's centre.
inline constexpr float kCameraFadeNear = 0.0027777778f;
inline constexpr float kCameraFadeFar = 1.8315f;  // 0x3fea6e99
inline constexpr uint32_t kVehicleFlagCameraFade = 0x80000;
[[nodiscard]] inline float cameraFadeAlpha(float distance, float nearDistance, float farDistance) {
    if (distance >= farDistance) return 1.0f;
    if (distance <= nearDistance) return 0.0f;
    return (distance - nearDistance) / (farDistance - nearDistance);
}

/// The fade's far bound as the camera holds it (0x00606f90): a third of it
/// at the barber shop (DAT_00bd19b8); and looked at steeply from above - its
/// pitch under -1.1868238 rad (68 degrees down) and its distance short of
/// its height over its unit (+0x128) - over the square of (pitch + pi/2) *
/// 2.604353: as it was at 68 degrees, ever wider straight down.
/// `distance` is the one the fade reads, before its near clip.
[[nodiscard]] inline float cameraFadeFarBound(float farDistance, bool barberShop, float distance, float pivotHeight,
                                              float pitchRad) {
    if (barberShop) farDistance *= 0.33333334f;
    if (distance < pivotHeight && pitchRad < -1.1868238f) {
        const float f = (-1.5707964f - pitchRad) * -2.604353f;
        if (std::fabs(f) >= 2.3841858e-07f) farDistance /= f * f;
    }
    return farDistance;
}

/// The vehicle camera's pull-in (0x00759580): the point the camera looks at
/// - the seat's CameraOffset about the unit, chased - is looked for from the
/// vehicle's place raised by its height - a non-player's capped at
/// 2.0277777 - less a sixth (0x00718080), one yard past it, 200 at most. A hit at a
/// share nearer than the share held snaps the held share in; a farther one,
/// or none, keeps the held share, which eases back out to 1 at 2.5
/// (0x00759d80, chaseStep). The point goes to that share less a yard; a
/// hit within a yard puts it at the vehicle's point.
[[nodiscard]] inline float vehicleCameraHeight(float modelHeight) {
    return (modelHeight > 2.0277777f ? 2.0277777f : modelHeight) - 0.16666667f;
}
struct CameraPullIn {
    float held = 1.0f;      ///< +0x44
    float velocity = 0.0f;  ///< +0x48
};
/// `hitShare` is where along the way something was hit, none where nothing.
[[nodiscard]] inline glm::vec3 vehicleCameraPulledIn(const glm::vec3& origin, const glm::vec3& point,
                                                    std::optional<float> hitShare, CameraPullIn& pull) {
    const glm::vec3 way = point - origin;
    const float length = std::sqrt(way.x * way.x + way.y * way.y + way.z * way.z);
    if (length <= 1e-05f) return point;
    const glm::vec3 dir = way / length;
    const float reach = std::min(length + 1.0f, 200.0f);
    float share = pull.held;
    if (hitShare) {
        if (*hitShare < pull.held) {
            pull.held = *hitShare;
            pull.velocity = 0.0f;
            share = *hitShare;
        }
        const float along = reach * share - 1.0f;
        if (along <= 1e-05f) return origin;
        return origin + dir * along;
    }
    return origin + dir * (reach * share - 1.0f);
}
/// Each step, the held share back out toward 1 (0x00759d80).
inline void easeCameraPullIn(CameraPullIn& pull, float dt) { chaseStep(pull.held, pull.velocity, 1.0f, 2.5f, dt); }


/// VehicleCamera_C's mode (+0xa4): waiting to enter, entering, seated,
/// waiting to leave, leaving; Off ends it (0x0075af40 case 5).
enum class CameraMode : uint8_t { Off = 0, EnterWait = 1, Entering = 2, Seated = 3, ExitWait = 4, Exiting = 5 };
struct CameraModeClock {
    CameraMode mode = CameraMode::Off;
    uint32_t startMs = 0;  ///< +0xa8
    uint32_t ms = 0;       ///< +0xac
};

/// The mode a change of seat starts the camera in (0x0075aac0): a wait for
/// its delay where there is one; else leaving for its time - or off, out of
/// any seat with no final time - and entering for its time - or seated, in
/// its seat with no final time.
[[nodiscard]] inline CameraModeClock cameraModeAtChange(SeatState first, const CameraTiming& t, uint32_t nowMs) {
    const bool leaving = first == SeatState::None || first == SeatState::ExitDelay || first == SeatState::Exiting;
    const bool settled = t.durationMs == 0 || !t.fixed;
    if (t.delayMs > 0) return {leaving ? CameraMode::ExitWait : CameraMode::EnterWait, nowMs, t.delayMs};
    if (leaving) {
        if (first == SeatState::None && settled) return {CameraMode::Off, nowMs, 0};
        return {CameraMode::Exiting, nowMs, t.durationMs};
    }
    if (first == SeatState::Seated && settled) return {CameraMode::Seated, nowMs, 0};
    return {CameraMode::Entering, nowMs, t.durationMs};
}

/// Each step, a mode whose time is up moves on (0x0075af40): either wait to
/// entering - for the final time, else a second, less what it ran over -
/// entering to seated, leaving to off.
inline void stepCameraMode(CameraModeClock& c, uint32_t nowMs, bool fixed, uint32_t durationMs) {
    if (c.mode == CameraMode::Off || c.mode == CameraMode::Seated) return;
    const int32_t over = static_cast<int32_t>(nowMs - c.ms - c.startMs);
    if (over < 0) return;
    c.startMs = nowMs;
    switch (c.mode) {
        case CameraMode::EnterWait:
        case CameraMode::ExitWait: {
            const uint32_t whole = fixed ? durationMs : 1000u;
            c.mode = CameraMode::Entering;
            c.ms = whole > static_cast<uint32_t>(over) ? whole - static_cast<uint32_t>(over) : 0u;
            break;
        }
        case CameraMode::Entering:
            c.mode = CameraMode::Seated;
            c.ms = 0;
            break;
        case CameraMode::Exiting:
            c.mode = CameraMode::Off;
            c.ms = 0;
            break;
        default:
            break;
    }
}

/// How far along its mode the camera is (0x007598f0): 1 seated; the
/// passenger's own way in (or out) where it is on it and the seat gives no
/// camera time of its own (FlagsB 0x40 in, 0x80 out); else its time, 1 with
/// none.
[[nodiscard]] inline float cameraModeProgress(const CameraModeClock& c, uint32_t nowMs, SeatState passenger,
                                              const SeatRecord* seat, float passengerProgress) {
    if (c.mode == CameraMode::Seated) return 1.0f;
    const bool entering = c.mode == CameraMode::EnterWait || c.mode == CameraMode::Entering;
    const bool onItsWay = entering ? passenger == SeatState::Entering : passenger == SeatState::Exiting;
    const uint32_t own = entering ? kSeatFlagBEnterCameraTimes : kSeatFlagBExitCameraTimes;
    if (onItsWay && (!seat || (seat->flagsB & own) == 0)) return passengerProgress;
    if (c.ms == 0) return 1.0f;
    const float t = static_cast<float>(nowMs - c.startMs) / static_cast<float>(c.ms);
    return std::clamp(t, 0.0f, 1.0f);
}

/// Whether the camera springs to its point on the way rather than going
/// evenly (0x00759d80): FlagsB 0x20000 entering, 0x40000 leaving; and the
/// spring's rate, stiffer as the time runs out.
[[nodiscard]] inline bool cameraSpringsOnTheWay(CameraMode mode, const SeatRecord* seat) {
    const uint32_t flag = mode == CameraMode::Entering ? 0x20000u : 0x40000u;
    return seat && (seat->flagsB & flag) != 0;
}
[[nodiscard]] inline float cameraWaySpringRate(uint32_t modeMs, float t) {
    const float left = 1.0f - t;
    return (1.0f / (static_cast<float>(modeMs) * 0.001f + 0.001f)) * 4.0f + 1.0f / (left * left);
}

/// The vehicle camera's facing (0x0075a1c0): the watched unit's facing in
/// the world - within a half turn of the last - chased at the seat's 20 /
/// CameraFacingChaseRate while waiting to enter, entering or seated; and
/// the facing the camera turns by: seated that one, waiting the one at the
/// change, on the way in or out from there to it by the camera's ease
/// (0x005fffd0) - or sprung, as the point is (FlagsB 0x20000 in, 0x40000
/// out).
struct CameraFacing {
    float start = 0.0f;           ///< +0x7c
    float out = 0.0f;             ///< +0x80
    float target = 0.0f;          ///< +0x88
    float spring = 0.0f;          ///< +0x8c
    float springVelocity = 0.0f;  ///< +0x90
    float chase = 0.0f;           ///< +0x94
    float chaseVelocity = 0.0f;   ///< +0x98
    bool holdsYaw = false;        ///< +0x8 0x2: the seat's FlagsB 0x1, set at the change
    bool follows = false;         ///< +0x8 0x4: the yaw held this step
    /// A change of seat starts over from where it faces (0x0075aac0); a
    /// new camera from the unit's facing (0x0075b1a0).
    void restart() {
        start = target = chase = spring = out;
        springVelocity = 0.0f;
    }
    void restartAt(float facing) {
        out = facing;
        restart();
    }
};
/// Whether a seat's FlagsB 0x1 holds the camera's yaw (0x00759880, +0x8
/// 0x2, at the change): waiting to enter, entering or seated.
[[nodiscard]] inline bool seatHoldsCameraYaw(CameraMode mode, const SeatRecord* seat) {
    const bool seatModes = mode == CameraMode::EnterWait || mode == CameraMode::Entering || mode == CameraMode::Seated;
    return seatModes && seat && (seat->flagsB & 0x1u) != 0;
}
/// One step; how far the camera turns with it (0x00604490 adds the facing
/// to the camera's yaw). `snap` is +0x8 0x20, `t` the mode's progress,
/// `keysTurn` the turn keys held without the mouse steering (the input's
/// 0x300 without 0x2000001), `freeLook` the camera's own yaw held
/// (+0xac): the facing is not added then. Where the seat holds the yaw
/// and the keys do not turn, seated the turn is taken back (0x005fe5f0)
/// and on the way in the facing is held where it was.
inline float stepCameraFacing(CameraFacing& f, float facing, CameraMode mode, const SeatRecord* seat, float t,
                              uint32_t modeMs, bool snap, float dt, bool keysTurn, bool freeLook) {
    constexpr float kPi = 3.1415927f;
    const float before = f.target;
    float want = facing;
    while (want + kPi < before) want += 2.0f * kPi;
    while (before < want - kPi) want -= 2.0f * kPi;
    f.target = want;
    const bool seatModes = mode == CameraMode::EnterWait || mode == CameraMode::Entering || mode == CameraMode::Seated;
    if (seat && seatModes) {
        if (const auto rate = seatChaseRate(seat->cameraFacingChaseRate)) {
            if (!snap) {
                chaseStep(f.chase, f.chaseVelocity, f.target, *rate, dt);
                f.target = f.chase;
            } else {
                f.chase = f.target;
                f.chaseVelocity = 0.0f;
            }
        }
    }
    const float prevOut = f.out;
    switch (mode) {
        case CameraMode::Seated:
            f.out = f.target;
            break;
        case CameraMode::EnterWait:
        case CameraMode::ExitWait:
            f.out = f.start;
            break;
        default:
            if (!cameraSpringsOnTheWay(mode, seat)) {
                f.out = (f.target - f.start) * cameraEase(t) + f.start;
            } else if (t >= 0.999999f) {
                f.spring = f.target;
                f.springVelocity = 0.0f;
                f.out = f.spring;
            } else {
                float w = f.target;
                while (w + kPi < f.spring) w += 2.0f * kPi;
                while (f.spring < w - kPi) w -= 2.0f * kPi;
                chaseStep(f.spring, f.springVelocity, w, cameraWaySpringRate(modeMs, t), dt);
                f.out = f.spring;
            }
            break;
    }
    float turned = freeLook ? 0.0f : f.out - prevOut;
    if (f.holdsYaw && seatModes) {
        if (f.follows && !freeLook) {
            if (mode != CameraMode::Seated) {
                f.out = prevOut;
                f.spring = prevOut;
                f.target = before;
            }
            turned = 0.0f;
        }
        f.follows = !keysTurn;
    } else {
        f.follows = false;
    }
    return turned;
}

/// Whom the camera watches and the frame it is held in (0x0075a930): in a
/// control seat (Flags 0x800), on the way in or there, the vehicle - its
/// point and facing the vehicle's, its chase in the vehicle's own frame;
/// otherwise the unit itself.
[[nodiscard]] inline bool cameraWatchesVehicle(bool leaving, const SeatRecord* seat) {
    return !leaving && seat && (seat->flags & kSeatFlagCanControl) != 0;
}

/// A unit's place on its transport (its movement's raw position, vfunc
/// +0x30) taken to the world (0x004f4460): its offset - x ahead, y to the
/// left - turned by the transport's facing; here on the render axes, `renderYaw` as
/// seatCameraOffset takes it.
[[nodiscard]] inline glm::vec3 transportOffsetAbout(const glm::vec3& offset, float renderYaw) {
    const float c = std::cos(renderYaw);
    const float s = std::sin(renderYaw);
    return {offset.x * c + offset.y * s, offset.x * s - offset.y * c, offset.z};
}

}  // namespace wowee::game::vehicle

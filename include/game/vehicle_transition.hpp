#pragma once

// A passenger's way into and out of its seat, as the passenger component
// (CGUnit_C +0xf60) runs it: its state (+0x14), when it began and ends
// (+0x34, +0x38), the arc it flies (+0xc4 gravity, +0xc8 height scale) and
// how far along it is (+0x48).
//
//   0 none, 1 waiting to enter, 2 entering, 3 seated, 4 waiting to exit,
//   5 exiting.
//
// A change of transport picks the first state (0x00749aa0), each frame
// steps it (0x0074af70), a new state plans its time and arc (0x0074a200),
// and the matrix is drawn from it (0x0074a7f0).

#include "game/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::game::vehicle {

enum class SeatState : uint8_t {
    None = 0,
    EnterDelay = 1,
    Entering = 2,
    Seated = 3,
    ExitDelay = 4,
    Exiting = 5,
};

/// VehicleSeat Flags (+4) the moves read.
inline constexpr uint32_t kSeatFlagHasEnterAnim = 0x1;            ///< 0x00749aa0, 0x00747b20
inline constexpr uint32_t kSeatFlagExitAnimVoluntary = 0x8;       ///< 0x0074bb60
inline constexpr uint32_t kSeatFlagEnterEaseIn = 0x20;            ///< 0x00747d70
inline constexpr uint32_t kSeatFlagEnterEaseOut = 0x40;
inline constexpr uint32_t kSeatFlagExitEaseIn = 0x80;
inline constexpr uint32_t kSeatFlagExitEaseOut = 0x100;
inline constexpr uint32_t kSeatFlagExitAnimForced = 0x8000;       ///< 0x0074bb60
inline constexpr uint32_t kSeatFlagHasVehicleRideAnim = 0x20000;  ///< 0x007489c0
inline constexpr uint32_t kSeatFlagHasExitAnimEnd = 0x200000;     ///< 0x0074b9c0
/// FlagsB (+0xb4): a passenger leaving it always leaves by its arc
/// (0x0074b840).
inline constexpr uint32_t kSeatFlagBExitAlwaysAnimated = 0x1000000;

/// AnimationData ids the exit's end picks among (0x007489c0, 0x0074b9c0).
inline constexpr uint32_t kAnimJumpEnd = 39;
inline constexpr uint32_t kAnimFall = 40;
inline constexpr uint32_t kAnimSwimIdle = 41;
inline constexpr uint32_t kAnimJumpLandRun = 187;
inline constexpr int32_t kNoAnimation = 0x1fa;

/// The key bone a VehicleSeat vehicle animation plays on (0x00756f80,
/// 0x007577e0): past 0x22 it is 0x1a, which is the whole model.
inline constexpr int32_t kWholeModelBone = 0x1a;
[[nodiscard]] inline int32_t vehicleAnimBone(int32_t column) {
    return static_cast<uint32_t>(column) > 0x22u ? kWholeModelBone : column;
}

/// VehicleSeat Flags for the vehicle's own animations as a passenger gets in
/// or out (0x0074bcb0, 0x0074be10, 0x00748070).
inline constexpr uint32_t kSeatFlagVehicleEnterAnim = 0x400000;
inline constexpr uint32_t kSeatFlagVehicleExitAnimVoluntary = 0x40000;
inline constexpr uint32_t kSeatFlagVehicleExitAnimForced = 0x80000;
inline constexpr uint32_t kSeatFlagEnterAnimDelayed = 0x8000000;
inline constexpr uint32_t kSeatFlagExitAnimDelayed = 0x10000000;

/// The vehicle animation a monster move into or out of a seat waits on
/// (0x0074bcb0): in, VehicleEnterAnim on VehicleEnterAnimBone with Flags
/// 0x400000; out, VehicleExitAnim on VehicleExitAnimBone with Flags 0x40000
/// where the unit leaves of its own will (movement flags2 0x40), else
/// 0x80000; below 0x1fa either way. The wait is the seat's
/// VehicleEnterAnimDelay / VehicleExitAnimDelay where Flags 0x8000000 /
/// 0x10000000 give one, else until the animation's "$VG" / "$VT" event for
/// the seat; three seconds at most (0x00748070).
struct VehicleSeatAnim {
    uint32_t animation = 0;
    int32_t bone = 0;
    bool timed = false;
    uint32_t waitMs = 3000;
    uint32_t eventBase = 0;  ///< "$VG0" or "$VT0" as a word; the seat's is plus (seat + 1) << 24
};
[[nodiscard]] inline std::optional<VehicleSeatAnim> vehicleSeatAnim(const SeatRecord& seat, bool entering,
                                                                    bool voluntary) {
    const uint32_t flag = entering ? kSeatFlagVehicleEnterAnim
                                   : (voluntary ? kSeatFlagVehicleExitAnimVoluntary : kSeatFlagVehicleExitAnimForced);
    const int32_t anim = entering ? seat.vehicleEnterAnim : seat.vehicleExitAnim;
    if ((seat.flags & flag) == 0 || static_cast<uint32_t>(anim) >= static_cast<uint32_t>(kNoAnimation))
        return std::nullopt;
    VehicleSeatAnim a;
    a.animation = static_cast<uint32_t>(anim);
    a.bone = vehicleAnimBone(entering ? seat.vehicleEnterAnimBone : seat.vehicleExitAnimBone);
    a.timed = (seat.flags & (entering ? kSeatFlagEnterAnimDelayed : kSeatFlagExitAnimDelayed)) != 0;
    if (a.timed) {
        const float delay = entering ? seat.vehicleEnterAnimDelay : seat.vehicleExitAnimDelay;
        const auto ms = static_cast<uint32_t>(static_cast<int64_t>(std::lround(delay * 1000.0f)));
        a.waitMs = ms > 3000u ? 3000u : ms;
    }
    a.eventBase = entering ? 0x30475624u : 0x30545624u;
    return a;
}

/// Whether a change of transport is moved through (0x0074b840): the unit's
/// +0xa30 0x20000000 - set while a monster move (0x0073c8e0) or an 0xED
/// movement packet (0x007188f0) or the active mover's own move with flags2
/// 0x2000 (0x006ecf80) sets it - or, leaving, its old seat's FlagsB
/// 0x1000000.
[[nodiscard]] inline bool changeIsAnimated(bool unitAnimates, bool leaving, const SeatRecord* oldSeat) {
    return unitAnimates || (leaving && oldSeat && (oldSeat->flagsB & kSeatFlagBExitAlwaysAnimated) != 0);
}

/// The exit animations' flag (0x0074bb60): 0x8 where the unit's movement
/// flags2 have 0x40 - it is leaving of its own will - else 0x8000.
[[nodiscard]] inline uint32_t exitAnimFlag(bool voluntary) {
    return voluntary ? kSeatFlagExitAnimVoluntary : kSeatFlagExitAnimForced;
}

/// 0x00749aa0: the state a change of transport starts. Into a seat: 3 at
/// once, unless moved through to a seat with Flags 0x1 and an EnterSpeed -
/// then 1 where it has an EnterPreDelay, else 2. Out of one: 0 at once,
/// unless moved through from a seat with the exit animations' flag and an
/// ExitSpeed - then 4 where it has an ExitPreDelay, else 5.
[[nodiscard]] inline SeatState firstState(bool entering, const SeatRecord* seat, bool animated, bool voluntary) {
    if (entering) {
        if (!animated || !seat || (seat->flags & kSeatFlagHasEnterAnim) == 0 || seat->enterSpeed <= 0.0001f)
            return SeatState::Seated;
        return seat->enterPreDelay <= 0.0001f ? SeatState::Entering : SeatState::EnterDelay;
    }
    if (!animated || !seat || (seat->flags & exitAnimFlag(voluntary)) == 0 || !(seat->exitSpeed > 0.0001f))
        return SeatState::None;
    return seat->exitPreDelay <= 0.0001f ? SeatState::Exiting : SeatState::ExitDelay;
}

/// A wait's length (0x0074a200, states 1 and 4): the pre-delay, no more
/// than ten seconds, in ms.
[[nodiscard]] inline uint32_t preDelayMs(float seconds) {
    return static_cast<uint32_t>(std::lround(std::min(seconds, 10.0f) * 1000.0f));
}

/// The arc a state 2 or 5 flies (0x0074a200).
struct Arc {
    uint32_t durationMs = 0;
    float gravity = 0.0f;      ///< +0xc4
    float heightScale = 1.0f;  ///< +0xc8
};

/// 0x0074a200 for states 2 and 5. `distance` is how far it goes. Entering
/// a vehicle it is timed by the seat's EnterSpeed less the vehicle's speed
/// along the way (`closingSpeed`, EnterSpeed - velocity . direction); else
/// by the ExitSpeed - which the client reads there entering too. Either is
/// held between the min and max durations (the max at most 10 s, 1.5 where
/// it has none). The gravity is the seat's, 0.1 where it has none but an
/// arc height asks for one, and the arc's peak (g T^2 / 8) is scaled into
/// the min and max arc heights.
[[nodiscard]] inline Arc planArc(const SeatRecord& seat, bool entering, float distance,
                                 std::optional<float> enterSpeedAlong) {
    Arc arc;
    arc.gravity = entering ? seat.enterGravity : seat.exitGravity;
    const float minDuration = entering ? seat.enterMinDuration : seat.exitMinDuration;
    const float maxColumn = entering ? seat.enterMaxDuration : seat.exitMaxDuration;
    const float minArc = entering ? seat.enterMinArcHeight : seat.exitMinArcHeight;
    const float maxArc = entering ? seat.enterMaxArcHeight : seat.exitMaxArcHeight;
    const float maxDuration = maxColumn >= 0.0001f ? std::min(maxColumn, 10.0f) : 1.5f;
    // 0x00497a90: not at or above the min the min, at or above the max the max.
    auto hold = [&](float t) { return !(minDuration <= t) ? minDuration : (t < maxDuration ? t : maxDuration); };
    float seconds = maxDuration;
    if (entering && enterSpeedAlong) {
        if (distance <= 0.0001f) {
            seconds = 0.0f;
        } else if (*enterSpeedAlong > 0.0001f) {
            seconds = hold(distance / *enterSpeedAlong);
        }
    } else {
        seconds = hold(distance / seat.exitSpeed);
    }
    arc.durationMs = static_cast<uint32_t>(std::max(0L, std::lround(seconds * 1000.0f)));
    if (std::fabs(arc.gravity) < 0.0001f && (minArc > 0.0f || maxArc < 0.0f)) arc.gravity = 0.1f;
    arc.heightScale = 1.0f;
    if (seconds > 0.0001f) {
        const float peak = arc.gravity * seconds * seconds * 0.125f;
        if (peak < minArc) arc.heightScale = minArc / peak;
        else if (maxArc < peak) arc.heightScale = maxArc / peak;
    }
    return arc;
}

/// 0x00747d70: how far along a state is, 0..1, by the time; 1 where it
/// lasts under 0.1 ms. Entering eases by the seat's Flags 0x20 (in, t^2)
/// and 0x40 (out, 1 - (1-t)^2), both together a half cosine; exiting by
/// 0x80 and 0x100.
[[nodiscard]] inline float progress(SeatState state, uint32_t seatFlags, uint32_t startMs, uint32_t endMs,
                                    uint32_t nowMs) {
    const float span = static_cast<float>(static_cast<int32_t>(endMs - startMs)) * 0.001f;
    if (span < 0.0001f) return 1.0f;
    float p = static_cast<float>(static_cast<int32_t>(nowMs - startMs)) * 0.001f / span;
    p = std::clamp(p, 0.0f, 1.0f);
    uint32_t easeIn = 0, easeOut = 0;
    if (state == SeatState::Entering) {
        easeIn = kSeatFlagEnterEaseIn;
        easeOut = kSeatFlagEnterEaseOut;
    } else if (state == SeatState::Exiting) {
        easeIn = kSeatFlagExitEaseIn;
        easeOut = kSeatFlagExitEaseOut;
    } else {
        return p;
    }
    const bool in = (seatFlags & easeIn) != 0;
    const bool out = (seatFlags & easeOut) != 0;
    if (in && out) return 0.5f - std::cos(p * 3.1415927f) * 0.5f;
    if (in) return p * p;
    if (out) return 1.0f - (1.0f - p) * (1.0f - p);
    return p;
}

/// 0x0074a7f0: the arc's height over the straight way at a progress -
/// g t (T - t) / 2, scaled, t the progress of the time T; none where the
/// gravity is under 0.0001.
[[nodiscard]] inline float arcHeight(const Arc& arc, float p) {
    if (std::fabs(arc.gravity) <= 0.0001f) return 0.0f;
    const float total = static_cast<float>(arc.durationMs) * 0.001f;
    const float t = total * p;
    const float half = arc.gravity * t * 0.5f;
    return (half * total - half * t) * arc.heightScale;
}

/// 0x00747a30: the facing drawn while it flies - the unit's facing, turned
/// by whole turns to within half a turn of the facing drawn before, eased
/// from the facing it started at by its progress.
[[nodiscard]] inline float flightFacing(float startFacing, float drawnFacing, float unitFacing, float p) {
    constexpr float kPi = 3.1415927f;
    constexpr float kTwoPi = 6.2831855f;
    float f = unitFacing;
    while (f + kPi < drawnFacing) f += kTwoPi;
    while (drawnFacing < f - kPi) f -= kTwoPi;
    return f * p + (1.0f - p) * startFacing;
}

/// What the passenger plays in a state (0x00747b20): entering, with Flags
/// 0x1, EnterAnimStart until it has played (+0x10 0x2), then EnterAnimLoop;
/// seated, with 0x2, the ride animations; exiting, with the exit animations'
/// flag, ExitAnimStart and ExitAnimLoop. Nothing where it has none.
struct SeatAnimation {
    std::optional<uint32_t> start;
    std::optional<uint32_t> loop;
};
[[nodiscard]] inline std::optional<SeatAnimation> seatAnimation(SeatState state, const SeatRecord& seat,
                                                                bool voluntary) {
    int32_t start = -1, loop = -1;
    switch (state) {
        case SeatState::EnterDelay:
        case SeatState::Entering:
            if ((seat.flags & kSeatFlagHasEnterAnim) == 0) return std::nullopt;
            start = seat.enterAnimStart;
            loop = seat.enterAnimLoop;
            break;
        case SeatState::Seated:
            if ((seat.flags & kSeatFlagHasRideAnim) == 0) return std::nullopt;
            start = seat.rideAnimStart;
            loop = seat.rideAnimLoop;
            break;
        case SeatState::ExitDelay:
        case SeatState::Exiting:
            if ((seat.flags & exitAnimFlag(voluntary)) == 0) return std::nullopt;
            start = seat.exitAnimStart;
            loop = seat.exitAnimLoop;
            break;
        default:
            return std::nullopt;
    }
    SeatAnimation out;
    if (start >= 0 && start != kNoAnimation) out.start = static_cast<uint32_t>(start);
    if (loop >= 0 && loop != kNoAnimation) out.loop = static_cast<uint32_t>(loop);
    if (!out.start && !out.loop) return std::nullopt;
    return out;
}

/// A change of state that keeps the animation started before (0x007489c0
/// clears +0x10 0x6 - the start played - but from 1 to 2 and out of 4).
[[nodiscard]] inline bool keepsAnimation(SeatState from, SeatState to) {
    return (from == SeatState::EnterDelay && to == SeatState::Entering) || from == SeatState::ExitDelay;
}

/// What the unit plays as it lands out of state 5 (0x007489c0 ->
/// 0x0074b9c0): the seat's ExitAnimEnd - JumpEnd turned JumpLandRun while
/// it moves - where the seat has Flags 0x200000 and one; else Fall while it
/// falls, SwimIdle while it swims, nothing while it flies, else the
/// ExitAnimEnd where it has one. `oneShot` says it plays once.
struct LandingAnimation {
    uint32_t id = 0;
    bool oneShot = false;
};
[[nodiscard]] inline std::optional<LandingAnimation> landingAnimation(const SeatRecord* seat, bool moving,
                                                                      bool falling, bool swimming, bool flying) {
    int32_t anim = -1;
    if (seat) {
        anim = seat->exitAnimEnd;
        if (anim == static_cast<int32_t>(kAnimJumpEnd) && moving) anim = static_cast<int32_t>(kAnimJumpLandRun);
    }
    if (!seat || (seat->flags & kSeatFlagHasExitAnimEnd) == 0 || anim == -1) {
        if (falling) return LandingAnimation{kAnimFall, false};
        if (swimming) return LandingAnimation{kAnimSwimIdle, false};
        if (flying || anim == -1) return std::nullopt;
    }
    return LandingAnimation{static_cast<uint32_t>(anim), true};
}

}  // namespace wowee::game::vehicle

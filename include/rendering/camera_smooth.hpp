#pragma once

// The camera's follow smoothing, cameraSmoothStyle (Interface > Camera
// Following Style). The client's default is "4", Smarter: while the
// character moves, strafes or turns, the camera swings back behind it and its
// pitch back into a band; standing still, it is left where it was put.
//
// The client decides this when the movement controls change (0x00602760, from
// the control starts and stops 0x005fa170 / 0x005fa450): which state the
// controls make, that state's delay and factor for the style
// (cameraSmooth<Style><State>Delay/Factor, 0x005fd910), and per part the
// view's (cameraSmoothViewData<Style><Part>). A part with a factor eases to
// its target over the angle at its smooth speed times the factor; a part
// whose factor is 0 has its ease stopped where it is. All parts eased take
// the longest one's time, held to cameraSmoothTimeMin..Max.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace camera_smooth {

/// The states, in the client's order (its bit for each is 1 << the value; the
/// highest bit set wins).
enum class State { Idle = 0, Stop = 1, Track = 2, Move = 3, Strafe = 4, Turn = 5, Fear = 6 };

/// What the controls make (0x00602760): moving forward, back or on autorun
/// or with both buttons is Move; a strafe Strafe; a turn key, or the right
/// button's mouselook, Turn.
///
/// Fear is the camera's +0x98 0x1000, which 0x00606f90 sets through
/// 0x00602eb0 every frame the unit it looks at is carried by a server move
/// spline - fear, charm, a charge, a jump - other than a flight path's
/// (the unit's flag 0x100000) and one flagged 0x400. It outranks the rest.
/// Track is +0x98 0x100, which the click-to-move controls set (0x00602dc0
/// from 0x005fa170 / 0x005fa450 on their bits 0x1e00000); this client has
/// no click-to-move, so nothing sets it, but it ranks where the client has
/// it: under every control of the player's own.
inline State stateFor(bool move, bool strafe, bool turnKeys, bool mouselook, bool track = false,
                      bool fear = false) {
    if (fear) return State::Fear;
    if (turnKeys || mouselook) return State::Turn;
    if (strafe) return State::Strafe;
    if (move) return State::Move;
    if (track) return State::Track;
    return State::Idle;
}

/// cameraSmoothSmarter<State>Delay / Factor's defaults (0x005fd910).
struct DelayFactor {
    float delay = 0.0f;
    float factor = 0.0f;
};
inline DelayFactor smarter(State s) {
    switch (s) {
        case State::Track:
        case State::Fear: return {0.4f, 10.0f};
        case State::Move:
        case State::Strafe:
        case State::Turn: return {0.0f, 1.0f};
        default: return {0.0f, 0.0f};
    }
}

// The rest of the defaults: cameraSmoothViewDataSmarterPitch and Yaw are
// delay 0 factor 1 (Distance factor 0, never eased); cameraYawSmoothSpeed
// 180 and cameraPitchSmoothSpeed 45 degrees a second; cameraPitchSmoothMin
// and Max 0 and 30, cameraYawSmoothMin and Max both 0; cameraSmoothTimeMin
// and Max 0.1 and 2 seconds.
constexpr float kYawSpeed = 180.0f;
constexpr float kPitchSpeed = 45.0f;
constexpr float kPitchMin = 0.0f;
constexpr float kPitchMax = 30.0f;
constexpr float kTimeMin = 0.1f;
constexpr float kTimeMax = 2.0f;
/// The eases' own threshold, 0.001 radians (0x00601190, 0x00601410).
constexpr float kAtRest = 0.001f * 57.29578f;
/// How far the way a spline-carried character faces has to turn to be a new
/// direction for the Fear state. The client sets the unit's facing to its
/// spline's at once, so any change is a new one there; this client turns the
/// character toward it over a few frames, and taking each of those small
/// steps as new restarted the state's 0.4 s delay every frame, so the camera
/// never moved while the character turned.
constexpr float kFearNewFacing = 10.0f;

/// What to do with the camera when the controls change.
struct Plan {
    bool stopYaw = false;    ///< a factor of 0: the yaw ease stops where it is
    bool stopPitch = false;
    bool easeYaw = false;    ///< ease the yaw from the facing to yawTo
    float yawTo = 0.0f;
    bool easePitch = false;  ///< ease the pitch to pitchTo
    float pitchTo = 0.0f;
    float seconds = 0.0f;    ///< over this long, both
    /// Starting this long from now (the state's delay plus the part's,
    /// 0x00602760; each ease starts at now + delay, 0x00601410).
    float delay = 0.0f;
};

/// `yawFromFacing` the camera's yaw off the character's facing and `pitch`
/// its pitch, both in degrees as /gps gives them (the pitch down from level).
/// `freelook`: a mouse button holds the camera, which nothing here moves
/// (0x00602680, 0x005ffeb0 on +0x98 1). `pitchFree`: swimming or flying,
/// where the pitch is the mover's and is left alone (0x005ffeb0 on
/// 0x2200000).
inline Plan plan(State state, float yawFromFacing, float pitch, bool freelook, bool pitchFree) {
    Plan p;
    if (freelook) return p;
    const float factor = smarter(state).factor;
    // The views' own delays (cameraSmoothViewDataSmarterYaw/Pitch) are 0.
    p.delay = smarter(state).delay;
    // The pitch is smoothed only outside its band, and to the edge it left.
    const bool pitchOut = !pitchFree && (pitch < kPitchMin || pitch > kPitchMax);
    // The yaw's band is 0..0: anything off straight behind is smoothed.
    const float yaw = std::remainder(yawFromFacing, 360.0f);
    const bool yawOut = std::fabs(yaw) >= kAtRest;
    float longest = 0.0f;
    if (pitchOut) {
        if (factor == 0.0f) {
            p.stopPitch = true;
        } else {
            p.pitchTo = std::clamp(pitch, kPitchMin, kPitchMax);
            p.easePitch = true;
            longest = std::max(longest, std::fabs(p.pitchTo - pitch) / kPitchSpeed * factor);
        }
    }
    if (yawOut) {
        if (factor == 0.0f) {
            p.stopYaw = true;
        } else {
            p.yawTo = 0.0f;
            p.easeYaw = true;
            longest = std::max(longest, std::fabs(yaw) / kYawSpeed * factor);
        }
    }
    p.seconds = std::clamp(longest, kTimeMin, kTimeMax);
    return p;
}

/// Whether the Fear state's ease starts afresh: when none was planned yet
/// for this spline, or the way the character faces has turned to a new
/// direction since (kFearNewFacing).
inline bool fearPlansAgain(bool planned, float facingDeg, float plannedFacingDeg) {
    return !planned || std::fabs(std::remainder(facingDeg - plannedFacingDeg, 360.0f)) >= kFearNewFacing;
}

/// Whether the player is carried by a server spline as the camera sees it
/// (0x00606f90): the unit the camera looks at - the player, not through a
/// far sight - on a move spline, and not a flight path's nor a seat's. The
/// active mover does not come into it: fear and charm take the player's
/// control first, and that leaves no mover (0x0072cca0).
inline bool playerOnServerSpline(bool onSpline, bool onTaxi, bool seated, uint64_t cameraTarget, uint64_t player) {
    return onSpline && !onTaxi && !seated && player != 0 && cameraTarget == player;
}

}  // namespace camera_smooth
}  // namespace rendering
}  // namespace wowee

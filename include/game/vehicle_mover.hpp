#pragma once

// Which unit the client moves: the active mover (DAT_00ca1238), the unit its
// keys drive and its movement packets name. It is the player, or - in a
// vehicle's control seat, or possessing - the unit the player looks through
// (PLAYER_FARSIGHT) once the server gives the client control of it
// (SMSG_CLIENT_CONTROL_UPDATE, the unit's +0xa30 0x400).

#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::game::vehicle {

/// What a unit's SMSG_CLIENT_CONTROL_UPDATE does to the active mover
/// (0x0072cca0); nothing where it stays. `cameraTarget` is the unit the
/// camera looks through (the camera's +0x88: the farsight object, else the
/// player); `cameraTargetControlled` says it is a unit the client controls.
[[nodiscard]] inline std::optional<uint64_t> moverAfterControlUpdate(uint64_t unit, bool allow, uint64_t cameraTarget,
                                                                     uint64_t mover, bool cameraTargetControlled) {
    if (unit == cameraTarget) {
        if (allow) return unit;
        if (unit != mover) return std::nullopt;
        return 0;
    }
    if (unit != mover || allow) return std::nullopt;
    // The mover lost control: the camera's unit where the client controls it.
    if (cameraTargetControlled) return cameraTarget;
    return 0;
}

/// The active mover after the player's PLAYER_FARSIGHT changes (0x006e4fd0
/// -> 0x006e2880): the farsight unit where it is in view and controlled,
/// else the player where the player is, else none.
[[nodiscard]] inline uint64_t moverAfterFarsight(uint64_t farsight, bool farsightKnown, bool farsightControlled,
                                                 uint64_t player, bool playerControlled) {
    if (farsight != 0 && farsightKnown && farsightControlled) return farsight;
    return playerControlled ? player : 0;
}

/// Whether CMSG_FAR_SIGHT goes out on that change and with what (0x006e2880,
/// the player's +0x1858 0x1): 1 the first time the farsight object is in
/// view, 0 once it is cleared after that.
[[nodiscard]] inline std::optional<bool> farSightAnnouncement(uint64_t farsight, bool farsightKnown, bool announced) {
    if (farsight != 0) {
        if (farsightKnown && !announced) return true;
        return std::nullopt;
    }
    if (announced) return false;
    return std::nullopt;
}

/// A pitch asked of a unit that pitches over time (movement flags2 0x10,
/// 0x005fb3a0 -> 0x0072d470 -> 0x006ef490 -> 0x006ef7a0): the way it turns
/// (up where the target is above) and how long it pitches at its pitch rate,
/// in ms - the way there wrapped to a half turn (0x004c50c0), over the rate.
struct PitchOverTime {
    bool up = false;
    uint32_t durationMs = 0;
};
[[nodiscard]] inline PitchOverTime pitchOverTime(float current, float target, float pitchRate) {
    constexpr float kPi = 3.14159265f;
    float delta = target - current;
    while (delta > kPi) delta -= 2.0f * kPi;
    while (delta < -kPi) delta += 2.0f * kPi;
    PitchOverTime p;
    p.up = delta > 0.0f;
    if (pitchRate > 0.0f) p.durationMs = static_cast<uint32_t>(std::lround(std::fabs(delta) / pitchRate * 1000.0f));
    return p;
}

/// The keys' pitch: up, down or neither (0x005fb1a0, the input control's
/// 0x400 and 0x800).
[[nodiscard]] inline int aimKeyDirection(bool up, bool down) { return (up ? 1 : 0) - (down ? 1 : 0); }

/// What a full-speed pitcher's start is held for (0x00721ac0 / 0x00721b90):
/// a START_PITCH waits 100 ms and goes out after; a stop before then goes as
/// one MSG_MOVE_SET_PITCH instead, and none where neither the facing nor
/// the pitch moved 0.1 from what was last sent (0x0071ae80).
inline constexpr uint32_t kPitchStartHoldMs = 100;
[[nodiscard]] inline bool pitchUnchangedSinceSent(float facing, float sentFacing, float pitch, float sentPitch,
                                                  bool pitchSent) {
    if (std::fabs(facing - sentFacing) >= 0.1f) return false;
    if (pitchSent && std::fabs(pitch - sentPitch) >= 0.1f) return false;
    return true;
}

/// A unit's move spline as the client keeps it (its movement's +0xbc): from
/// a monster move with a path, until it has run its time or is let go - the
/// client marks one finished with its flags' 0x400 rather than freeing it
/// (0x0098ba90, 0x006eaf50), and a move that comes with 0x400 is done from
/// the start. A cyclic one (0x80000) does not run out.
struct MoveSpline {
    uint32_t startMs = 0;
    uint32_t durationMs = 0;
    bool cyclic = false;
    /// Its MoveSplineFlags: Falling 0x200, Parabolic 0x800, Flying 0x2000
    /// (+0xbc +0x20, read by 0x004f5290, 0x006e9a70, 0x006e9aa0).
    uint32_t flags = 0;
};
[[nodiscard]] inline std::optional<MoveSpline> moveSplineFrom(bool hasPath, uint32_t splineFlags, uint32_t durationMs,
                                                              uint32_t nowMs) {
    if (!hasPath || (splineFlags & 0x400u) != 0) return std::nullopt;
    return MoveSpline{.startMs = nowMs,
                      .durationMs = durationMs,
                      .cyclic = (splineFlags & 0x80000u) != 0,
                      .flags = splineFlags};
}
[[nodiscard]] inline bool moveSplineRunning(const MoveSpline& s, uint32_t nowMs) {
    return s.cyclic || static_cast<int32_t>(nowMs - s.startMs) < static_cast<int32_t>(s.durationMs);
}

/// Whether the mover may not move for its seat (0x0074ba40): its own seat
/// state is on the way in or out (not 0 or 3), or a move of its is held for
/// its vehicle's animation (+0x10 0x200); or - where a driver controls it -
/// the same of the driver's, or the driver, in a seat, asked for a seat
/// change it has not yet had (0x00747fa0's +0x10 0x8).
[[nodiscard]] inline bool moverHeldBySeat(uint8_t moverState, bool moverAnimHeld, uint8_t driverState,
                                          bool driverAnimHeld, bool driverAskedSeatChange) {
    if (moverState != 0 && moverState != 3) return true;
    if (moverAnimHeld) return true;
    if (driverState != 0 && driverState != 3) return true;
    if (driverAnimHeld) return true;
    return driverState != 0 && driverAskedSeatChange;
}

/// SendCast (0x0080ac90): a spell with AttributesEx7 (+0x2c) 0x80 cast by the
/// active mover asks the seated player's seat change (0x00747fa0) and stops
/// the mover (0x0071ae10 -> 0x006ecde0, MSG_MOVE_STOP).
/// That stop leaves the input as it is - its forward/back move still started
/// (+4 0x10000) - so a key held starts nothing again (0x005fae70 restarts
/// only a move not started, or one the other way): the mover stays stopped
/// until the forward and back keys come to rest, which stops and forgets it,
/// or until its keys are let go, as a mover that may not move has them
/// (0x005fbbc0, the seat change's hold among it). At rest is 0x005fae70's
/// count: one for each of autorun (+4 0x1000), the forward key (0x10) and
/// both mouse buttons (0x1 and 0x2), less one for the back key (0x20),
/// coming to 0. Autorun counts as a forward key held, and ends only when
/// it is turned off or a forward or back key or both mouse buttons are
/// pressed (0x005fa170), which then count instead.
[[nodiscard]] constexpr bool forwardBackStopHolds(bool stopped, int forwardKeys, int backKeys, bool moverHeld) {
    return stopped && forwardKeys != backKeys && !moverHeld;
}
inline constexpr uint32_t kAttrEx7PrepareForVehicleControlEnd = 0x80;
[[nodiscard]] inline bool castEndsVehicleControl(uint32_t attrEx7, uint64_t caster, uint64_t mover) {
    return caster != 0 && caster == mover && (attrEx7 & kAttrEx7PrepareForVehicleControlEnd) != 0;
}

/// The keys the mover's movement flags2 hold: 0x1 starts no strafe
/// (0x00988b00) and no rise or dive in water or air (0x009898e0); 0x2 starts
/// no jump (0x009883f0).
struct MoverKeyRestrictions {
    bool noStrafe = false;
    bool noJump = false;
};
[[nodiscard]] inline MoverKeyRestrictions moverKeyRestrictions(uint32_t flags2) {
    return {.noStrafe = (flags2 & 0x1u) != 0, .noJump = (flags2 & 0x2u) != 0};
}

}  // namespace wowee::game::vehicle

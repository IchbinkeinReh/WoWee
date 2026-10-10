#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace body_yaw {

// ============================================================================
// The yaw a unit's body is drawn at, apart from the facing it moves and is
// reported at.
//
// The client keeps the two apart. The facing - the movement facing for the
// player at the controls, the eased orientation for every other unit - is
// copied into the unit's target yaw each frame (+0xaa0, FUN_00735f60), and
// the model is drawn at the body yaw (+0xa94, read by FUN_007156a0), which
// CGUnit's per-frame animation update (FUN_0073dab0) brings round to it:
// sprung across while strafing or starting off, caught up at eight times the
// turn rate while the unit stands, and held while the player turns it, the
// spine and head taking the lag and the body dragged only past a quarter
// turn. Whatever the body has not turned this frame the spine and the head
// look round to; whatever it did turn, standing, is a shuffle of the feet.
//
// Angles are radians, anticlockwise from above, as the facing is.
// ============================================================================

// The movement flag bits FUN_0073dab0 tests (CMovement +0x44).
constexpr uint32_t kForward     = 0x00000001u;
constexpr uint32_t kBackward    = 0x00000002u;
constexpr uint32_t kStrafeLeft  = 0x00000004u;
constexpr uint32_t kStrafeRight = 0x00000008u;
constexpr uint32_t kTurnLeft    = 0x00000010u;
constexpr uint32_t kTurnRight   = 0x00000020u;
constexpr uint32_t kFalling     = 0x00001000u;
constexpr uint32_t kSwimming    = 0x00200000u;
constexpr uint32_t kAscending   = 0x00400000u;
constexpr uint32_t kDescending  = 0x00800000u;
constexpr uint32_t kFlying      = 0x02000000u;

constexpr uint32_t kStrafe = kStrafeLeft | kStrafeRight;
/// Standing still, for the shuffle: no direction, not falling, swimming,
/// rising, sinking or flying (FUN_0073dab0's & 0x2e0100f).
constexpr uint32_t kNotStillMask = 0x02e0100fu;

constexpr float kPi = 3.1415927f;
constexpr float kHalfPi = 1.5707964f;
constexpr float kQuarterPi = 0.7853982f;

/// Into (-pi, pi] (FUN_007156c0).
[[nodiscard]] inline float wrap(float a) noexcept {
    if (a > kPi || a < -kPi) {
        a = std::remainder(a, 2.0f * kPi);
    }
    return a;
}

/// What FUN_0073dab0 keeps between frames: the body yaw (+0xa94), the
/// spring's angular velocity (+0xa98) and the spring's weight (+0xa9c).
struct State {
    float body = 0.0f;
    float velocity = 0.0f;
    float weight = 0.0f;
};

/// The critically damped spring the body is pulled through (FUN_00719660):
/// first held within a quarter turn of the facing, then sprung towards
/// `target` at omega 20 (the Game Programming Gems "SmoothCD" step), then
/// mixed with the target by the weight - none of the spring at weight 0.
inline void spring(State& s, float facing, float target, float dt) noexcept {
    constexpr float kHold = 1.5704823f;
    float f = facing;
    if (s.body > f + kPi) f += 2.0f * kPi;
    else if (s.body < f - kPi) f -= 2.0f * kPi;
    if (f > s.body + kHold) s.body = f - kHold;
    else if (f < s.body - kHold) s.body = f + kHold;
    s.body = wrap(s.body);

    float t = target;
    if (s.body > t + kPi) t += 2.0f * kPi;
    else if (s.body < t - kPi) t -= 2.0f * kPi;

    constexpr float kOmega = 20.0f;
    const float x = dt * kOmega;
    const float e = 1.0f / (x * x * 0.48f + x * x * x * 0.235f + x + 1.0f);
    const float change = s.body - t;
    const float temp = (change * kOmega + s.velocity) * dt;
    s.body = (change + temp) * e + t;
    s.velocity = (s.velocity - temp * kOmega) * e;
    s.body = s.body * s.weight + (1.0f - s.weight) * t;
}

struct Input {
    float facing = 0.0f;          // The target yaw (+0xaa0)
    float dt = 0.0f;              // Seconds this frame
    uint32_t moveFlags = 0;       // CMovement +0x44
    /// How long the catch-up may turn the body for: the time since the
    /// facing was last copied while the player was not turning it (+0xabc,
    /// stamped by FUN_00735f60). The camera copies it once a frame after the
    /// units' update, so for the player that is the frame's time; nothing
    /// stamps it for other units, whose body takes the whole lag at once.
    float catchUpSeconds = 0.0f;
    float turnRate = kPi;         // +0x834, radians a second
    /// The player turning the unit: a turn key (0x30) or the mouse steering
    /// it (FUN_005fa6b0) - +0xa38 bit 0x1, set by FUN_00735f60.
    bool inputTurning = false;
    /// Drawn at the facing outright: dead, seated in a vehicle that says so,
    /// or a vehicle itself that does (FUN_0073dab0's first test).
    bool snap = false;
    /// The model's SpineLow and Head key bones (4 and 6), which the unit
    /// notes when its model loads (+0xa38 0x80 and 0x100, FUN_0073e840).
    /// A model with neither is drawn at the facing.
    bool hasSpine = false;
    bool hasHead = false;
    /// SpineLow takes its share only while the unit is not mounted (+0x98c).
    bool spineAllowed = true;
    /// SpineLow takes half the lag for every unit but the player's own (and
    /// the player's own in camera mode 0xd); the head takes what is left.
    bool halveSpine = false;
};

struct Result {
    /// How far the body turned this frame by catching up or being dragged
    /// (FUN_0073dab0's local_8): positive anticlockwise, to the left.
    float step = 0.0f;
    /// The lag still to show, on SpineLow and on Head, about the bones' Z.
    float spineYaw = 0.0f;
    float headYaw = 0.0f;
};

/// One frame of FUN_0073dab0's body yaw, for the unit whose state `s` is.
inline Result update(State& s, const Input& in) noexcept {
    const uint32_t f = in.moveFlags;
    const float facing = wrap(in.facing);

    if (in.snap || (f & (kSwimming | kFlying)) != 0 || !(in.hasSpine || in.hasHead)) {
        s.body = facing;
        s.velocity = 0.0f;
        s.weight = 0.0f;
    } else if ((f & kStrafe) == 0) {
        if ((f & (kForward | kBackward | kFalling)) == 0) {
            // Standing: the spring's weight comes back, for the next start.
            if (s.weight < 1.0f) s.weight += in.dt * 2.5f;
        } else if (s.weight <= 0.0f) {
            s.body = facing;
            s.velocity = 0.0f;
        } else {
            // Moving off or falling: the spring carries the body round to
            // the facing as its weight runs out over 0.4 s.
            s.weight -= in.dt * 2.5f;
            if (s.weight > 1.0f) s.weight = 1.0f;
            spring(s, facing, facing, in.dt);
        }
    } else {
        // Strafing: the body turns to the side it goes, a quarter turn, or
        // an eighth going forward or back too. Backing off to the left turns
        // it right, as backing up turns the legs.
        float offset = (f & (kForward | kBackward)) != 0 ? kQuarterPi : kHalfPi;
        const uint32_t backLeft = f & (kBackward | kStrafeLeft);
        if (backLeft == (kBackward | kStrafeLeft) || backLeft == 0) offset = -offset;
        s.weight = 1.0f;
        const float target = wrap(wrap(facing - s.body + offset) + s.body);
        spring(s, facing, target, in.dt);
    }

    Result r;
    const float delta = wrap(facing - s.body);
    const float lag = std::abs(delta);
    if (lag < 0.001f) return r;

    // Never more than a quarter turn behind: the rest is taken at once.
    if (lag > kHalfPi) r.step = std::copysign(lag - kHalfPi, delta);
    // And standing, not turned by the player, it catches up at eight times
    // the turn rate (1440 degrees a second at the default pi).
    if ((f & kStrafe) == 0 && !in.inputTurning) {
        const float most = std::max(in.catchUpSeconds, 0.0f) * in.turnRate * 8.0f;
        r.step += std::copysign(std::min(lag, most), delta);
    }
    s.body = wrap(s.body + r.step);

    const float left = std::abs(wrap(facing - s.body));
    if (left < 1e-5f) return r;
    float rest = left;
    if (in.spineAllowed && in.hasSpine) {
        float spine = in.halveSpine ? left * 0.5f : left;
        spine = std::min(spine, kQuarterPi);
        r.spineYaw = std::copysign(spine, delta);
        rest = left - spine;
    }
    if (in.hasHead) r.headYaw = std::copysign(std::min(rest, kQuarterPi), delta);
    return r;
}

/// Which way a unit shuffles its feet turning on the spot (FUN_0073dab0, the
/// +0xa38 0x800 / 0x1000 bits, chosen by FUN_0071e180).
enum class TurnShuffle : uint8_t { None, Left, Right };

/// The shuffle a unit wants this frame: standing still and standing up
/// (`mayShuffle`: stand state 0, the vfunc +0x138 FUN_0073dab0 asks), it
/// shuffles left while turned left by key (0x10) or while its body turned
/// left this frame, right likewise - left first, as FUN_0071e180 asks. With
/// neither, a unit still shuffling goes back to its stand.
[[nodiscard]] inline TurnShuffle turnShuffle(uint32_t moveFlags, float step,
                                             bool mayShuffle) noexcept {
    if ((moveFlags & kNotStillMask) != 0 || !mayShuffle) return TurnShuffle::None;
    if ((moveFlags & kTurnLeft) != 0 || step > 1e-5f) return TurnShuffle::Left;
    if ((moveFlags & kTurnRight) != 0 || step < -1e-5f) return TurnShuffle::Right;
    return TurnShuffle::None;
}

} // namespace body_yaw
} // namespace rendering
} // namespace wowee

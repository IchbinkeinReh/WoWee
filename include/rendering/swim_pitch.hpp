#pragma once

// The tilt of a swimming or flying player's body (0x0071fd80 -> 0x00719b80).
//
// The client builds the active player's model matrix with a pitch while the
// unit swims or flies (movement flags 0x200000 | 0x2000000) and moves forward
// or back, or while a pitch it had is still easing out. The pitch it eases
// toward:
//   - moving forward or back: the unit's movement pitch - for a swimmer the
//     camera's, so the body dives where the camera looks - or 45 degrees up
//     while ascending and 45 down while descending (the swim-up and swim-down
//     keys), negated when moving backward;
//   - strafing, or not moving forward or back: level.
// It eases at 4 pi a second (the step clamped to the frame time held to
// 1/90 .. 1/20 s), and snaps once within 0.01 rad. Positive is nose up.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::swim_pitch {

inline constexpr float kQuarterTurn = 0.7853982f;  // 45 degrees
inline constexpr float kEaseRate = 12.566371f;     // 4 pi a second
inline constexpr float kSnap = 0.01f;

struct Motion {
    bool forward = false;
    bool backward = false;
    bool strafing = false;
    bool ascending = false;
    bool descending = false;
};

/// The pitch the body eases toward, in radians, nose up positive.
[[nodiscard]] inline float target(const Motion& m, float movementPitch) {
    float t = 0.0f;
    if (m.forward || m.backward) {
        if (m.ascending) t = kQuarterTurn;
        else if (m.descending) t = -kQuarterTurn;
        else t = movementPitch;
    }
    if (m.strafing) t = 0.0f;
    if (m.backward && (m.ascending || m.descending)) t = -t;
    return t;
}

/// One frame of easing from `current` toward `target`.
[[nodiscard]] inline float step(float current, float target, float dt) {
    const float diff = target - current;
    if (std::abs(diff) < kSnap) return target;
    const float k = std::clamp(std::clamp(dt, 1.0f / 90.0f, 0.05f) * kEaseRate, 0.0f, 1.0f);
    return current + k * diff;
}

/// Whether the tilted matrix is used this frame: swimming or flying, and
/// moving forward or back or still easing out of a pitch.
[[nodiscard]] inline bool active(bool swimmingOrFlying, const Motion& m, float current) {
    return swimmingOrFlying && (m.forward || m.backward || std::abs(current) > kSnap);
}

/// Another unit's tilt (0x00719a90): its movement pitch as its last packet
/// gave it, taken at once - no easing - while it swims or flies and moves
/// forward or back; level otherwise.
[[nodiscard]] inline float otherUnit(bool swimmingOrFlying, bool forwardOrBack, float movementPitch) {
    return swimmingOrFlying && forwardOrBack ? movementPitch : 0.0f;
}

}  // namespace wowee::rendering::swim_pitch

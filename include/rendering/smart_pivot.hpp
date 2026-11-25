#pragma once

// The camera's smart pivot (cameraPivot, which is on): looking up from below, with the camera
// against the ground or a wall, the mouse tilts the view and leaves the camera where it is,
// instead of drawing it in to the character's feet. The client keeps that tilt beside the pitch
// (+0x130, by which 0x00606f90 turns the view once the camera is placed).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "game/vehicle_camera.hpp"

namespace wowee::rendering::smart_pivot {

/// Where the camera and its unit are, as the last picture had it (0x005ffde0). The pitch is the
/// client's, below 0 with the camera beneath what it looks at.
struct Standing {
    bool on = true;           ///< cameraPivot
    bool following = true;    ///< a unit
    bool moving = false;      ///< forward, back or sideways (0xf of its flags)
    bool flying = false;      ///< 0x2000000
    bool distanceHit = false; ///< something between the camera and what it looks at (0x10000)
    bool heightHit = false;   ///< something over the unit's head (0x20000)
    float pitch = 0.0f;
};
[[nodiscard]] inline bool mayTilt(const Standing& s) {
    if (!s.on || !s.following || s.moving || s.pitch > 0.0f) return false;
    return s.distanceHit || (!s.flying && s.heightHit);
}

/// A turn of the mouse, in radians: the pitch down the mouse's way, `sign` -1 with it inverted.
struct Turn {
    float yaw = 0.0f, pitch = 0.0f, sign = 1.0f;
};
/// cameraPivotDXMax and cameraPivotDYMin.
struct Limits {
    float yawMost = 0.05f, pitchLeast = 0.0f;
    float lowest = -1.5533430f;   ///< the least pitch there is
};

class Pivot {
public:
    /// +0x130: below 0 looking up.
    [[nodiscard]] float tilt() const { return std::fabs(ease_.value) < kAtRest ? 0.0f : ease_.value; }
    [[nodiscard]] bool easing() const { return ease_.easing(); }
    void reset() { ease_ = {}; }

    /// The mouse turns the view (0x006020b0). What the pitch is to change by; none while it is the
    /// tilt that has changed.
    [[nodiscard]] std::optional<float> turn(Turn by, bool may, float pitch, const Limits& limits, uint32_t nowMs) {
        float tilt = ease_.value;
        bool pivoting = !ease_.easing() && std::fabs(tilt) >= kAtRest;
        if (may) {
            if (limits.pitchLeast < std::fabs(by.pitch) && std::fabs(by.yaw) < limits.yawMost) pivoting = true;
            if (by.pitch > 0.0f) {
                const bool within = tilt >= 0.0f || by.sign * by.pitch + tilt <= 0.0f;
                if (!within) by.pitch += tilt;   // what is left of it
                if (!within || std::fabs(tilt) < kNothing) {
                    pivoting = false;
                    tilt = 0.0f;
                    stay(0.0f);
                }
            }
        }
        if (pivoting) {
            tilt += by.sign * by.pitch;
            if (pitch < 0.0f) tilt = std::max(tilt, limits.lowest - pitch);
            ease_.value = tilt;
            if (tilt <= 0.0f && pitch < 0.0f) {
                stay(tilt);
                return std::nullopt;
            }
        }
        back(nowMs);
        return by.sign * by.pitch;
    }

    /// As the camera has been placed (0x00606f90 -> 0x00602600): back to 0 where it may not tilt,
    /// else it stays.
    void placed(bool may, uint32_t nowMs) {
        if (std::fabs(ease_.value) >= kAtRest && !may) back(nowMs);
        else stay(ease_.value);
    }
    void step(uint32_t nowMs) { ease_.step(nowMs); }

private:
    static constexpr float kAtRest = 0.001f;
    static constexpr float kNothing = 2.3841858e-07f;
    /// cameraTargetSmoothSpeed's 90 degrees a second (0x006012d0).
    static constexpr float kSpeed = 1.5707964f;

    void stay(float tilt) {
        ease_ = {};
        ease_.value = tilt;
    }
    void back(uint32_t nowMs) {
        if (ease_.easing() || std::fabs(ease_.value) < kAtRest) return;
        const auto ms = static_cast<uint32_t>(std::fabs(ease_.value) / kSpeed * 1000.0f);
        ease_.set(0.0f, nowMs, nowMs + std::max(ms, 1u));
    }

    game::vehicle::EasedCameraOffset ease_;
};

}  // namespace wowee::rendering::smart_pivot

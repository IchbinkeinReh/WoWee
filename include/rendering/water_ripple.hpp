#pragma once

// The client's ripples on liquid (Water0Ripple): a ring where a unit stands
// or turns in water, a wake behind one that moves, a ring as it crosses four
// tenths of its height. Logic only; SwimEffects draws them.
//
//   0x0071cba0  what a unit spawns, and when the next is due
//   0x0079d460, 0x0079d180, 0x0079cf40  how a ripple starts
//   0x0079d5e0  its life, each frame
//
// The client clips each ripple to the liquid's triangles. It is drawn here as
// one flat quad at the surface.

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::rendering::water_ripple {

enum class Mode : uint8_t { Still = 0, Turning = 1, Moving = 2, Crossing = 3 };

// Move flags' direction bits (0x0071cba0 reads them for a wake's angle).
inline constexpr uint32_t kForward = 1, kBackward = 2, kStrafeLeft = 4, kStrafeRight = 8;

struct Ripple {
    glm::vec3 position{0.0f};
    float angle = 0.0f;
    float size = 0.0f;  // half the quad's width
    float growth = 0.0f;
    float alpha = 0.0f;
    float peak = 0.0f;
    float rise = 0.0f;
    float fall = 0.0f;
    float life = 0.0f;  // seconds left
    bool wake = false;
};

/// 0x0079d460 and 0x0079cf40: faint under a sixth of strength, up 40% of its
/// life and down the rest.
inline Ripple start(const glm::vec3& position, float angle, float size, float life, float strength,
                    float growth, bool wake) {
    constexpr float kRiseShare = 0.4f;  // 0x00a3fac0, both kinds
    Ripple r;
    r.position = position;
    r.angle = -angle;
    r.size = size;
    r.growth = growth;
    r.peak = strength < 1.0f / 6.0f ? strength * 6.0f : 1.0f;
    r.rise = r.peak / (kRiseShare * life);
    r.fall = -(r.peak / ((1.0f - kRiseShare) * life));
    r.life = life;
    r.wake = wake;
    return r;
}

/// 0x0079d5e0. False when it is over.
inline bool step(Ripple& r, float dt) {
    r.life -= dt;
    if (r.life < 0.0f) return false;
    r.size += r.growth * dt;
    float left = dt;
    if (r.rise != 0.0f) {
        r.alpha += dt * r.rise;
        if (r.alpha > r.peak) {
            left = dt - (r.alpha - r.peak) / r.rise;
            r.alpha = r.peak;
            r.rise = 0.0f;
        }
    }
    if (r.rise == 0.0f) r.alpha += left * r.fall;
    return r.alpha > 0.0f;
}

/// A wake's angle off the facing, by the move flags (0x0071cba0).
inline float wakeAngle(float facing, uint32_t flags) {
    constexpr float kPi = 3.1415927f, kHalf = 1.5707964f, kQuarter = 0.78539819f, kThree = kPi - kQuarter;
    if (flags & kStrafeLeft) {
        if (flags & kForward) return facing + kQuarter;
        return (flags & kBackward) ? facing + kThree : facing + kHalf;
    }
    if (flags & kStrafeRight) {
        if (flags & kForward) return facing - kQuarter;
        return (flags & kBackward) ? facing - kThree : facing - kHalf;
    }
    return (flags & kBackward) ? facing + kPi : facing;
}

/// What a unit's state asks of 0x0071cba0.
struct Unit {
    glm::vec3 surfacePoint{0.0f};  // on the water, at the unit
    float depth = 0.0f;            // surface less feet
    float height = 2.0f;           // collision height
    float speed = 0.0f;
    float facing = 0.0f;
    float sizeValue = 1.0f;        // the unit's virtual at 0x3c
    uint32_t moveFlags = 0;
    Mode mode = Mode::Still;
};

/// The client's random draws, each 0..1.
struct Rolls {
    float size = 0.5f, life = 0.5f, growth = 0.5f, angle = 0.5f, delay = 0.5f;
};

/// One spawn, if the unit is shallow enough; the seconds to the next go in
/// `next`.
inline bool spawn(const Unit& u, const Rolls& roll, Ripple& out, float& next) {
    const float reach = std::max(u.height + u.height, 1.0f);
    if (u.depth >= reach) return false;
    const bool wake = u.mode == Mode::Moving;
    float interval = 1.0f;
    if (wake && u.speed > 0.0001f) interval = u.speed >= 20.0f ? 0.125f : 2.5f / u.speed;
    const float inverse = 1.0f / interval;

    float size = std::clamp(u.sizeValue * 0.33333334f * (roll.size * 0.2f - 0.1f + 1.0f),
                            0.33333334f, 5.0f / 3.0f);
    float life = roll.life * 0.1f - 0.05f + 0.65f;
    float growth = (roll.growth * 1.5f - 0.75f + 3.75f) * 12.0f * 0.027777778f * inverse;
    float strength = 0.16666667f;
    float fade = 1.0f;
    const float half = reach * 0.5f;
    if (half < u.depth) {
        fade = ((reach - u.depth) / (reach - half)) * 0.5f + 0.5f;
        strength *= fade;
        life *= fade;
        size *= fade;
    }
    if (u.mode == Mode::Still) {
        strength *= 0.8f;
        growth *= 0.25f;
        size *= 0.6f;
    }
    const float angle = wake ? wakeAngle(u.facing, u.moveFlags) : roll.angle * 6.2831855f;
    out = start(u.surfacePoint, angle, size, life, strength, growth, wake);
    next = wake ? fade * interval * 0.25f : 0.4f + roll.delay * 0.05f;
    return true;
}

}  // namespace wowee::rendering::water_ripple

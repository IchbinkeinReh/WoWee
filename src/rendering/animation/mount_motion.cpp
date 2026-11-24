#include "rendering/animation/mount_motion.hpp"
#include "core/coordinates.hpp"
#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

void MountMotion::reset() {
    prevYaw_ = 0.0f;
    roll_ = 0.0f;
}

MountMotion::Output MountMotion::update(const Input& in) {
    Output out;
    const float dt = in.deltaTime;

    // ── Banking lean (client FUN_00719b80) ──────────────────────────────
    // The client banks the mount's model only when the movement block has its
    // flying bit set (the roll is gated on param_4 = flags & flying); a ground
    // mount is never rolled. The target roll is the turn rate in radians a
    // second times 0.15, eased toward by clamp(dt * pi/2 * k, 0, 1) - k is 4
    // unless the mount is moving forward or back, when it is 1 - and clamped to
    // a quarter turn either way. The turn is measured as the wrapped change in
    // facing *before* the divide by dt; the old code wrapped the rate itself,
    // and worked in degrees so any turn pinned the roll at its small cap.
    //
    // The magnitude, the sign and the gating are the client's: -0.15, the
    // left side down into a turn to the left, about the model's own forward
    // axis as the character renderer now composes it (yaw, pitch, roll, as
    // 0x004c56d0) - the player's own bank (swim_pitch::stepRoll) is the same.
    constexpr float kPi = core::coords::TWO_PI * 0.5f;
    float yawDeltaDeg = in.characterYaw - prevYaw_;
    while (yawDeltaDeg > 180.0f) yawDeltaDeg -= 360.0f;
    while (yawDeltaDeg < -180.0f) yawDeltaDeg += 360.0f;
    prevYaw_ = in.characterYaw;
    if (!in.taxiFlight && in.flying && dt > 0.0f) {
        const float yawRateRad = (yawDeltaDeg * (core::coords::TWO_PI / 360.0f)) / dt;
        const float targetRoll = yawRateRad * -0.15f;
        const float ease = std::clamp(dt * (kPi * 0.5f) * (in.forwardOrBack ? 1.0f : 4.0f), 0.0f, 1.0f);
        roll_ += ease * (targetRoll - roll_);
        roll_ = std::clamp(roll_, -kPi * 0.25f, kPi * 0.25f);
    } else {
        // Not flying: the client leaves the stored roll untouched, so on the
        // ground it never leaves zero. Ease any residual bank from a landing
        // back to level rather than freezing the mount mid-roll.
        roll_ = roll_ + (0.0f - roll_) * (1.0f - std::exp(-8.0f * dt));
    }
    out.roll = roll_;

    if (in.moving && in.haveAnimation && in.animationDuration > 1.0f) {
        const float norm = std::fmod(in.animationTime, in.animationDuration) / in.animationDuration;
        out.bob = std::sin(norm * core::coords::TWO_PI * (in.taxiFlight ? 2.0f : 1.0f)) * 0.12f;
    }
    return out;
}

} // namespace rendering
} // namespace wowee

#pragma once

// How a swimmer or a flyer moves by its own keys (0x00987b50 for +0x44's
// movement flags; its speed 0x00987570): along its facing at its pitch, to its
// side level, up or down at its full speed, and level with half of each of two
// ways at once. Nothing else moves it: no buoyancy lifts a swimmer to the
// surface, and a depth kept is a depth held (0x00760b40 only sweeps the move).

#include <cmath>
#include <numbers>

#include <glm/glm.hpp>

namespace wowee::rendering::collision {

/// +0x44's 0x1, 0x2, 0x4, 0x8, 0x400000 and 0x800000.
struct SwimFlyKeys {
    bool forward = false, backward = false, left = false, right = false;
    bool rising = false, sinking = false;
    [[nodiscard]] bool moves() const { return forward || backward || left || right || rising || sinking; }
};

/// 0x00987570: the way back's speed while going back, where it is no faster.
[[nodiscard]] inline float swimFlySpeed(const SwimFlyKeys& k, float speed, float backSpeed) {
    return k.backward && backSpeed <= speed ? backSpeed : speed;
}

/// The velocity in yards a second. `facing` and `left` are unit vectors in the
/// horizontal plane, `pitch` the unit's (+0x24), up positive.
[[nodiscard]] inline glm::vec3 swimFlyVelocity(SwimFlyKeys k, const glm::vec2& facing, const glm::vec2& left,
                                               float pitch, float speed) {
    constexpr float kHalf = std::numbers::sqrt2_v<float> * 0.5f;
    // Two opposite ways at once are none (they clear each other's flag).
    if (k.forward && k.backward) k.forward = k.backward = false;
    if (k.left && k.right) k.left = k.right = false;
    if (k.rising && k.sinking) k.rising = k.sinking = false;
    const float ahead = k.forward ? 1.0f : k.backward ? -1.0f : 0.0f;
    const float aside = k.left ? 1.0f : k.right ? -1.0f : 0.0f;
    // +0x70 and +0x64: level, and along the pitch where it is not swimming or
    // flying to the side alone; two ways at once half of each (0x00987ef0).
    const float share = ahead != 0.0f && aside != 0.0f ? kHalf : 1.0f;
    const glm::vec2 level = (facing * ahead + left * aside) * share;
    if (k.rising || k.sinking) {
        const float up = k.rising ? 1.0f : -1.0f;
        if (ahead == 0.0f && aside == 0.0f) return {0.0f, 0.0f, up * speed};
        // 0x00987700: level, whatever the pitch.
        return glm::vec3(level * kHalf * speed, up * kHalf * speed);
    }
    if (ahead == 0.0f) return glm::vec3(level * speed, 0.0f);
    const float c = std::cos(pitch), s = std::sin(pitch);
    const glm::vec2 along = facing * (ahead * c) + left * aside;
    return glm::vec3(along.x, along.y, ahead * s) * (share * speed);
}

}  // namespace wowee::rendering::collision

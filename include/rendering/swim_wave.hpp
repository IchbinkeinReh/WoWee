#pragma once

// Small waves on the traced water, made by what a swimmer's animation does at
// the surface: a hand or the body moving near the water starts a ring, and
// the faster it moves the stronger the ring. An experiment, not the client's
// (it has no such thing); with the tracing off its ripples show instead
// (water_ripple.hpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::swim_wave {

inline constexpr size_t kMaxWaves = 16;
inline constexpr float kLife = 1.6f;          // seconds a ring lasts
inline constexpr float kNearSurface = 0.35f;  // how close a limb must be
inline constexpr float kMinSpeed = 0.8f;      // yards a second, slower makes none
inline constexpr float kFullSpeed = 5.0f;     // and at this, a ring's full strength
inline constexpr float kCooldown = 0.16f;     // seconds between one limb's rings

struct Wave {
    glm::vec2 position{0.0f};
    float age = 0.0f;
    float strength = 0.0f;
};

/// One moving part: a hand, or the body.
struct Limb {
    glm::vec3 last{0.0f};
    bool seen = false;
    float cooldown = 0.0f;
};

/// Ages the rings and drops the finished.
inline void age(std::vector<Wave>& waves, float dt) {
    for (Wave& w : waves) w.age += dt;
    std::erase_if(waves, [](const Wave& w) { return w.age >= kLife; });
}

/// A limb's move this frame: a ring if it is at the surface and quick enough.
inline void limbMoved(std::vector<Wave>& waves, Limb& limb, const glm::vec3& now, float waterZ, float dt) {
    limb.cooldown -= dt;
    const glm::vec3 was = limb.last;
    const bool seen = limb.seen;
    limb.last = now;
    limb.seen = true;
    if (!seen || dt <= 0.0f || limb.cooldown > 0.0f) return;
    if (std::abs(now.z - waterZ) > kNearSurface) return;
    const float speed = glm::length(now - was) / dt;
    if (speed < kMinSpeed) return;
    if (waves.size() >= kMaxWaves) waves.erase(waves.begin());
    Wave w;
    w.position = glm::vec2(now);
    w.strength = std::clamp(speed / kFullSpeed, 0.15f, 1.0f);
    waves.push_back(w);
    limb.cooldown = kCooldown;
}

}  // namespace wowee::rendering::swim_wave

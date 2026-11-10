#pragma once

// Small waves on the traced water, made by what a swimmer's animation does at
// the surface: a hand or the body moving near the water starts a ring, and
// the faster it moves the stronger the ring. An experiment, not the client's
// (it has no such thing); with the tracing off its ripples show instead
// (water_ripple.hpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::swim_wave {

/// Rings at once: the character's reserve (kOwnRings; its three limbs make
/// at most kLife / kCooldown each, 30) and the other units' share of the rest
/// (water_unit_share.hpp), enough for a crowd out to 150 yards.
inline constexpr size_t kMaxWaves = 128;
inline constexpr size_t kOwnRings = 32;
inline constexpr float kLife = 1.6f;          // seconds a ring lasts
inline constexpr float kNearSurface = 0.35f;  // how close a limb must be
inline constexpr float kMinSpeed = 0.8f;      // yards a second, slower makes none
inline constexpr float kFullSpeed = 5.0f;     // and at this, a ring's full strength
inline constexpr float kCooldown = 0.16f;     // seconds between one limb's rings

struct Wave {
    glm::vec2 position{0.0f};
    float age = 0.0f;
    float strength = 0.0f;
    uint64_t owner = 0;  ///< the unit's guid, 0 the character's
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

/// Why a limb's move made no ring, or that it made one.
enum class Made { Ring, None, OverShare, Full };

/// A limb's move this frame: a ring if it is at the surface and quick enough,
/// and its cooldown over (`cooldown` seconds after its last).
inline bool limbReady(Limb& limb, const glm::vec3& now, float waterZ, float dt, float cooldown, float& speed) {
    limb.cooldown -= dt;
    const glm::vec3 was = limb.last;
    const bool seen = limb.seen;
    limb.last = now;
    limb.seen = true;
    if (!seen || dt <= 0.0f || limb.cooldown > 0.0f) return false;
    if (std::abs(now.z - waterZ) > kNearSurface) return false;
    speed = glm::length(now - was) / dt;
    if (speed < kMinSpeed) return false;
    limb.cooldown = cooldown;
    return true;
}

inline Wave ringAt(const glm::vec3& now, float speed, uint64_t owner) {
    Wave w;
    w.position = glm::vec2(now);
    w.strength = std::clamp(speed / kFullSpeed, 0.15f, 1.0f);
    w.owner = owner;
    return w;
}

/// The character's limb. With the rings full, another unit's oldest ring
/// gives way, or with none the character's oldest.
inline Made limbMoved(std::vector<Wave>& waves, Limb& limb, const glm::vec3& now, float waterZ, float dt) {
    float speed = 0.0f;
    if (!limbReady(limb, now, waterZ, dt, kCooldown, speed)) return Made::None;
    if (waves.size() >= kMaxWaves) {
        auto unitRing = std::find_if(waves.begin(), waves.end(), [](const Wave& w) { return w.owner != 0; });
        waves.erase(unitRing != waves.end() ? unitRing : waves.begin());
    }
    waves.push_back(ringAt(now, speed, 0));
    return Made::Ring;
}

/// Another unit's limb (`owner` its guid), within its share of `share` rings
/// (water_unit_share.hpp): it waits between rings as long as `limbs` limbs
/// keep to the share, and makes none while the share or the units' part of
/// the rings (all but kOwnRings) is spent - nothing of another's gives way.
inline Made unitLimbMoved(std::vector<Wave>& waves, Limb& limb, const glm::vec3& now, float waterZ, float dt,
                          uint64_t owner, size_t share, int limbs) {
    const float cooldown =
        share > 0 ? std::max(kCooldown, kLife * static_cast<float>(limbs) / static_cast<float>(share)) : kLife;
    float speed = 0.0f;
    if (!limbReady(limb, now, waterZ, dt, cooldown, speed)) return Made::None;
    size_t mine = 0, units = 0;
    for (const Wave& w : waves) {
        mine += w.owner == owner ? 1u : 0u;
        units += w.owner != 0 ? 1u : 0u;
    }
    if (mine >= share) return Made::OverShare;
    if (units >= kMaxWaves - kOwnRings || waves.size() >= kMaxWaves) return Made::Full;
    waves.push_back(ringAt(now, speed, owner));
    return Made::Ring;
}

}  // namespace wowee::rendering::swim_wave

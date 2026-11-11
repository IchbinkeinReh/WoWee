#pragma once
/// Where an emitter's particle light stands, and how it is split into
/// several (the Ray Tracing page's "Point lights per particle light",
/// rtparticlelights, 1..10; a WoWee-only option, user decision 2026-11-11).
///
/// The light is placed on the particles' mean path, worked out in closed form
/// from the emitter's motion as it is this frame - where it emits from, the
/// mean direction and speed it gives a particle, its gravity and its drag -
/// with nothing kept from earlier frames:
///
///   - N = 1: one light where the particles are on average at half their
///     lifetime, age L / 2.
///   - N > 1: N lights spaced evenly between the particles' start (age 0)
///     and their mean end (age L), as flexbox's space-evenly: light i
///     (1..N) at age L * i / (N + 1). N = 1 is the same rule.
///   - Their strengths are geometric, r^(i-1) for r = kParticleLightRatio,
///     normalised to sum 1: the youngest light the strongest, each later one
///     less, and all together exactly the single light's.
///
/// A short lifespan needs no special case: the ages are fractions of it, so
/// the lights only stand closer together. The wind (file 0x1a0) is left out
/// of the mean path - it blows only for the first windTime seconds and few
/// emitters carry one.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::rt_lights {

inline constexpr int kMinParticleLights = 1;
inline constexpr int kMaxParticleLights = 10;
inline constexpr float kParticleLightRatio = 0.6f;
/// The least a split part may give and still be traced: selectLights keeps
/// no light dimmer than this (its kMinStrength). A fainter part's share goes
/// to the first light instead, so the total stays the one light's.
inline constexpr float kMinParticlePartStrength = 0.02f;

/// What of the single light the i-th of n (0-based) takes.
inline std::vector<float> particleLightWeights(int n) {
    n = std::clamp(n, kMinParticleLights, kMaxParticleLights);
    std::vector<float> w(static_cast<size_t>(n));
    float sum = 0.0f;
    float v = 1.0f;
    for (int i = 0; i < n; ++i) {
        w[static_cast<size_t>(i)] = v;
        sum += v;
        v *= kParticleLightRatio;
    }
    for (float& x : w) x /= sum;
    return w;
}

/// The age of the i-th of n lights (0-based) for a lifespan `life`.
inline float particleLightAge(int i, int n, float life) {
    return life * static_cast<float>(i + 1) / static_cast<float>(n + 1);
}

/// sin(x) / x, the mean of cos over -x..x.
inline float meanCos(float x) {
    return std::fabs(x) < 1e-4f ? 1.0f : std::sin(x) / x;
}

/// An emitter's mean particle as it leaves it, in the emitter's own space:
/// the mean start (offset from the emitter's position) and the mean unit
/// direction times the speed. Per emitter type, as the client rolls them
/// (0x009815c0 plane, 0x00981950 sphere): a roll of -1..1 times a range
/// averages to the mean of the cosine over it, the sines to nothing.
struct MeanLaunch {
    glm::vec3 offset{0.0f};
    glm::vec3 velocity{0.0f};
};
inline MeanLaunch meanLaunch(int emitterType, float speed, float verticalRange, float horizontalRange,
                             float areaLength, float areaWidth, float zSource, bool upward) {
    MeanLaunch m;
    glm::vec3 dir(0.0f, 0.0f, 1.0f);
    if (emitterType == 2) {
        // Sphere: out from the centre at the mean radius; straight up under
        // flag 0x8000; away from zSource.
        const float radius = 0.5f * (areaLength + areaWidth);
        const glm::vec3 out(meanCos(horizontalRange) * meanCos(verticalRange), 0.0f, 0.0f);
        m.offset = out * radius;
        if (zSource != 0.0f) {
            dir = glm::vec3(0.0f, 0.0f, zSource > 0.0f ? -1.0f : 1.0f);
        } else if (upward) {
            dir = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            dir = out;
        }
    } else if (emitterType == 3) {
        // Spline: up from the curve, turned about it either way alike.
        dir = glm::vec3(0.0f, 0.0f, meanCos(verticalRange));
    } else {
        // Plane: a lean off the up axis either way alike; away from zSource.
        if (zSource != 0.0f) {
            dir = glm::vec3(0.0f, 0.0f, zSource > 0.0f ? -1.0f : 1.0f);
        } else {
            dir = glm::vec3(0.0f, 0.0f, meanCos(verticalRange));
        }
    }
    m.velocity = dir * speed;
    return m;
}

/// How far the mean particle has moved at age t from a start velocity v0,
/// under gravity g (down the space's z) and drag k per second - the client's
/// step (m2_particle::step) taken as its continuous limit.
inline glm::vec3 meanDisplacement(const glm::vec3& v0, float gravity, float drag, float t) {
    if (!(t > 0.0f)) return glm::vec3(0.0f);
    if (drag > 1e-4f) {
        const float e = (1.0f - std::exp(-drag * t)) / drag;
        glm::vec3 d = v0 * e;
        d.z -= gravity / drag * (t - e);
        return d;
    }
    glm::vec3 d = v0 * t;
    d.z -= 0.5f * gravity * t * t;
    return d;
}

}  // namespace wowee::rendering::rt_lights

#pragma once

/// The arithmetic of the volumetric light (VolumetricLight), apart from Vulkan
/// so a test can state it: what each quality spends, how a lamp's light fades
/// through the air round it, where a view ray crosses a lamp's reach, how
/// thick the air is, and how the sun's and the moon's shares hand over at the
/// horizon. volumetric_light.comp.glsl holds the same formulas; each says
/// which of these it mirrors.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::volumetric_light {

/// The setting's choices, in the order the settings file stores them.
enum class Quality : int { Off = 0, Low = 1, High = 2 };

inline Quality qualityFromIndex(int index) {
    return index <= 0 ? Quality::Off : (index == 1 ? Quality::Low : Quality::High);
}

/// What one frame may spend, fixed per quality, so the cost has a ceiling
/// whatever the scene holds: the march runs at a fraction of the screen's
/// size, the sun's (or moon's) light in a fixed number of steps along each
/// ray, every lamp the ray passes through in a fixed number more, and at most
/// so many lamps.
struct Budget {
    unsigned divisor = 0;     ///< the march's image is the screen's size over this
    unsigned sunSteps = 0;    ///< samples of the sky body's light along a ray
    unsigned lightSteps = 0;  ///< samples inside each lamp's reach a ray crosses
    unsigned maxLights = 0;   ///< lamps per frame, the most important first
};

inline Budget budgetFor(Quality q) {
    switch (q) {
        case Quality::Low: return {.divisor = 4, .sunSteps = 12, .lightSteps = 4, .maxLights = 16};
        case Quality::High: return {.divisor = 2, .sunSteps = 24, .lightSteps = 8, .maxLights = 32};
        case Quality::Off: break;
    }
    return {};
}

/// The most lamps any quality takes: the size of the shader's array.
inline constexpr unsigned kMaxLights = 32;

/// How far along the view the sun's and the moon's light is gathered, yards.
/// The near air, where a shaft is a real share of what the eye looks
/// through; the shadow map's own reach ends the rest.
inline constexpr float kSunReach = 120.0f;

/// How much of a lamp's light the air `distance` yards from it holds, for a
/// lamp that reaches `end` yards: bright at its core, a soft tail, nothing
/// at its reach. The inverse square from a core a fifth of the reach (three
/// quarters of a yard to three yards), times 2 so it crosses 1 at the core,
/// windowed to nothing at the end - rt_lights::realisticAttenuation's shape
/// with a core the size of the glow rather than of the flame.
/// volumetric_light.comp.glsl's vlGlowFalloff must agree.
inline float glowFalloff(float distance, float end) {
    if (!(end > 0.0f) || !(distance < end)) return 0.0f;
    const float core = std::clamp(0.2f * end, 0.75f, 3.0f);
    const float q = distance / core;
    const float x = distance / end;
    const float w = 1.0f - x * x * x * x;
    return std::min(2.0f * w * w / (1.0f + q * q), 2.0f);
}

/// Where the ray origin + t * dir (dir of unit length) runs inside the sphere
/// round `centre` of `radius`, clipped to 0..maxT: the stretch of air a lamp
/// lights on the way to what the pixel shows. None when it misses, or when
/// that stretch lies behind the eye or past the surface.
inline std::optional<std::pair<float, float>> segmentThroughSphere(const glm::vec3& origin, const glm::vec3& dir,
                                                                   float maxT, const glm::vec3& centre,
                                                                   float radius) {
    const glm::vec3 oc = origin - centre;
    const float b = glm::dot(oc, dir);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (!(disc > 0.0f)) return std::nullopt;
    const float s = std::sqrt(disc);
    const float t0 = std::max(-b - s, 0.0f);
    const float t1 = std::min(-b + s, maxT);
    if (!(t1 > t0)) return std::nullopt;
    return std::make_pair(t0, t1);
}

/// Henyey-Greenstein, scaled so scattering evenly in every direction is 1 -
/// the mist's phaseHG (volumetric_fog_inject.comp.glsl), the same function.
inline float phaseHG(float cosTheta, float g) {
    const float g2 = g * g;
    return (1.0f - g2) / std::pow(std::max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f), 1.5f);
}

/// How much thicker than clear air the air is: the zone's own fog (read off
/// how close its authored fog ends: Duskwood's 525 yards gives more, an open
/// plain's far end less - the mist's factor, Renderer::volumetricFogExtinction),
/// the morning mist round half past six, and rain or snow. 1 is a clear day
/// on a plain whose fog ends at 900 yards.
inline float airFactor(float authoredFogEnd, float hours, float rain, float snow) {
    float f = 1.0f;
    if (authoredFogEnd > 1.0f) f *= std::clamp(900.0f / authoredFogEnd, 0.6f, 2.0f);
    const float fromDawn = std::abs(hours - 6.5f);
    const float t = std::clamp(fromDawn / 2.5f, 0.0f, 1.0f);
    const float dawn = 1.0f - t * t * (3.0f - 2.0f * t);
    f *= 1.0f + 0.8f * dawn;
    f *= 1.0f + 1.2f * std::clamp(rain, 0.0f, 1.0f);
    f *= 1.0f + 0.8f * std::clamp(snow, 0.0f, 1.0f);
    return f;
}

/// The sun's and the moon's shares of the sky's light in the air, from the
/// sines of their drawn elevations: the sun's while it is up, fading out over
/// its last few degrees; the moon's only once the sun has gone and while she
/// is up herself. Neither below the horizon - no light comes from a sky body
/// that is not there to be seen.
struct BodyShares {
    float sun = 0.0f;
    float moon = 0.0f;
};

inline BodyShares bodyShares(float sunHeight, float moonHeight) {
    const auto up = [](float h) {
        const float t = std::clamp(h / 0.08f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    BodyShares s;
    s.sun = up(sunHeight);
    s.moon = (1.0f - s.sun) * up(moonHeight);
    return s;
}

/// The forward lobe's share while the sky body's disc is hidden from the eye
/// (`seen` 0, the renderer's eased occlusion) - behind a crown of leaves or a
/// roof the glow round it fades to this, and the shafts through the gaps,
/// which each sample's own line to the sun decides, stay.
inline constexpr float kHiddenLobe = 0.4f;

inline float lobeShare(float seen) {
    return kHiddenLobe + (1.0f - kHiddenLobe) * std::clamp(seen, 0.0f, 1.0f);
}

/// The lamps the march takes, out of a list most important first (as
/// rt_lights::selectLights leaves it): the first `max` whose reach comes
/// within `range` yards of the eye. Returns their indices, in that order.
template <class Light>
std::vector<size_t> pickLights(const std::vector<Light>& lights, const glm::vec3& eye, float range, size_t max) {
    std::vector<size_t> out;
    for (size_t i = 0; i < lights.size() && out.size() < max; ++i) {
        const Light& l = lights[i];
        if (!(l.attenuationEnd > 0.0f)) continue;
        if (glm::length(l.position - eye) - l.attenuationEnd > range) continue;
        out.push_back(i);
    }
    return out;
}

}  // namespace wowee::rendering::volumetric_light

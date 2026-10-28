#pragma once

// Toward the sun the shadows fall from: the sun the sky draws while it is up,
// the light's own direction (Light.dbc) while it is down - and, near the
// horizon, a blend of the two.
//
// The switch was at the horizon itself. In the evening the light's
// direction stands some twenty degrees up while the drawn sun sets, so the
// moment the sun touched the horizon every shadow jumped from the one to the
// other: between Debug Time 6.0 and 6.25, and again between 20.5 and 20.75.
// Over kHorizonBand either side of the horizon (by the drawn sun's height)
// the direction now goes from one to the other smoothly - about an hour of
// game time each way.

#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering::shadow_sun {

/// The drawn sun's height (the sine of its elevation) over which the shadows
/// hand over from the light's direction to the drawn sun, either side of the
/// horizon.
inline constexpr float kHorizonBand = 0.12f;

/// Toward the sun the shadows fall from. `towardLight`: toward the light
/// (Light.dbc's direction, reversed); `drawnSun`: toward the sun the sky
/// draws (zero when there is none); `minHeight`: the least height the drawn
/// sun is held at (the shadow map's and the trace's floor), below -1 for
/// none.
inline glm::vec3 towardSun(const glm::vec3& towardLight, const glm::vec3& drawnSun, float minHeight) {
    const glm::vec3 light = glm::dot(towardLight, towardLight) > 1e-12f
                                ? glm::normalize(towardLight)
                                : glm::normalize(glm::vec3(0.3f, 0.7f, 0.6f));
    if (!(glm::dot(drawnSun, drawnSun) > 1e-12f)) return light;
    const glm::vec3 sun = glm::normalize(drawnSun);
    glm::vec3 held = sun;
    if (held.z < minHeight) {
        const glm::vec2 flat(sun.x, sun.y);
        const float flatLen = glm::length(flat);
        held = flatLen > 1e-6f ? glm::vec3(flat / flatLen * std::sqrt(1.0f - minHeight * minHeight), minHeight)
                               : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    const float x = glm::clamp((sun.z + kHorizonBand) / (2.0f * kHorizonBand), 0.0f, 1.0f);
    const float t = x * x * (3.0f - 2.0f * x);
    const glm::vec3 blended = light * (1.0f - t) + held * t;
    return glm::dot(blended, blended) > 1e-12f ? glm::normalize(blended) : held;
}

}  // namespace wowee::rendering::shadow_sun

#pragma once

// The ray traced water's optics (water_rt.glsli), for the tests: Fresnel at
// the air-water interface, the refracted direction and its total internal
// reflection, the light that comes up through the water over a path, and
// the sky a reflection that leaves the scene meets.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include <limits>

namespace wowee::rendering::rt_water {

inline constexpr float kIor = 1.333f;
inline constexpr float kF0 = 0.02f;

/// Schlick's reflectance at the interface, cos the angle to the normal.
[[nodiscard]] inline float fresnel(float cosTheta) {
    const float c = std::clamp(1.0f - cosTheta, 0.0f, 1.0f);
    return kF0 + (1.0f - kF0) * c * c * c * c * c;
}

/// The direction refracted through the surface from `incident` (unit, going
/// in) about `normal` (unit, facing the incident side), eta = n1 / n2; zero
/// past the critical angle, as GLSL's refract.
[[nodiscard]] inline glm::vec3 refractDir(const glm::vec3& incident, const glm::vec3& normal, float eta) {
    const float d = glm::dot(normal, incident);
    const float k = 1.0f - eta * eta * (1.0f - d * d);
    if (k < 0.0f) return glm::vec3(0.0f);
    return eta * incident - (eta * d + std::sqrt(k)) * normal;
}

/// What comes up through the water: the bed's light absorbed over the path
/// length `d` (Beer-Lambert, exp(-sigma d)), and the water's own light
/// scattered in by what was absorbed.
[[nodiscard]] inline glm::vec3 throughWater(const glm::vec3& bed, const glm::vec3& scatter,
                                            const glm::vec3& sigma, float d) {
    const glm::vec3 t = glm::exp(-sigma * d);
    return bed * t + scatter * (glm::vec3(1.0f) - t);
}

/// How far the sun comes through the water to a bed `depth` under the surface:
/// it bends down at the surface, so however low it stands the path stays short.
/// `toSun` unit, above the water.
[[nodiscard]] inline float sunPathInWater(float depth, const glm::vec3& toSun) {
    const glm::vec3 up = -refractDir(-toSun, glm::vec3(0.0f, 0.0f, 1.0f), 1.0f / kIor);
    return depth / std::max(up.z, 0.2f);
}

/// The sky along `dir`: the horizon's colour level, the zenith's overhead.
[[nodiscard]] inline glm::vec3 sky(const glm::vec3& dir, const glm::vec3& horizon, const glm::vec3& top) {
    const float up = std::clamp(dir.z, 0.0f, 1.0f);
    return glm::mix(horizon, top, std::sqrt(up));
}

/// The depth the screen reports under a surface point `depthUnder` yards
/// deep (water.frag's shoreDepth before rtwDepthBelow): the depth buffer's
/// distance past the surface along the view, times the view's slope, is the
/// depth where the view's ray meets the bed. Over a bed that falls away from
/// the camera at `bedSlope` (rise over run) seen at `viewSlope` (the view's
/// descent over its run, more than the bed's), that is further out and deeper:
/// depthUnder * viewSlope / (viewSlope - bedSlope). Exact on a flat bed or
/// looking straight down; on a beach under a low camera, several times the
/// depth, which squeezed the shore's masks into the waterline.
[[nodiscard]] inline float screenShoreDepth(float depthUnder, float viewSlope, float bedSlope) {
    if (viewSlope <= bedSlope) return std::numeric_limits<float>::infinity();
    return depthUnder * viewSlope / (viewSlope - bedSlope);
}

}  // namespace wowee::rendering::rt_water

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

/// The waves' normal from their tangent and binormal (water.frag's meshNorm):
/// cross(binormal, tangent) faces down - (0, 0, -1) on still water - so it is
/// turned over. Left facing down and mixed with the detail normal, which
/// faces up, the shaded normal pointed down wherever the detail had less than
/// half the say, and the glint, which needs it toward the sky, was gone.
[[nodiscard]] inline glm::vec3 waveNormalUp(const glm::vec3& tangent, const glm::vec3& binormal) {
    const glm::vec3 n = glm::normalize(glm::cross(binormal, tangent));
    return n.z < 0.0f ? -n : n;
}

/// The normal the water is shaded with (water.frag's norm): the waves' with
/// `detailShare` of the hand-made detail normal (0.55 by its fade, times the
/// weather's share on the physics waves).
[[nodiscard]] inline glm::vec3 shadedNormal(const glm::vec3& wavesUp, const glm::vec3& detail, float detailShare) {
    return glm::normalize(glm::mix(wavesUp, detail, detailShare));
}

/// The body whose disc the waves mirror as their glint (water.frag's rtGlint):
/// the sun while it is up, the moon by night while she is (Wow.exe's sky
/// bodies, LightingParams::sunDir and moonDir), each faded in over its rise
/// from just under the horizon. `strength` 0: neither is up.
struct GlintBody {
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    float strength = 0.0f;
};
inline constexpr float kGlintRiseFrom = -0.01f;  ///< sine of the body's height where its glint starts
inline constexpr float kGlintRiseTo = 0.04f;     ///< and where it is whole
[[nodiscard]] inline GlintBody glintBody(const glm::vec3& sunDir, const glm::vec3& moonDir) {
    auto up = [](float z) {
        const float t = std::clamp((z - kGlintRiseFrom) / (kGlintRiseTo - kGlintRiseFrom), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    GlintBody g;
    if (glm::dot(sunDir, sunDir) > 1e-12f) {
        const glm::vec3 sun = glm::normalize(sunDir);
        g.dir = sun;
        g.strength = up(sun.z);
    }
    if (g.strength <= 0.0f && glm::dot(moonDir, moonDir) > 1e-12f) {
        const glm::vec3 moon = glm::normalize(moonDir);
        g.dir = moon;
        g.strength = up(moon.z);
    }
    return g;
}

/// GGX's distribution (water.frag's DistributionGGX), for the glint's peak.
[[nodiscard]] inline float ggx(const glm::vec3& n, const glm::vec3& h, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float nh = std::max(glm::dot(n, h), 0.0f);
    const float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d + 1e-7f);
}

/// How far the reflected and the refracted ray go, in yards. The same with
/// ray queries and with the compute tracer's walk of its BVH in the water's
/// shader (rt_trace_sw.glsli, where there are no ray queries: MoltenVK, iOS,
/// macOS and older GPUs): the water looks the same on both. The walk was
/// held to 150 and 40 yards, which cut a far shore out of the mirror.
struct Reach {
    float reflect = 400.0f;
    float refract = 80.0f;
};
[[nodiscard]] constexpr Reach reach(bool /*rayQueries*/) { return Reach{}; }

}  // namespace wowee::rendering::rt_water

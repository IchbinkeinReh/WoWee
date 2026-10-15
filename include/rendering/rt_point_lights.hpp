#pragma once

/// The point lights the ray traced lighting shades with: torches, braziers,
/// lamps and spell effects (M2 lights, pipeline::M2Light), and the omni
/// lights a building carries (WMO MOLT). The original client bakes the
/// building's into its vertex colours and links an M2's into a grid that
/// lights models near it (0x00834c70); it never lights the world with either
/// at runtime. RtLighting does, in its modes only, with a shadow ray to each.
/// What glows - emissive surfaces and particle emitters - gives lights of its
/// own beside these (rt_glow.hpp).
///
/// Gathered on the CPU every frame - from the instances that carry lights,
/// not from every instance - culled to what can reach the view, ranked, and
/// cut to what the shader's buffer holds (rt_point_lights in rt_trace.comp).
/// Pure: the parts here are what tests/test_rt_point_lights.cpp checks.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"

namespace wowee::rendering::rt_lights {

/// What gave a light, for the Debug Graphics settings to scale each kind on
/// its own (Renderer::LightingTune::sourceIntensity and sourceReach).
enum class Source : uint8_t {
    Model = 0,     // an M2 light of a doodad or game object
    Fire = 1,      // fireLight: a fire that carries none of its own
    Lamp = 2,      // lampLight: a lamp that carries none of its own
    Building = 3,  // a WMO's MOLT light
    Unit = 4,      // an M2 light of a character or creature
    Emissive = 5,  // glowLight: a model's or a building's glowing surfaces (rt_glow.hpp)
    Particle = 6,  // glowLight: a particle emitter's live particles (rt_glow.hpp)
};
inline constexpr size_t kSourceCount = 7;

struct PointLight {
    glm::vec3 position{0.0f};
    float attenuationStart = 0.0f;  // yards: full strength within
    glm::vec3 color{0.0f};          // linear, intensity folded in
    float attenuationEnd = 0.0f;    // yards: nothing beyond
    /// A building's own light (MOLT), which its interior already carries in
    /// its vertex colours (MOCV), its doodads in their MODD colour and the
    /// units on its floors in the floor's colour (0x007a0d60). Those
    /// surfaces take only its shadows; adding its light again would light
    /// them twice (rt_lighting.glsli, rtPointLightOwn and rtBakedShade).
    bool baked = false;
    Source source = Source::Model;
};

/// What the shader reads per light (std430): position and end, colour and
/// start. Must match RtPointLight in rt_trace.comp.glsl. A baked light's end
/// goes negative; an end is never zero (selectLights drops those).
struct PointLightGPU {
    glm::vec4 positionEnd;
    glm::vec4 colorStart;
};
static_assert(sizeof(PointLightGPU) == 32, "PointLightGPU is uploaded as std430");

inline PointLightGPU toGpu(const PointLight& l) {
    return {glm::vec4(l.position, l.baked ? -l.attenuationEnd : l.attenuationEnd),
            glm::vec4(l.color, l.attenuationStart)};
}

/// How much of a light reaches `distance` yards: all of it within the start,
/// none past the end, and a smooth ramp between - the window an artist set
/// for the light, which the client's own uses of it treat as linear
/// (0x007a0d60 for the interior lights: 1 - (d - start) / (end - start)). The
/// ramp is smoothed so the edge of a torch's reach does not show as a ring.
/// The shader's rtLightFalloff must agree.
inline float attenuation(float distance, float start, float end) {
    if (!(end > 0.0f) || distance >= end) return 0.0f;
    start = std::clamp(start, 0.0f, end);
    if (distance <= start) return 1.0f;
    const float t = (end - distance) / (end - start);
    return t * t * (3.0f - 2.0f * t);
}

/// A rough measure of what a light adds to the view from `eye`, to keep the
/// ones that matter when there are more than the buffer holds: its strength,
/// times how much of its reach it shows - full when the eye is inside it,
/// falling with the square of the distance outside.
inline float importance(const PointLight& l, const glm::vec3& eye) {
    const float strength = std::max(l.color.r, std::max(l.color.g, l.color.b));
    const float reach = std::max(l.attenuationEnd, 0.01f);
    const float outside = std::max(glm::length(l.position - eye) - reach, 0.0f);
    return strength * reach * reach / (reach * reach + outside * outside);
}

/// Keep the lights that can light something within `maxDistance` of the eye
/// and that `visible(position, radius)` says reach into the view, the
/// `maxLights` most important of them, most important first. Lights too faint
/// or too small to see go too. Returns the count kept.
template <class Visible>
size_t selectLights(std::vector<PointLight>& lights, const glm::vec3& eye, float maxDistance,
                    size_t maxLights, Visible&& visible) {
    constexpr float kMinStrength = 0.02f;
    constexpr float kMinReach = 0.25f;
    struct Ranked {
        float score;
        uint32_t index;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const PointLight& l = lights[i];
        const float strength = std::max(l.color.r, std::max(l.color.g, l.color.b));
        if (!(strength >= kMinStrength) || !(l.attenuationEnd >= kMinReach)) continue;
        if (!std::isfinite(l.position.x) || !std::isfinite(l.position.y) || !std::isfinite(l.position.z))
            continue;
        if (glm::length(l.position - eye) - l.attenuationEnd > maxDistance) continue;
        if (!visible(l.position, l.attenuationEnd)) continue;
        ranked.push_back({importance(l, eye), i});
    }
    const size_t kept = std::min(ranked.size(), maxLights);
    // Ties by index, so the same lights keep the same order from frame to
    // frame and the accumulated shadows do not shuffle.
    auto better = [](const Ranked& a, const Ranked& b) {
        return a.score != b.score ? a.score > b.score : a.index < b.index;
    };
    if (kept < ranked.size()) {
        std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(kept), ranked.end(), better);
        ranked.resize(kept);
    }
    std::sort(ranked.begin(), ranked.end(), better);
    std::vector<PointLight> out;
    out.reserve(kept);
    for (const Ranked& r : ranked) out.push_back(lights[r.index]);
    lights.swap(out);
    return lights.size();
}

/// The least reach an M2 light is given in the world, in yards (evaluateM2Light).
inline constexpr float kMinM2LightReach = 3.0f;
/// How much farther than its authored window a light reaches in the world.
/// The windows were set for lighting a model and its neighbours by vertex,
/// or a building's walls in its baked colours; on the ground beneath a
/// street lamp they stop short of the ground itself (Dalaran's lamps hang
/// nine and a half yards up with a reach of nine).
inline constexpr float kWorldReachScale = 1.5f;

/// An M2 light as it is this frame: the diffuse colour times its intensity,
/// the attenuation window, and the position carried by its bone into the
/// world. None for a directional light, one switched off by its visibility
/// track, or one with no light to give. `bones` may be empty (an unanimated
/// model); the radii scale with the model.
inline std::optional<PointLight> evaluateM2Light(const pipeline::M2Light& light, int sequence,
                                                 float timeMs, float globalTimeMs,
                                                 const std::vector<uint32_t>& globalSequenceDurations,
                                                 const glm::mat4& modelMatrix,
                                                 const std::vector<glm::mat4>& bones) {
    if (light.type != 1) return std::nullopt;
    const auto& gs = globalSequenceDurations;
    if (m2_track::sampleFloat(light.visibility, sequence, timeMs, globalTimeMs, gs, 1.0f) == 0.0f)
        return std::nullopt;
    const glm::vec3 color = m2_track::sampleVec3(light.diffuseColor, sequence, timeMs, globalTimeMs, gs,
                                                 glm::vec3(0.0f));
    const float intensity =
        m2_track::sampleFloat(light.diffuseIntensity, sequence, timeMs, globalTimeMs, gs, 0.0f);
    PointLight out;
    out.color = glm::max(color, glm::vec3(0.0f)) * std::max(intensity, 0.0f);
    if (std::max(out.color.r, std::max(out.color.g, out.color.b)) <= 0.0f) return std::nullopt;

    glm::mat4 toWorld = modelMatrix;
    if (light.bone >= 0 && static_cast<size_t>(light.bone) < bones.size()) {
        toWorld = toWorld * bones[static_cast<size_t>(light.bone)];
    }
    out.position = glm::vec3(toWorld * glm::vec4(light.position, 1.0f));
    const float scale = glm::length(glm::vec3(modelMatrix[0])) * kWorldReachScale;
    out.attenuationStart =
        std::max(m2_track::sampleFloat(light.attenuationStart, sequence, timeMs, globalTimeMs, gs, 0.0f), 0.0f) *
        scale;
    out.attenuationEnd =
        std::max(m2_track::sampleFloat(light.attenuationEnd, sequence, timeMs, globalTimeMs, gs, 0.0f), 0.0f) *
        scale;
    if (!(out.attenuationEnd > 0.0f)) return std::nullopt;
    // A torch's or a candle's window is often under a yard: what the client
    // lights with it is the model itself and the few models nearest, by
    // distance alone (the four nearest lights, 0x00834f60). Taken as it is it
    // would light nothing past the flame; the world gets at least this reach.
    out.attenuationEnd = std::max(out.attenuationEnd, kMinM2LightReach);
    return out;
}

/// The light of a fire that carries none of its own: a brazier, a bonfire, a
/// torch (the classifier's isBrazierOrFire and isTorch), whose glow the
/// original draws only as a card. At the top of its box, warm, reaching
/// some three times its size; flickering with `timeMs` and a phase of its
/// own so a row of torches does not breathe in step.
inline PointLight fireLight(const glm::vec3& boxMin, const glm::vec3& boxMax, const glm::mat4& modelMatrix,
                            bool torch, float timeMs, float phase) {
    const glm::vec3 size = boxMax - boxMin;
    const glm::vec3 local(0.5f * (boxMin.x + boxMax.x), 0.5f * (boxMin.y + boxMax.y), boxMax.z - 0.2f * size.z);
    const float scale = glm::length(glm::vec3(modelMatrix[0]));
    const float extent = std::max(std::max(size.x, size.y), size.z) * scale;
    PointLight out;
    out.position = glm::vec3(modelMatrix * glm::vec4(local, 1.0f));
    const float t = timeMs * 0.001f;
    const float flicker = 0.88f + 0.08f * std::sin(t * 7.3f + phase) + 0.04f * std::sin(t * 17.9f + 2.1f * phase);
    out.color = (torch ? glm::vec3(1.0f, 0.62f, 0.3f) : glm::vec3(1.0f, 0.55f, 0.22f)) * flicker;
    out.attenuationEnd = std::clamp(extent * 3.0f, torch ? 4.0f : 5.0f, 14.0f) * kWorldReachScale;
    out.attenuationStart = 0.25f * out.attenuationEnd;
    out.source = Source::Fire;
    return out;
}

/// The light of a lamp that carries none of its own: a street lamp, a
/// hanging lantern, a sconce (the classifier's isLanternLike). None of the
/// human lamps and lanterns has an M2 light (their files list none); the
/// original draws only their glowing glass. The light sits at `local`, the
/// middle of that glass in the model's space (lampGlowCentre), warm, reaching
/// some nine yards. Its glass glows at every hour, and so does its light.
inline constexpr float kLampReach = 6.0f;
inline PointLight lampLight(const glm::vec3& local, const glm::mat4& modelMatrix) {
    PointLight out;
    out.position = glm::vec3(modelMatrix * glm::vec4(local, 1.0f));
    const float scale = glm::length(glm::vec3(modelMatrix[0]));
    out.color = glm::vec3(1.0f, 0.78f, 0.48f);
    out.attenuationEnd = std::clamp(kLampReach * scale, 3.0f, 12.0f) * kWorldReachScale;
    out.attenuationStart = 0.2f * out.attenuationEnd;
    out.source = Source::Lamp;
    return out;
}

/// Where a lamp's light comes from in its model's space: the middle of what
/// glows - its unlit or additive batches, or its glow cards - weighted by
/// their vertices; the top of its box, as a fire's, when none does.
/// `glowing(i)` says whether index i of `indices` belongs to such a batch.
template <class Glowing>
glm::vec3 lampGlowCentre(const std::vector<glm::vec3>& positions, const std::vector<uint16_t>& indices,
                         Glowing&& glowing, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    glm::dvec3 sum(0.0);
    double count = 0.0;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= positions.size() || !glowing(i)) continue;
        sum += glm::dvec3(positions[indices[i]]);
        count += 1.0;
    }
    if (count > 0.0) return glm::vec3(sum / count);
    const glm::vec3 size = boxMax - boxMin;
    return {0.5f * (boxMin.x + boxMax.x), 0.5f * (boxMin.y + boxMax.y), boxMax.z - 0.2f * size.z};
}

/// A WMO's omni light (MOLT type 0) placed by its instance. Spot,
/// directional and ambient lights are only the baked lighting's.
inline std::optional<PointLight> placeWmoLight(const glm::vec3& localPosition, const glm::vec3& color,
                                               float intensity, float attenuationStart,
                                               float attenuationEnd, uint32_t type,
                                               const glm::mat4& modelMatrix) {
    if (type != 0) return std::nullopt;
    if (!(attenuationEnd > 0.0f) || !(intensity > 0.0f)) return std::nullopt;
    PointLight out;
    out.position = glm::vec3(modelMatrix * glm::vec4(localPosition, 1.0f));
    const float scale = glm::length(glm::vec3(modelMatrix[0])) * kWorldReachScale;
    out.color = glm::max(color, glm::vec3(0.0f)) * intensity;
    out.attenuationStart = std::max(attenuationStart, 0.0f) * scale;
    out.attenuationEnd = attenuationEnd * scale;
    out.baked = true;
    out.source = Source::Building;
    return out;
}

}  // namespace wowee::rendering::rt_lights

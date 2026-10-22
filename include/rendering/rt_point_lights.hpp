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
#include <string_view>
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
    /// A building's light: the storey whose baked colours alone it shades,
    /// from its floor to its ceiling in world z (rt_light_storey.hpp,
    /// Renderer gives it after selectLights). Without one, every baked colour.
    bool hasStorey = false;
    float storeyFloor = 0.0f;
    float storeyCeiling = 0.0f;
};

/// What the shader reads per light (std430): position and end, colour and
/// start, and the storey (floor, ceiling, 1 when it has one). Must match
/// RtPointLight in rt_trace.comp.glsl. A baked light's end goes negative; an
/// end is never zero (selectLights drops those). An open ceiling goes up as
/// a large finite height, which the shader's comparison takes alike.
struct PointLightGPU {
    glm::vec4 positionEnd;
    glm::vec4 colorStart;
    glm::vec4 storey;
};
static_assert(sizeof(PointLightGPU) == 48, "PointLightGPU is uploaded as std430");

inline constexpr float kOpenCeiling = 1e30f;

inline PointLightGPU toGpu(const PointLight& l) {
    return {glm::vec4(l.position, l.baked ? -l.attenuationEnd : l.attenuationEnd),
            glm::vec4(l.color, l.attenuationStart),
            l.hasStorey ? glm::vec4(l.storeyFloor, std::min(l.storeyCeiling, kOpenCeiling), 1.0f, 0.0f)
                        : glm::vec4(0.0f)};
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

/// What of a point light's light a surface takes for one shadow sample `vis`
/// at point shadow strength `strength` (Debug Graphics): mix(1, vis, s), so a
/// shadow lets 1 - s through - as rtGather and rt_apply's findTraced have it,
/// never below nothing.
inline float shadowedShare(float vis, float strength) {
    return std::max(1.0f + strength * (vis - 1.0f), 0.0f);
}

/// The shadow sample rt_trace writes for a light that the ground or a
/// building (RtScene::kRayStructure) stops: one that shadowedShare takes to
/// nothing at that strength, so no light comes through a floor or a wall
/// however soft the shadows are set - the Goldshire inn's cellar took 30% of
/// the hearth's flames seven yards above it at 70%. Below 0 as it must be;
/// the baked lamps' shadow takes it clamped to 0..1, a whole shadow. At
/// strength 0 there are no point shadows at all. rt_trace.comp.glsl's
/// rtHardShadowVis must agree.
inline float hardShadowVisibility(float strength) {
    return strength > 1e-3f ? (strength - 1.0f) / strength : 0.0f;
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

/// The bone that carries most of what glows - each glowing vertex's
/// heaviest bone, counted once per index - for a lamp on a chain, a lantern
/// that swings, whose glass the bones move away from its bind pose. ~0u when
/// nothing glows or no vertex has a weight. `glowing(i)` as lampGlowCentre's.
template <class Glowing>
uint32_t dominantBone(const std::vector<pipeline::M2Vertex>& vertices, const std::vector<uint16_t>& indices,
                      Glowing&& glowing) {
    std::vector<uint32_t> votes;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertices.size() || !glowing(i)) continue;
        const pipeline::M2Vertex& v = vertices[indices[i]];
        int best = -1;
        for (int k = 0; k < 4; ++k) {
            if (v.boneWeights[k] > 0 && (best < 0 || v.boneWeights[k] > v.boneWeights[best])) best = k;
        }
        if (best < 0) continue;
        const uint32_t bone = v.boneIndices[best];
        if (bone >= votes.size()) votes.resize(bone + 1, 0);
        ++votes[bone];
    }
    uint32_t bone = ~0u;
    uint32_t most = 0;
    for (uint32_t b = 0; b < votes.size(); ++b) {
        if (votes[b] > most) {
            most = votes[b];
            bone = b;
        }
    }
    return bone;
}

/// A model that is a flat sheet: half a yard or more across and no thicker
/// than a tenth of that (nor than 0.15 yards) - a rug, a doormat. It is kept
/// out of the ray traced scene as an occluder (M2Renderer::registerRtModel).
inline bool isFlatSheet(const std::vector<pipeline::M2Vertex>& vertices) {
    if (vertices.empty()) return false;
    glm::vec3 lo(vertices[0].position), hi(vertices[0].position);
    for (const auto& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    const glm::vec3 size = hi - lo;
    const float across = std::max(size.x, size.y);
    return across >= 0.5f && size.z < 0.15f && size.z < 0.1f * across;
}

/// The matrix that carries a point of a model's space into the world this
/// frame: the model's, times `bone`'s pose where the instance has one.
inline glm::mat4 boneCarried(const glm::mat4& modelMatrix, const std::vector<glm::mat4>& bones, uint32_t bone) {
    return bone < bones.size() ? modelMatrix * bones[bone] : modelMatrix;
}

/// For each bone, the bone that turns what it carries toward the camera:
/// itself where it is a spherical billboard (`billboard[b]`), else the
/// nearest of its parents that is (`parents[b]`, below 0 for a root); ~0u
/// where none is. A cycle in a broken file ends the walk.
inline std::vector<uint32_t> billboardOwners(const std::vector<int16_t>& parents, const std::vector<bool>& billboard) {
    std::vector<uint32_t> owners(parents.size(), ~0u);
    for (size_t b = 0; b < parents.size(); ++b) {
        size_t at = b;
        for (size_t hops = 0; hops <= parents.size(); ++hops) {
            if (at < billboard.size() && billboard[at]) {
                owners[b] = static_cast<uint32_t>(at);
                break;
            }
            if (parents[at] < 0 || static_cast<size_t>(parents[at]) >= parents.size()) break;
            at = static_cast<size_t>(parents[at]);
        }
    }
    return owners;
}

/// boneCarried for what the traced lighting places - a light, a piece of a
/// model that casts - the same from wherever the camera looks at it. A
/// billboard bone (`billboard`, from billboardOwners; ~0u for none) turns
/// its card to face the camera about its `pivot` every frame, and a light
/// at the card or a piece hanging from it went round with the camera: a
/// Hallow's End skull lantern's glow, a few tenths of a yard off its pivot,
/// swung its light about the beam it hangs from, and the beam's shadow
/// turned across the ground as the camera moved. Such a point is carried
/// to where the bones take the pivot - which the turn leaves in place -
/// and keeps its authored offset from it, turned only by the model.
inline glm::mat4 steadyBoneCarried(const glm::mat4& modelMatrix, const std::vector<glm::mat4>& bones, uint32_t bone,
                                   uint32_t billboard, const glm::vec3& pivot) {
    if (billboard < bones.size()) {
        const glm::vec3 at = glm::vec3(bones[billboard] * glm::vec4(pivot, 1.0f));
        glm::mat4 shift(1.0f);
        shift[3] = glm::vec4(at - pivot, 1.0f);
        return modelMatrix * shift;
    }
    return boneCarried(modelMatrix, bones, bone);
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

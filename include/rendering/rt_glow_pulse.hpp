#pragma once
/// A glow card's pulse, for the ray traced light it gives (user164, user
/// decision 2026-11-11). Many glow cards pulse by their bone's scale track,
/// not by a colour or transparency track: Brill's street lamp
/// (tirisfallstreetlamp01.m2) scales its GLOWGREEN32 card's bone 1.0, 1.12,
/// 0.96, 1.16, 1.0 over a second of its loop, and a torch's glow card
/// (club_1h_torch_a_01.m2) does the same. The card's light follows it: times
/// the area the bone's matrix gives it this frame against its rest - the
/// bone's matrix with its parents', as the frame posed it, nothing kept
/// from earlier frames.
///
/// Found by the tracks alone: a bone pulses when it or one of its parents has
/// a scale track with more than one key in some sequence. Colour and
/// transparency tracks are followed already (GlowPatch::animatedBatch).
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "pipeline/m2_loader.hpp"

namespace wowee::rendering::rt_lights {

/// Whether any sequence of `track` holds more than one key.
inline bool trackMoves(const pipeline::M2AnimationTrack& track) {
    for (const auto& keys : track.sequences.records()) {
        if (keys.timestamps.size() > 1) return true;
    }
    return false;
}

/// Per bone, whether its scale - its own or a parent's - moves.
inline std::vector<uint8_t> pulsingBones(const std::vector<pipeline::M2Bone>& bones) {
    std::vector<uint8_t> out(bones.size(), 0);
    for (size_t i = 0; i < bones.size(); ++i) {
        // Parents come before their children in an M2's bone list, but walk
        // the chain anyway, bounded, in case a file does not keep to that.
        int b = static_cast<int>(i);
        for (size_t hops = 0; b >= 0 && static_cast<size_t>(b) < bones.size() && hops <= bones.size(); ++hops) {
            if (trackMoves(bones[static_cast<size_t>(b)].scale)) {
                out[i] = 1;
                break;
            }
            b = bones[static_cast<size_t>(b)].parentBone;
        }
    }
    return out;
}

/// The pulsing bone that carries most of a model's glowing batches, by the
/// file alone - for a worn or held item, whose batches the character
/// renderer draws: an added batch (blend 3, 4) or an unlit blended one (flag
/// 0x1, blend 2 to 4), as rt_lights::batchGlowColor takes them. ~0 for none.
inline uint32_t cardPulseBone(const pipeline::M2Model& model, const std::vector<uint8_t>& pulsing) {
    std::vector<float> area(pulsing.size(), 0.0f);
    bool any = false;
    for (const auto& batch : model.batches) {
        if (batch.materialIndex >= model.materials.size()) continue;
        const auto& mat = model.materials[batch.materialIndex];
        const bool added = mat.blendMode == 3 || mat.blendMode == 4;
        const bool unlitBlend = (mat.flags & 0x1u) != 0 && mat.blendMode >= 2 && mat.blendMode <= 4;
        if (!added && !unlitBlend) continue;
        const size_t end = std::min<size_t>(size_t(batch.indexStart) + batch.indexCount, model.indices.size());
        for (size_t i = batch.indexStart; i + 2 < end; i += 3) {
            const uint16_t a = model.indices[i], b = model.indices[i + 1], c = model.indices[i + 2];
            if (a >= model.vertices.size() || b >= model.vertices.size() || c >= model.vertices.size()) continue;
            const auto& va = model.vertices[a];
            int heaviest = -1;
            for (int k = 0; k < 4; ++k) {
                if (va.boneWeights[k] > 0 && (heaviest < 0 || va.boneWeights[k] > va.boneWeights[heaviest])) heaviest = k;
            }
            if (heaviest < 0) continue;
            const uint32_t bone = va.boneIndices[heaviest];
            if (bone >= pulsing.size() || !pulsing[bone]) continue;
            area[bone] += 0.5f * glm::length(glm::cross(model.vertices[b].position - model.vertices[a].position,
                                                        model.vertices[c].position - model.vertices[a].position));
            any = true;
        }
    }
    if (!any) return ~0u;
    uint32_t best = ~0u;
    float bestArea = 0.0f;
    for (size_t i = 0; i < area.size(); ++i) {
        if (area[i] > bestArea) {
            bestArea = area[i];
            best = static_cast<uint32_t>(i);
        }
    }
    return best;
}

/// A pulse's factor with its swing about 1 scaled (Debug Glow, "Glow pulse
/// strength"): 1 + (raw - 1) x strength, never below nothing - the mean
/// kept, the swing grown or shrunk, 0 a steady light.
inline float scaledPulse(float raw, float strength) {
    return std::max(1.0f + (raw - 1.0f) * strength, 0.0f);
}

/// A track's value with its swing about the track's own mean scaled (Debug
/// Glow's strengths): mean + (raw - mean) x strength, never below nothing.
inline float scaledAboutMean(float raw, float mean, float strength) {
    return std::max(mean + (raw - mean) * strength, 0.0f);
}
inline glm::vec3 scaledAboutMean(const glm::vec3& raw, const glm::vec3& mean, float strength) {
    return glm::max(mean + (raw - mean) * strength, glm::vec3(0.0f));
}

/// How much larger in area a bone's matrix draws what it carries than at
/// rest: |det|^(2/3) of its linear part, 1 for a matrix that only turns and
/// moves.
inline float boneAreaFactor(const glm::mat4& bone) {
    const float det = std::fabs(glm::determinant(glm::mat3(bone)));
    if (!(det > 0.0f) || !std::isfinite(det)) return 1.0f;
    return std::pow(det, 2.0f / 3.0f);
}

}  // namespace wowee::rendering::rt_lights

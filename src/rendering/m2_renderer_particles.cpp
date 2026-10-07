#include "rendering/m2_renderer.hpp"
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <random>
#include <limits>
#include <set>

namespace wowee {
namespace rendering {

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    // The same list, for the same reason; see renderM2Particles. A ribbon
    // emitter is a particle emitter as far as this list is concerned.
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& inst = instances[idx];
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

namespace {

/// TEMPORARY tuning for the emitters drawing fire or magic, read once from the
/// environment so the right values can be found without rebuilding:
/// WOWEE_PFX_RATE scales how many are emitted, WOWEE_PFX_GAIN how bright the
/// additive ones are, WOWEE_PFX_SIZE how large they are drawn.
float pfxTuning(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return 1.0f;
    const float f = static_cast<float>(std::atof(v));
    return f > 0.0f ? f : 1.0f;
}
const float kPfxRate = pfxTuning("WOWEE_PFX_RATE");
const float kPfxSize = pfxTuning("WOWEE_PFX_SIZE");

} // namespace

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;
        // The emitter's own on/off track (client FUN_0082d2f0): while it is
        // off nothing new is emitted, and what is already alive plays out.
        if (m2_track::sampleFloat(em.enabledTrack, inst.currentSequenceIndex, inst.animTime,
                                  inst.globalSequenceTime, gpu.globalSequenceDurations,
                                  1.0f) < 0.5f) {
            continue;
        }

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        // The emission rate as the client takes it: the track's value plus a
        // roll of the emitter's spread (FUN_0097d8c0), times the density the
        // player asked for. Nothing raises it beyond what the model authors.
        rate += distN(particleRng_) * em.emissionRateVary;
        rate *= particleDensity_;
        if (ei < gpu.particleSkipGenericDimming.size() && gpu.particleSkipGenericDimming[ei] != 0) {
            rate *= kPfxRate;
        }

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(gpu.name).second) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            // The lifespan and the size are rolled once, as the client does
            // when it makes a particle (FUN_00979e90): the track's value plus
            // the emitter's spread times a random in -1..1; the size is
            // multiplied by 1 plus the scale spread times another, never
            // below a tenth of a thousandth.
            p.maxLife = std::max(life + distN(particleRng_) * em.lifespanVary, 0.001f);
            p.sizeVary = std::max(1.0f + distN(particleRng_) * em.scaleVary.x, 0.0001f);
            p.tileIndex = 0.0f;

            // Position: emitter position transformed by bone matrix
            glm::vec3 localPos = em.position;
            // A plane emitter scatters its particles across a rectangle rather
            // than letting them all leave from one point. Left out, the demon
            // crystal's fire was a single thin thread of sprites down its
            // middle where the model draws a broad soft cloud. Kept to the
            // emitters drawing fire or magic, like the other corrections here.
            if (em.emitterType == 1 && ei < gpu.particleSkipGenericDimming.size() &&
                gpu.particleSkipGenericDimming[ei] != 0) {
                const float areaLength = interpFloat(em.emissionAreaLength, inst.animTime,
                                                     inst.globalSequenceTime,
                                                     inst.currentSequenceIndex,
                                                     gpu.globalSequenceDurations);
                const float areaWidth = interpFloat(em.emissionAreaWidth, inst.animTime,
                                                    inst.globalSequenceTime,
                                                    inst.currentSequenceIndex,
                                                    gpu.globalSequenceDurations);
                // Half the length and half the width either side of the emitter.
                localPos.x += distN(particleRng_) * areaLength * 0.5f;
                localPos.y += distN(particleRng_) * areaWidth * 0.5f;
            }
            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            glm::vec3 worldPos = glm::vec3(inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
            p.position = worldPos;

            // Velocity: emission speed in upward direction + random spread
            float speed = interpFloat(em.emissionSpeed, inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations);
            // (roll * speed spread + 1) * speed, as the client's FUN_009792d0.
            speed *= 1.0f + distN(particleRng_) *
                     interpFloat(em.speedVariation, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
            float vRange = interpFloat(em.verticalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations);
            float hRange = interpFloat(em.horizontalRange, inst.animTime, inst.globalSequenceTime,
                                       inst.currentSequenceIndex, gpu.globalSequenceDurations);

            // Base direction: up in model space, transformed to world
            glm::vec3 dir(0.0f, 0.0f, 1.0f);
            if (hRange > 1.0f) {
                // A horizontal range this wide is an azimuth sweep, not a
                // spread: 6.283 is a full turn around the emission axis, and
                // the vertical range is how far off that axis a particle may
                // lean. Read as the width of a random offset it threw the
                // demon crystal's flames out sideways at nearly right angles,
                // when the model sends them almost straight up.
                const float tilt = dist01(particleRng_) * std::abs(vRange);
                const float azimuth = dist01(particleRng_) * hRange;
                dir = glm::vec3(std::sin(tilt) * std::cos(azimuth),
                                std::sin(tilt) * std::sin(azimuth),
                                std::cos(tilt));
            } else {
                // Add random spread
                dir.x += distN(particleRng_) * hRange;
                dir.y += distN(particleRng_) * hRange;
                dir.z += distN(particleRng_) * vRange;
            }
            float lenSq = glm::dot(dir, dir);
            if (lenSq > 0.001f * 0.001f) dir *= glm::inversesqrt(lenSq);

            // Transform direction by bone + model orientation (rotation only)
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);
            p.velocity = rotMat * dir * speed;

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            // A random cell only when the emitter asks (flag 0x100000); otherwise
            // the cell comes from the emitter's head-cell track as the particle
            // ages, or is the first (FUN_00979e90).
            if ((em.flags & 0x100000) && totalTiles > 1) {
                distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                p.tileIndex = static_cast<float>(distTile(particleRng_));
            }

            inst.particles.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (gpu.isSpellEffect && inst.particles.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Hoist per-emitter gravity out of the per-particle loop. Gravity (and the
    // emissionSpeed fallback) depends only on the emitter and animation time -
    // not on the particle itself - so interpFloat was being re-evaluated for
    // every particle even when 100s of particles share one emitter.
    constexpr size_t kMaxStackEmitters = 16;
    float emitterGravStack[kMaxStackEmitters];
    std::vector<float> emitterGravHeap;
    const size_t numEm = gpu.particleEmitters.size();
    float* emitterGrav = nullptr;
    if (numEm > 0) {
        if (numEm <= kMaxStackEmitters) {
            emitterGrav = emitterGravStack;
        } else {
            emitterGravHeap.resize(numEm);
            emitterGrav = emitterGravHeap.data();
        }
        for (size_t e = 0; e < numEm; ++e) {
            const auto& pem = gpu.particleEmitters[e];
            // The gravity track's value, zero included (FUN_00979bb0 applies
            // +0xb4 as it is).
            const float grav = interpFloat(pem.gravity,
                                           inst.animTime, inst.globalSequenceTime,
                                           inst.currentSequenceIndex, gpu.globalSequenceDurations);
            emitterGrav[e] = grav;
        }
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        // FUN_00979bb0: the move over the step with gravity's half-square
        // term, then gravity on the velocity.
        const float g = (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm)
                            ? emitterGrav[p.emitterIndex] : 0.0f;
        p.position += p.velocity * dt;
        p.position.z -= g * dt * dt * 0.5f;
        p.velocity.z -= g * dt;
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    // Grow per-instance state arrays if needed
    if (inst.ribbonEdges.size() != emitters.size()) {
        inst.ribbonEdges.resize(emitters.size());
    }
    if (inst.ribbonEdgeAccumulators.size() != emitters.size()) {
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    }

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges    = inst.ribbonEdges[ri];
        auto& accum    = inst.ribbonEdgeAccumulators[ri];

        // Determine bone world position for spine
        glm::vec3 spineWorld = inst.position;
        // Use referenced bone; fall back to bone 0 if out of range (common for spell effects
        // where ribbon bone fields may be unset/garbage, e.g. bone=4294967295)
        uint32_t boneIdx = em.bone;
        if (boneIdx >= inst.boneMatrices.size() && !inst.boneMatrices.empty()) {
            boneIdx = 0;
        }
        if (boneIdx < inst.boneMatrices.size()) {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * inst.boneMatrices[boneIdx] * local);
        } else {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * local);
        }

        // Skip emitters that produce NaN positions (garbage bone/position data)
        if (std::isnan(spineWorld.x) || std::isnan(spineWorld.y) || std::isnan(spineWorld.z))
            continue;

        // Evaluate animated tracks (use first available sequence key, or fallback value)
        auto getFloatVal = [&](const pipeline::M2AnimationTrack& track, float fallback) -> float {
            for (const auto& seq : track.sequences) {
                if (!seq.floatValues.empty()) return seq.floatValues[0];
            }
            return fallback;
        };
        auto getVec3Val = [&](const pipeline::M2AnimationTrack& track, glm::vec3 fallback) -> glm::vec3 {
            for (const auto& seq : track.sequences) {
                if (!seq.vec3Values.empty()) return seq.vec3Values[0];
            }
            return fallback;
        };

        float visibility  = getFloatVal(em.visibilityTrack, 1.0f);
        float heightAbove = getFloatVal(em.heightAboveTrack, 0.5f);
        float heightBelow = getFloatVal(em.heightBelowTrack, 0.5f);
        glm::vec3 color   = getVec3Val(em.colorTrack, glm::vec3(1.0f));
        float alpha       = getFloatVal(em.alphaTrack, 1.0f);

        // Age existing edges and remove expired ones
        for (auto& e : edges) {
            e.age += dt;
            // Apply gravity
            if (em.gravity != 0.0f) {
                e.worldPos.z -= em.gravity * dt * dt * 0.5f;
            }
        }
        while (!edges.empty() && edges.front().age >= em.edgeLifetime) {
            edges.pop_front();
        }

        // Emit new edges based on edgesPerSecond
        if (visibility > 0.5f) {
            accum += em.edgesPerSecond * dt;
            while (accum >= 1.0f) {
                accum -= 1.0f;
                M2Instance::RibbonEdge e;
                e.worldPos    = spineWorld;
                e.color       = color;
                e.alpha       = alpha;
                // Scaled into world units here, where the instance is at hand.
                // The spine is a world position the model matrix produced, and
                // these two were model-space lengths added straight to it - so
                // a doodad placed at any scale but 1 got a trail the right
                // length and the wrong width.
                e.heightAbove = heightAbove * inst.scale;
                e.heightBelow = heightBelow * inst.scale;
                e.age         = 0.0f;
                edges.push_back(e);

                // Diagnostic: log first ribbon edge per spell effect instance+emitter
                if (gpu.isSpellEffect && edges.size() == 1) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name,
                             "' emitter=", ri, " pos=(", spineWorld.x, ",", spineWorld.y,
                             ",", spineWorld.z, ") hA=", heightAbove, " hB=", heightBelow,
                             " vis=", visibility, " eps=", em.edgesPerSecond,
                             " edgeLife=", em.edgeLifetime, " bone=", em.bone);
                }

                // Cap trail length
                if (edges.size() > 128) edges.pop_front();
            }
        } else {
            accum = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    // A ribbon's width runs across the trail and across the view, and it has to
    // be computed per edge from the direction the trail is actually going.
    //
    // This used a fixed world Z, which is only right for a trail travelling
    // horizontally. A flame licks upward, so its spine ran along the very axis
    // the strip was being widened on: the top and bottom vertices of every
    // edge landed on the spine itself, the quad collapsed, and the whole trail
    // drew as one tall thin sliver with the fire texture smeared up it. That
    // is the bonfire at Grom'gol standing four times the height of the huts
    // behind it.
    const glm::vec3 camPos = cachedCamPos_;
    const glm::vec3 upWorld(0.0f, 0.0f, 1.0f);

    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    for (const auto& inst : instances) {
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty()) continue;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;

            const auto& em = gpu.ribbonEmitters[ri];

            // Select blend pipeline based on material blend mode
            bool additive = false;
            if (em.materialIndex < gpu.batches.size()) {
                additive = (gpu.batches[em.materialIndex].blendMode >= 3);
            }
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom)
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                float t = (em.edgeLifetime > 0.0f)
                          ? 1.0f - (e.age / em.edgeLifetime) : 1.0f;
                float a = e.alpha * t;
                float u = static_cast<float>(ei) / static_cast<float>(edges.size() - 1);

                // The trail's own direction here, from the edges either side.
                const glm::vec3& prev = edges[ei > 0 ? ei - 1 : ei].worldPos;
                const glm::vec3& next = edges[ei + 1 < edges.size() ? ei + 1 : ei].worldPos;
                glm::vec3 spineDir = next - prev;
                float spineLen = glm::length(spineDir);
                // Widen across the trail and across the line of sight. Where
                // the trail doubles back on itself or points straight at the
                // eye there is no such direction, and world up is as good an
                // answer as any.
                glm::vec3 side = upWorld;
                if (spineLen > 1e-5f) {
                    glm::vec3 toEye = camPos - e.worldPos;
                    glm::vec3 cross = glm::cross(spineDir / spineLen, toEye);
                    float crossLen = glm::length(cross);
                    if (crossLen > 1e-5f) side = cross / crossLen;
                }

                // Top vertex (one side of the spine)
                glm::vec3 top = e.worldPos + side * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f; // v = top
                written++;

                // Bottom vertex (the other side)
                glm::vec3 bot = e.worldPos - side * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f; // v = bottom
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : instances) {
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbonEdge : inst.ribbonEdges) {
                    if (ribbonEdge.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbonEdge.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!particlePipeline_ || !m2ParticleVB_) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Collect all particles from all instances, grouped by texture+blend.
    // Reuse persistent map - keep the bucket structure, drop last frame's set.
    for (auto& [k, g] : particleGroups_) {
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    // Written straight into the mapped buffer rather than accumulated into a
    // vector per group and copied in afterwards; see ParticleRun.
    if (!m2ParticleVBMapped_) return;
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_);
    uint32_t vbWritten = 0;
    particleRuns_.clear();
    ParticleGroup* runGroup = nullptr;

    size_t totalParticles = 0;

    // Only the instances that carry emitters, not every instance in the world.
    //
    // particleInstanceIndices_ is built and maintained for exactly this and
    // the update path above already uses it; this pass walked all 66211
    // instances instead, asking each whether its particle vector was empty.
    // That walk was 2.0ms of a 16ms frame - two thirds of the M2 worker,
    // which is the critical path of renderWorld.
    particleDrawOrder_.clear();
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        const auto& candidate = instances[idx];
        if (candidate.particles.empty() || !candidate.cachedModel) continue;
        const glm::vec3 toCam = candidate.position - cachedCamPos_;
        particleDrawOrder_.emplace_back(glm::dot(toCam, toCam), idx);
    }
    std::sort(particleDrawOrder_.begin(), particleDrawOrder_.end());
    size_t droppedParticles = 0;

    for (const auto& [instDistSq, idx] : particleDrawOrder_) {
        auto& inst = instances[idx];
        const auto& gpu = *inst.cachedModel;


        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        // The display's ParticleColor slot this emitter takes its colours
        // from (client FUN_00825410 / FUN_0097a990), or null for its own.
        const glm::vec3* cachedColorOverride = nullptr;
        pipeline::M2FBlock cachedOverrideBlock;
        ParticleGroup* cachedGroup = nullptr;

        // How far this instance's particles actually reach, against how far
        // the model says it extends. A fire twice the height of the hut behind
        // it is either particles outliving their authored lifespan, flying at
        // the wrong speed, or drawn at the wrong size - and none of those says
        // which model it is. This does.
        float highestParticleZ = -std::numeric_limits<float>::max();
        float widestParticle = 0.0f;

        // WOWEE_M2_PARTICLE_DIAG=<name substring>: once every two seconds, what
        // each emitter of a matching model has alive - how many, how bright,
        // how big on screen, and how far above and below its instance.
        static const std::string kParticleDiag = [] {
            const char* v = std::getenv("WOWEE_M2_PARTICLE_DIAG");
            std::string t = v ? v : "";
            std::transform(t.begin(), t.end(), t.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return t;
        }();
        struct EmitterDiag { uint32_t n = 0; float alpha = 0, size = 0, minZ = 1e9f, maxZ = -1e9f; };
        std::vector<EmitterDiag> emitterDiag;
        if (!kParticleDiag.empty()) {
            std::string lower = gpu.name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // Several names may be given, separated by commas.
            size_t from = 0;
            while (from <= kParticleDiag.size()) {
                size_t to = kParticleDiag.find(',', from);
                if (to == std::string::npos) to = kParticleDiag.size();
                const std::string name = kParticleDiag.substr(from, to - from);
                if (!name.empty() && lower.find(name) != std::string::npos) {
                    emitterDiag.resize(gpu.particleEmitters.size());
                    break;
                }
                from = to + 1;
            }
        }

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];
                cachedColorOverride = (inst.hasParticleColors &&
                                       cachedEm->particleColorIndex >= 11 &&
                                       cachedEm->particleColorIndex <= 13)
                    ? &inst.particleColors[(cachedEm->particleColorIndex - 11u) * 3u] : nullptr;
                if (cachedColorOverride) {
                    // The record's start, mid and end replace the emitter's
                    // three colour keys, over the emitter's own key times.
                    cachedOverrideBlock.timestamps = cachedEm->particleColor.timestamps.size() == 3
                        ? cachedEm->particleColor.timestamps : std::vector<float>{0.0f, 0.5f, 1.0f};
                    cachedOverrideBlock.vec3Values.assign(cachedColorOverride, cachedColorOverride + 3);
                }

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            if (cachedColorOverride) color = interpFBlockVec3(cachedOverrideBlock, lifeRatio);
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            float rawScale = interpFBlockFloat(em.particleScale, lifeRatio);

            // The authored colour, alpha and size, as they are: the client puts
            // no floor under a particle's colour or alpha, no cap or floor on its
            // size, and no gain on an emitter, and drew every particle the same
            // whatever model it belongs to. Each of those was here to make a
            // point sprite that was too small, and a quad is the size it is.
            const float scale = rawScale;

            if (vbWritten >= MAX_M2_PARTICLE_VERTS) {
                droppedParticles += 1;
                continue;
            }
            // A run per stretch of particles sharing a group. The group only
            // changes when the emitter does, and particles from one emitter
            // are adjacent, so this closes a run about once per emitter.
            if (cachedGroup != runGroup) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0});
            }
            highestParticleZ = std::max(highestParticleZ, p.position.z);
            if (!emitterDiag.empty()) {
                auto& dg = emitterDiag[static_cast<size_t>(p.emitterIndex)];
                ++dg.n;
                dg.alpha += alpha;
                dg.size += scale * inst.scale;
                dg.minZ = std::min(dg.minZ, p.position.z - inst.position.z);
                dg.maxZ = std::max(dg.maxZ, p.position.z - inst.position.z);
                // The first particle of each emitter, whole, once in a while:
                // what is drawn and from what.
                static float lastDump = -10.0f;
                const float nowDump = std::chrono::duration<float>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (dg.n == 1 && nowDump - lastDump > 4.0f) {
                    if (static_cast<size_t>(p.emitterIndex) + 1 == gpu.particleEmitters.size()) {
                        lastDump = nowDump;
                    }
                    LOG_WARNING("PFX FIRST '", gpu.name, "' emitter=", p.emitterIndex,
                                " at=(", p.position.x - inst.position.x, ",",
                                p.position.y - inst.position.y, ",",
                                p.position.z - inst.position.z, ")",
                                " life=", p.life, "/", p.maxLife,
                                " color=(", color.r, ",", color.g, ",", color.b, ")",
                                " alpha=", alpha, " half=", scale * p.sizeVary,
                                " tile=", p.tileIndex, " tiles=", cachedTilesX, "x", cachedTilesY,
                                " tex=", (cachedTex && cachedTex->isValid()) ? "ok" : "INVALID",
                                " texSet=", (cachedGroup && cachedGroup->preAllocSet != VK_NULL_HANDLE)
                                                ? "ok" : "NULL",
                                " blend=", static_cast<int>(cachedBlendType),
                                " flags=0x", std::hex, cachedEm->flags, std::dec);
                }
            }
            widestParticle = std::max(widestParticle, scale);

            float* vd = vbBase + static_cast<size_t>(vbWritten) * 9;
            vd[0] = p.position.x;
            vd[1] = p.position.y;
            vd[2] = p.position.z;
            vd[3] = color.r;
            vd[4] = color.g;
            vd[5] = color.b;
            vd[6] = alpha;
            // The quad is a size either side of the centre, so this is half its
            // width: the scale, times the particle's own roll of the spread, and
            // the model's scale where the emitter is flagged 0x400.
            const float modelScale = (cachedEm->flags & 0x400) ? inst.scale : 1.0f;
            vd[7] = scale * p.sizeVary * modelScale * kPfxSize;
            // The cell of the atlas: the head-cell track at this point of the
            // particle's life, rounded (FUN_00979560), whatever the emitter's
            // flags say; without one, the cell rolled at birth.
            float tileIndex = p.tileIndex;
            if (cachedTotalTiles > 1 && !em.headCell.floatValues.empty()) {
                tileIndex = std::round(interpFBlockFloat(em.headCell, lifeRatio));
            }
            tileIndex = std::clamp(tileIndex, 0.0f, static_cast<float>(cachedTotalTiles) - 1.0f);
            vd[8] = tileIndex;
            ++vbWritten;
            ++particleRuns_.back().count;
            totalParticles++;
        }

        if (!emitterDiag.empty()) {
            // Per instance, so every one of a model's instances says what it has.
            static std::unordered_map<uint32_t, float> lastDiagByInstance;
            float& lastDiagSeconds = lastDiagByInstance.try_emplace(inst.id, -10.0f).first->second;
            const float now = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - lastDiagSeconds > 2.0f) {
                lastDiagSeconds = now;
                for (size_t ei = 0; ei < emitterDiag.size(); ++ei) {
                    const auto& dg = emitterDiag[ei];
                    const float n = std::max<float>(1.0f, static_cast<float>(dg.n));
                    LOG_WARNING("PARTICLE DIAG '", gpu.name, "' inst=", inst.id, " go=", inst.isGameObject ? 1 : 0, " pos=(", inst.position.x, ",", inst.position.y, ",", inst.position.z, ") scale=", inst.scale, " emitter=", ei,
                                " alive=", dg.n, " avgAlpha=", dg.alpha / n,
                                " avgSize=", dg.size / n, " z=[", dg.minZ, ",", dg.maxZ, "]",
                                " blend=", static_cast<int>(gpu.particleEmitters[ei].blendingType),
                                " skipDim=", ei < gpu.particleSkipGenericDimming.size()
                                                 ? static_cast<int>(gpu.particleSkipGenericDimming[ei]) : -1);
                }
            }
        }

        // Said once per model, on an absolute reach rather than a ratio
        // against the model's own bounds. A bonfire's authored box is 25 to 32
        // yards tall - it has to hold the flame's full extent - so a ratio
        // test can never trip on the one model anybody is complaining about.
        // Nothing that stands on the ground should be throwing particles eight
        // yards into the air.
        if (highestParticleZ > -std::numeric_limits<float>::max()) {
            const float reach = highestParticleZ - inst.position.z;
            if (reach > 8.0f) {
                static std::set<std::string> saidTall;
                if (saidTall.insert(gpu.name).second) {
                    LOG_WARNING("Particles reach ", reach, " yd above '", gpu.name,
                                "' (model bound ", gpu.boundMax.z, " yd, scale ", inst.scale,
                                ", ", gpu.particleEmitters.size(), " emitters, ",
                                inst.particles.size(), " live, largest ", widestParticle, " yd)");
                }
            }
        }
    }

    if (droppedParticles > 0) {
        static float lastSaid = -30.0f;
        const float nowSaid = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowSaid - lastSaid > 10.0f) {
            lastSaid = nowSaid;
            LOG_WARNING("Particle buffer full: ", droppedParticles,
                        " of the furthest particles not drawn this frame (", vbWritten, " drawn)");
        }
    }

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : instances) {
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& run : particleRuns_) {
        if (run.count == 0 || !run.group) continue;
        ParticleGroup& group = *run.group;

        uint8_t blendType = group.blendType;
        // The client's blend for each M2 blend type (table 0x00a453b0 into the
        // Gx factors): 4 adds by its alpha, 3 adds outright, 5 and 6 modulate.
        VkPipeline desiredPipeline = particlePipeline_;
        switch (blendType) {
            case 3: desiredPipeline = particleNoAlphaAddPipeline_; break;
            case 4: desiredPipeline = particleAdditivePipeline_; break;
            case 5: desiredPipeline = particleModPipeline_; break;
            case 6: desiredPipeline = particleMod2xPipeline_; break;
            default: break;
        }
        if (!desiredPipeline) desiredPipeline = particlePipeline_;
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            // Fallback: allocate per-frame (pool exhaustion risk - should not happen in practice)
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = (group.texture && group.texture->isValid())
                    ? group.texture : whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        // No texture set (the pool was full): drawing without one reads an
        // unbound set, which loses the device.
        if (texSet == VK_NULL_HANDLE) continue;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &texSet, 0, nullptr);

        // Push constants: tileCount + alphaKey
        struct { float tileX, tileY; int alphaKey; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        // A quad of four vertices for each particle of the run.
        vkCmdDraw(cmd, 4, run.count, 0, run.first);
    }
}

} // namespace rendering
} // namespace wowee

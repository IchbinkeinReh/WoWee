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

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }
    if (inst.emitterSplineEnd.size() != gpu.particleEmitters.size()) {
        inst.emitterSplineEnd.assign(gpu.particleEmitters.size(), 0.0f);
        inst.emitterSplinePin.assign(gpu.particleEmitters.size(), 0);
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

            const auto track = [&](const pipeline::M2AnimationTrack& t) {
                return interpFloat(t, inst.animTime, inst.globalSequenceTime,
                                   inst.currentSequenceIndex, gpu.globalSequenceDurations);
            };
            // Speed: the track's value times 1 + a roll of the speed spread
            // (0x009792d0).
            float speed = track(em.emissionSpeed);
            speed *= 1.0f + distN(particleRng_) * track(em.speedVariation);
            const float vRange = track(em.verticalRange);
            const float hRange = track(em.horizontalRange);
            const float zSource = m2_particle::zSourceValue(track(em.zSource));

            // Where in the emitter's own space the particle starts, and its
            // direction there - one function per shape.
            glm::vec3 offset(0.0f);
            glm::vec3 dir(0.0f, 0.0f, 1.0f);
            if (em.emitterType == 2) {
                // Sphere (0x00981950): on a shell between the area's length
                // and width, at an elevation of -1..1 times the vertical
                // range and an azimuth of -1..1 times the horizontal range;
                // out from the centre, straight up under flag 0x8000, or away
                // from zSource.
                const float areaLength = track(em.emissionAreaLength);
                const float areaWidth = track(em.emissionAreaWidth);
                const float radius = areaLength + dist01(particleRng_) * (areaWidth - areaLength);
                const float pol = distN(particleRng_) * vRange;
                const float az = distN(particleRng_) * hRange;
                const glm::vec3 out(std::cos(az) * std::cos(pol),
                                    std::sin(az) * std::cos(pol),
                                    std::sin(pol));
                offset = out * radius;
                if (zSource != 0.0f) {
                    dir = m2_particle::sphereZSourceDirection(offset, zSource);
                } else if (em.flags & 0x8000) {
                    dir = glm::vec3(0.0f, 0.0f, 1.0f);
                } else {
                    dir = out;
                }
            } else if (em.emitterType == 3) {
                // Spline (0x00981d40): somewhere between the start (length
                // track, 0x00981c90) and the end (width track, 0x00981cd0) of
                // the curve, exactly at the end once after the end moves.
                // Up, turned about the curve by -1..1 times the vertical
                // range and pushed out along that by 0..1 times the
                // horizontal range; or away from zSource.
                static const m2_particle::BezierSpline kNoSpline;
                const auto& spline = ei < gpu.particleSplines.size() ? gpu.particleSplines[ei] : kNoSpline;
                const float t0 = std::clamp(track(em.emissionAreaLength), 0.0f, 1.0f);
                if (em.emissionAreaWidth.hasData()) {
                    const float t1 = std::clamp(track(em.emissionAreaWidth), 0.0f, 1.0f);
                    if (std::fabs(t1 - inst.emitterSplineEnd[ei]) >= 2.3841858e-07f) {
                        inst.emitterSplineEnd[ei] = t1;
                        inst.emitterSplinePin[ei] = 1;
                    }
                }
                float t;
                if (inst.emitterSplinePin[ei]) {
                    t = inst.emitterSplineEnd[ei];
                    inst.emitterSplinePin[ei] = 0;
                } else {
                    t = t0 + dist01(particleRng_) * (inst.emitterSplineEnd[ei] - t0);
                }
                offset = spline.position(t);
                if (zSource != 0.0f) {
                    dir = m2_particle::zSourceDirection(offset, zSource);
                } else if (vRange != 0.0f) {
                    dir = m2_particle::splineDirection(spline.tangent(t),
                                                       distN(particleRng_) * vRange);
                    if (hRange != 0.0f) {
                        offset += dir * hRange * dist01(particleRng_);
                    }
                }
            } else {
                // Plane (0x009815c0): across a rectangle, a roll of -1..1
                // times half the length and half the width; a lean off the
                // emitter's up axis of -1..1 times the vertical range, turned
                // about it by -1..1 times the horizontal range; or away from
                // zSource.
                if (em.emitterType == 1) {
                    offset.x = distN(particleRng_) * track(em.emissionAreaLength) * 0.5f;
                    offset.y = distN(particleRng_) * track(em.emissionAreaWidth) * 0.5f;
                }
                if (zSource != 0.0f) {
                    dir = m2_particle::zSourceDirection(offset, zSource);
                } else {
                    const float pol = distN(particleRng_) * vRange;
                    const float az = distN(particleRng_) * hRange;
                    dir = glm::vec3(std::cos(az) * std::sin(pol),
                                    std::sin(az) * std::sin(pol),
                                    std::cos(pol));
                }
            }

            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            const glm::vec3 spawnLocal = em.position + offset;
            // Through the bone and the model, scale and all, as the client
            // multiplies it by the emitter's world matrix.
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);

            // Flag 0x10 keeps the particle in the emitter bone's space
            // (FUN_00832ea0 maps it to 0x200; FUN_00981950 then leaves the
            // position and velocity untransformed), drawn through the bone as
            // it is at that frame: the Eversong lamps' motes, on a bone that
            // turns once every 1.4 s, circle the flame with it. Kept in the
            // world, they stayed where they were born and only drifted.
            if (em.flags & 0x10) {
                p.modelSpace = true;
                p.position = spawnLocal;
                p.velocity = dir * speed;
            } else {
                p.position = glm::vec3(inst.modelMatrix * boneXform * glm::vec4(spawnLocal, 1.0f));
                p.velocity = rotMat * dir * speed;
            }

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

    // What each emitter hands its particles' step, sampled once per frame.
    constexpr size_t kMaxStackEmitters = 16;
    m2_particle::StepParams stepStack[kMaxStackEmitters];
    std::vector<m2_particle::StepParams> stepHeap;
    const size_t numEm = gpu.particleEmitters.size();
    m2_particle::StepParams* stepParams = stepStack;
    if (numEm > kMaxStackEmitters) {
        stepHeap.resize(numEm);
        stepParams = stepHeap.data();
    }
    for (size_t e = 0; e < numEm; ++e) {
        const auto& pem = gpu.particleEmitters[e];
        // The gravity track's value, zero included (0x00979bb0 applies +0xb4
        // as it is); drag and wind as the file gives them.
        stepParams[e].gravity = interpFloat(pem.gravity,
                                            inst.animTime, inst.globalSequenceTime,
                                            inst.currentSequenceIndex, gpu.globalSequenceDurations);
        stepParams[e].drag = pem.drag;
        stepParams[e].wind = pem.windVector;
        stepParams[e].windTime = pem.windTime;
    }
    static const m2_particle::StepParams kNoEmitter;

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        const float age = p.life;
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        const auto& sp = (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm)
                             ? stepParams[p.emitterIndex] : kNoEmitter;
        m2_particle::step(p.position, p.velocity, age, dt, sp);
        i++;
    }
}

void M2Renderer::prepareM2Particles() {
    particlesPrepared_ = true;
    particleRunCursor_ = 0;
    particleRuns_.clear();
    // This frame's buffer: the other may still be in use by the GPU.
    const uint32_t vbSlot = vkCtx_->getCurrentFrame() % kDynamicVBSlots;
    if (!particlePipeline_ || !m2ParticleVB_[vbSlot]) return;
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
    if (!m2ParticleVBMapped_[vbSlot]) return;
    float* const vbBase = static_cast<float*>(m2ParticleVBMapped_[vbSlot]);
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

    for (uint32_t instOrder = 0; instOrder < particleDrawOrder_.size(); ++instOrder) {
        const size_t idx = particleDrawOrder_[instOrder].second;
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
        // Size times the emitter's world scale where it is flagged 0x20.
        float cachedModelScale = 1.0f;

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

        // Each emitter is its own batch in the client, the model's last
        // emitter drawn first, and one flagged 0x2 sorts its particles back to
        // front (FUN_0097e580). Swap-and-pop removal leaves them interleaved,
        // so they are put in that order here. Last first is what the client
        // shows: the demon crystal's flame sheet (emitter 1, added) lights up
        // its dark smoke (emitter 2) rather than being buried under it.
        // Where a particle is in the world: a model-space one (flag 0x10)
        // through its emitter's bone as it is this frame.
        const auto particleWorld = [&inst, &gpu](const M2Particle& q) -> glm::vec3 {
            if (!q.modelSpace) return q.position;
            glm::mat4 world = inst.modelMatrix;
            if (q.emitterIndex >= 0 && q.emitterIndex < static_cast<int>(gpu.particleEmitters.size())) {
                const uint16_t bone = gpu.particleEmitters[q.emitterIndex].bone;
                if (bone < inst.boneMatrices.size()) world = world * inst.boneMatrices[bone];
            }
            return glm::vec3(world * glm::vec4(q.position, 1.0f));
        };
        particleSortScratch_.clear();
        for (uint32_t pi = 0; pi < inst.particles.size(); ++pi) {
            const auto& sp = inst.particles[pi];
            float key = 0.0f;
            if (sp.emitterIndex >= 0 && sp.emitterIndex < static_cast<int>(gpu.particleEmitters.size()) &&
                (gpu.particleEmitters[sp.emitterIndex].flags & 0x2)) {
                const glm::vec3 d = particleWorld(sp) - cachedCamPos_;
                key = -glm::dot(d, d);
            }
            particleSortScratch_.emplace_back(key, pi);
        }
        std::sort(particleSortScratch_.begin(), particleSortScratch_.end(),
                  [&inst](const auto& a, const auto& b) {
                      const int ea = inst.particles[a.second].emitterIndex;
                      const int eb = inst.particles[b.second].emitterIndex;
                      return ea != eb ? ea > eb : a.first < b.first;
                  });

        for (const auto& [sortKey, particleIdx] : particleSortScratch_) {
            const auto& p = inst.particles[particleIdx];
            const glm::vec3 pWorld = particleWorld(p);
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
                // Lit unless flagged 0x1, for blends 0-4 (FUN_0081fb10); fogged
                // by blend - black for the adds, the fog's colour for opaque,
                // key and alpha, white and grey for the modulates - unless
                // flagged 0x8 (FUN_00832ea0, table 0x00a45390).
                const uint8_t lit = (!(cachedEm->flags & 0x1) && cachedBlendType <= 4) ? 1 : 0;
                uint8_t fogMode = 0;
                if (cachedEm->flags & 0x8) fogMode = 2;
                else if (cachedBlendType <= 2) fogMode = 1;
                else if (cachedBlendType == 5) fogMode = 3;
                else if (cachedBlendType >= 6) fogMode = 4;
                // The client's M2 flag 0x20 is its size-by-scale bit (0x400
                // inside it): the length of the emitter's world matrix's
                // first row, the instance's scale among it (FUN_0097ac20).
                cachedModelScale = 1.0f;
                if (cachedEm->flags & 0x20) {
                    glm::mat4 world = inst.modelMatrix;
                    if (cachedEm->bone < inst.boneMatrices.size()) world = world * inst.boneMatrices[cachedEm->bone];
                    cachedModelScale = glm::length(glm::vec3(world[0]));
                }
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType),
                                     .tilesX = cachedTilesX, .tilesY = cachedTilesY,
                                     .lit = lit, .fogMode = fogMode};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                cachedGroup->lit = lit;
                cachedGroup->fogMode = fogMode;
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
            if (cachedGroup != runGroup || particleRuns_.empty() ||
                particleRuns_.back().instanceOrder != instOrder) {
                runGroup = cachedGroup;
                particleRuns_.push_back({.group = cachedGroup, .first = vbWritten, .count = 0,
                                         .instanceOrder = instOrder,
                                         .instanceIndex = static_cast<uint32_t>(idx),
                                         .distSq = particleDrawOrder_[instOrder].first});
            }
            highestParticleZ = std::max(highestParticleZ, pWorld.z);
            if (!emitterDiag.empty()) {
                auto& dg = emitterDiag[static_cast<size_t>(p.emitterIndex)];
                ++dg.n;
                dg.alpha += alpha;
                dg.size += scale * inst.scale;
                dg.minZ = std::min(dg.minZ, pWorld.z - inst.position.z);
                dg.maxZ = std::max(dg.maxZ, pWorld.z - inst.position.z);
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
                                " at=(", pWorld.x - inst.position.x, ",",
                                pWorld.y - inst.position.y, ",",
                                pWorld.z - inst.position.z, ")",
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
            vd[0] = pWorld.x;
            vd[1] = pWorld.y;
            vd[2] = pWorld.z;
            vd[3] = color.r;
            vd[4] = color.g;
            vd[5] = color.b;
            vd[6] = alpha;
            // The quad is a size either side of the centre, so this is half its
            // width: the scale, times the particle's own roll of the spread, and
            // the emitter's world scale where it is flagged 0x20.
            vd[7] = scale * p.sizeVary * cachedModelScale;
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
                                " flags=0x", std::hex, gpu.particleEmitters[ei].flags, std::dec);
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

    if (totalParticles == 0) {
        particleRuns_.clear();
        return;
    }

    // Far instances first, as the client's scene list draws them back to
    // front; each instance's runs keep their emitters' order. The buffer was
    // filled nearest first, so that it is the far ones a full buffer drops.
    std::stable_sort(particleRuns_.begin(), particleRuns_.end(),
                     [](const ParticleRun& a, const ParticleRun& b) {
                         return a.instanceOrder > b.instanceOrder;
                     });
}

bool M2Renderer::drawM2ParticleRuns(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                                    float behindDistSq, uint32_t ownerIndex) {
    if (particleRunCursor_ >= particleRuns_.size()) return false;
    // The runs farther than the doodad about to be drawn, and the doodad's
    // own: the client sorts emitters into the scene with every other blended
    // batch, so a crystal's smoke inside it is drawn before the eyes on its
    // face rather than over them.
    const auto wanted = [&](const ParticleRun& run) {
        return run.distSq > behindDistSq || run.instanceIndex == ownerIndex;
    };
    if (!wanted(particleRuns_[particleRunCursor_])) return false;

    const uint32_t vbSlot = vkCtx_->getCurrentFrame() % kDynamicVBSlots;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_[vbSlot], &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (; particleRunCursor_ < particleRuns_.size(); ++particleRunCursor_) {
        auto& run = particleRuns_[particleRunCursor_];
        if (!wanted(run)) break;
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

        // Push constants: tileCount, alphaKey, lit, fogMode
        struct { float tileX, tileY; int alphaKey; int lit; int fogMode; } pc = {
            .tileX = static_cast<float>(group.tilesX), .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0,
            .lit = group.lit, .fogMode = group.fogMode
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // The vertices are already in the buffer, at this run's own offset.
        // Both used to be wrong: every group copied to offset zero and drew
        // from vertex zero, so with more than one group up they all drew
        // whatever had been copied last.
        // A quad of four vertices for each particle of the run.
        vkCmdDraw(cmd, 4, run.count, 0, run.first);
    }
    return true;
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    // What the doodads' blended pass left: every run when it drew none of
    // them, or the ones nearer than its last doodad.
    if (!particlesPrepared_) prepareM2Particles();
    drawM2ParticleRuns(cmd, perFrameSet, -1.0f, UINT32_MAX);
    particlesPrepared_ = false;
}

} // namespace rendering
} // namespace wowee

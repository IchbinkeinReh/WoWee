#include <optional>
#include <cstring>
#include <limits>
#include "rendering/shadow_params.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_blend_mode.hpp"
#include "core/thread_pool.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_view_distance.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include "core/frame_profiler.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <future>
#include <thread>

#include <set>
#include <utility>

namespace wowee {
namespace rendering {

/// Starts a new instance's animation and gives it bones to draw with now.
///
/// Both spawn paths need this: the one that takes a position and the one that
/// takes a whole matrix, and each had its own copy.
///
/// The bone seed is what keeps a new instance from being invisible for a
/// frame. Bones are computed in update(), so an instance spawned mid-frame has
/// none until the next one; copying them from a sibling of the same model
/// draws it immediately. A seed entry pointing at an instance that has since
/// gone is dropped rather than followed.

namespace {

// The 2x3 a UV needs from it: rows of the linear part, and the translation.
void setUvTransform(glm::vec4& linear, glm::vec2& offset, const glm::mat4& m) {
    linear = glm::vec4(m[0][0], m[1][0], m[0][1], m[1][1]);
    offset = glm::vec2(m[3][0], m[3][1]);
}

// A batch's texture transform for one of its stages, or null: the client
// takes stage i's from the transform combos at the batch's index plus i, and
// an index past the model's transforms is the identity (0x0081f450).
const pipeline::M2TextureTransform* batchStageTransform(const M2ModelGPU& model,
                                                        const M2ModelGPU::BatchGPU& batch,
                                                        uint32_t stage) {
    if (batch.textureAnimIndex == 0xFFFF || !model.hasTextureAnimation) return nullptr;
    if (stage > 0 && ((batch.combinerModes >> 8) & 3) <= stage) return nullptr;
    const uint32_t lookupIdx = static_cast<uint32_t>(batch.textureAnimIndex) + stage;
    if (lookupIdx >= model.textureTransformLookup.size()) return nullptr;
    const uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
    if (transformIdx >= model.textureTransforms.size()) return nullptr;
    return &model.textureTransforms[transformIdx];
}

// Push constants carry per-batch data only; per-instance data is in the
// instance SSBO.
struct M2PushConstants {
    int32_t texCoordSet;        // each stage's coordinate source (m2PackCoordSources)
    int32_t isFoliage;          // -1 = sky, 0 = everything else
    int32_t instanceDataOffset; // Base index into instance SSBO for this draw group
    int32_t clutterList;        // chunked ground cover: first list entry; -1 otherwise
    float clutterDistSq;        // its view distance squared
    float centerX, centerY, centerZ;  // the model's box centre, in its own space
    // A chunk draw's texture matrices (both stages), the batch's for every tuft.
    glm::vec4 uvLinear;
    glm::vec4 uvLinear2;
    glm::vec4 uvOffsets;  // xy stage 0, zw stage 1
};
static_assert(sizeof(M2PushConstants) == 80, "matches m2.vert's push block");

// Whether a texture track gives one value whatever the sequence and time:
// every key of every timeline the same, and no timeline without keys beside
// ones with (that one would read the default).
bool vec3TrackConstant(const pipeline::M2AnimationTrack& track) {
    bool any = false, anyEmpty = false;
    glm::vec3 value(0.0f);
    for (size_t i = 0; i < track.sequences.size(); ++i) {
        const auto& keys = track.sequences[i];
        const size_t n = std::min(keys.timestamps.size(), keys.vec3Values.size());
        if (n == 0) { anyEmpty = true; continue; }
        for (size_t k = 0; k < n; ++k) {
            if (!any) { value = keys.vec3Values[k]; any = true; }
            else if (keys.vec3Values[k] != value) return false;
        }
    }
    return !(any && anyEmpty);
}
bool quatTrackConstant(const pipeline::M2AnimationTrack& track) {
    bool any = false, anyEmpty = false;
    pipeline::PackedQuat value;
    for (size_t i = 0; i < track.sequences.size(); ++i) {
        const auto& keys = track.sequences[i];
        const size_t n = std::min(keys.timestamps.size(), keys.quatValues.size());
        if (n == 0) { anyEmpty = true; continue; }
        for (size_t k = 0; k < n; ++k) {
            if (!any) { value = keys.quatValues[k]; any = true; }
            else if (std::memcmp(&keys.quatValues[k], &value, sizeof(value)) != 0) return false;
        }
    }
    return !(any && anyEmpty);
}
bool textureTransformConstant(const pipeline::M2TextureTransform& tt) {
    return vec3TrackConstant(tt.translation) && quatTrackConstant(tt.rotation) && vec3TrackConstant(tt.scale);
}

// Fill the model half of the push constants.
//
// There is no sway any more: the client moves a doodad only through its
// own bones and texture tracks, so a tree, a banner or a field of clutter
// moves exactly as its artist keyed it and the shader adds nothing.
/// The sequence a finished one-shot goes on to: the one it was told to
/// hand over to (M2Instance::afterOneShotIndex), else the idle one.
int afterOneShot(M2Instance& instance, const M2ModelGPU& model) {
    const int then = instance.afterOneShotIndex;
    instance.afterOneShotIndex = -1;
    if (then >= 0 && then < static_cast<int>(model.sequences.size())) return then;
    return instance.idleSequenceIndex;
}

void fillModelPush(M2PushConstants& pc, bool sky) {
    pc.isFoliage = sky ? -1 : 0;
    pc.clutterList = -1;
    pc.clutterDistSq = 0.0f;
    pc.centerX = pc.centerY = pc.centerZ = 0.0f;
    pc.uvLinear = pc.uvLinear2 = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    pc.uvOffsets = glm::vec4(0.0f);
}

} // namespace

void M2Renderer::seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                                       M2Instance& instance) {
        if (!model.sequences.empty()) {
            instance.currentSequenceIndex = 0;
            instance.idleSequenceIndex = 0;
            instance.animDuration = static_cast<float>(model.sequences[0].duration);
            instance.animTime = static_cast<float>(randRange(std::max(1u, model.sequences[0].duration)));
        }

    auto seedIt = boneSeedInstanceByModel_.find(modelId);
    if (seedIt != boneSeedInstanceByModel_.end()) {
        auto idxIt = instanceIndexById.find(seedIt->second);
        if (idxIt != instanceIndexById.end() && idxIt->second < instances.size()) {
            const auto& existing = instances[idxIt->second];
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
                instance.bonesDirty[0] = instance.bonesDirty[1] = true;
            } else {
                boneSeedInstanceByModel_.erase(seedIt);  // stale entry
            }
        } else {
            boneSeedInstanceByModel_.erase(seedIt);  // that instance is gone
        }
    }

    // No sibling to copy from, so pay for the bones now.
    if (instance.boneMatrices.empty()) {
        computeBoneMatrices(model, instance, &cachedCamPos_);
    }
    if (!instance.boneMatrices.empty()) {
        boneSeedInstanceByModel_.emplace(modelId, instance.id);
    }
}

// A model the bone loop does not step still plays its first sequence: its
// texture scroll and its emitters are keyed to it, and update() wraps the
// clock at this length. Left at zero, the length was unknown, the clock was
// wrapped at a fixed 3.3 s instead, and a waterfall whose scroll lasts 2.6 s
// stood still for the difference every lap.
void M2Renderer::seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance) {
    if (model.sequences.empty() || model.sequences[0].duration == 0) return;
    instance.currentSequenceIndex = 0;
    instance.idleSequenceIndex = 0;
    instance.animDuration = static_cast<float>(model.sequences[0].duration);
    instance.animTime = static_cast<float>(randRange(model.sequences[0].duration));
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale,
                                     bool allowPositionDedup) {
    // Reject NaN inputs at the boundary - std::round of NaN is implementation-
    // defined and a NaN instance position propagates into the GPU model matrix,
    // either tripping Vulkan validation or rendering at the world origin.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(rotation.x) ||
        !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
        !std::isfinite(scale) || scale <= 0.0f) {
        return 0;
    }
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdlRef = modelIt->second;
    modelUnusedSince_.erase(modelId);


    // Deduplicate: skip if same model already at nearly the same position.
    // Uses hash map for O(1) lookup instead of O(N) scan.
    // Spell effects are exempt - transient visuals must always create fresh instances.
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    if (mdlRef.isGroundDetail) {
        instance.position.z -= computeGroundDetailDownOffset(mdlRef, scale);
    }
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    glm::vec3 localMin, localMax;
    getCollisionQueryBounds(mdlRef, localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);

    // Cache model flags on instance to avoid per-frame hash lookups
    instance.cachedHasAnimation = mdlRef.hasAnimation;
    instance.cachedDisableAnimation = mdlRef.disableAnimation;
    // Ribbons are updated in the same pass, so a model with ribbons alone
    // has to be on the list too or its trails never move.
    instance.cachedHasParticleEmitters = !mdlRef.particleEmitters.empty() ||
                                         !mdlRef.ribbonEmitters.empty();
    instance.cachedBoundRadius = mdlRef.boundRadius;
    instance.cachedIsGroundDetail = mdlRef.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdlRef.isInvisibleTrap;
    instance.cachedIsLightBeam = mdlRef.isLightBeam;
    instance.cachedIsTransportDoodad = mdlRef.isTransportDoodad;
    instance.cachedIsValid = mdlRef.isValid();
    instance.cachedModel = &mdlRef;
    instance.recomputeCachedCullFactors();

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation) {
        seedInstanceAnimation(mdlRef, modelId, instance);
    } else {
        seedUnanimatedSequence(mdlRef, instance);
    }

    // Register in dedup map before pushing (uses original position, not ground-adjusted)
    // Spell effects are exempt from dedup tracking (transient, overlapping allowed).
    if (allowPositionDedup && !mdlRef.isGroundDetail && !mdlRef.isSpellEffect) {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    // One entry on the end of the shadow cull copy, rather than all of it
    // rebuilt at the next shadow pass: a city creates and removes spell
    // effects and particle twins every frame, and each rebuilt 120 000.
    if (!shadowCullDirty_ && shadowCull_.size() == idx) {
        shadowCull_.emplace_back();
        refreshShadowCullEntry(idx);
    } else {
        shadowCullDirty_ = true;
    }
    // Track special instances for fast-path iteration
    if (!mdlRef.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdlRef.hasAnimation && !mdlRef.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdlRef.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);
    // Placed in the traced scene by the next sync; listed if it lights.
    markRtUnsettled(instances[idx]);
    noteRtLit(instances[idx], mdlRef);

    return instance.id;
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                const glm::vec3& position) {
    // Reject NaN inputs at the boundary. position feeds the dedup hash
    // (std::round of NaN is implementation-defined); the matrix goes
    // straight to the GPU UBO and would crash validation.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return 0;
    }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(modelMatrix[c][r])) return 0;
    if (models.find(modelId) == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    modelUnusedSince_.erase(modelId);

    // Deduplicate: O(1) hash lookup
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;  // Used for frustum culling
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    glm::vec3 localMin, localMax;
    getCollisionQueryBounds(models[modelId], localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);
    // Cache model flags on instance to avoid per-frame hash lookups
    const auto& mdl2 = models[modelId];
    instance.cachedHasAnimation = mdl2.hasAnimation;
    instance.cachedDisableAnimation = mdl2.disableAnimation;
    instance.cachedHasParticleEmitters = !mdl2.particleEmitters.empty() ||
                                         !mdl2.ribbonEmitters.empty();
    instance.cachedBoundRadius = mdl2.boundRadius;
    instance.cachedIsGroundDetail = mdl2.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdl2.isInvisibleTrap;
    instance.cachedIsLightBeam = mdl2.isLightBeam;
    instance.cachedIsTransportDoodad = mdl2.isTransportDoodad;
    instance.cachedIsValid = mdl2.isValid();
    instance.cachedModel = &mdl2;
    instance.recomputeCachedCullFactors();

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        seedInstanceAnimation(mdl2, modelId, instance);
    } else {
        // A model with no skeleton can still have particle emitters, and their
        // rate and lifespan tracks are sampled at animTime. Starting every
        // instance at zero puts a courtyard of identical torches in lockstep,
        // so the phase is spread.
        //
        // createInstance above now does the same.
        instance.animTime = randFloat(0.0f, 10000.0f);
        seedUnanimatedSequence(mdl2, instance);
    }

    // Register in dedup map
    {
        DedupKey dk{.modelId = modelId,
                    .qx = static_cast<int32_t>(std::round(position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    // WOWEE_M2_CENSUS: what the model is drawn at, beside what it was
    // authored at. The load-time census gives the authored height; a game
    // object's scale comes from the server and is only known here.
    censusInstance(instance);
    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    // One entry on the end of the shadow cull copy, rather than all of it
    // rebuilt at the next shadow pass: a city creates and removes spell
    // effects and particle twins every frame, and each rebuilt 120 000.
    if (!shadowCullDirty_ && shadowCull_.size() == idx) {
        shadowCull_.emplace_back();
        refreshShadowCullEntry(idx);
    } else {
        shadowCullDirty_ = true;
    }
    if (!mdl2.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdl2.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    insertBounds(spatialGrid, instance.worldBoundsMin, instance.worldBoundsMax, instance.id);
    // Placed in the traced scene by the next sync; listed if it lights.
    markRtUnsettled(instances[idx]);
    noteRtLit(instances[idx], mdl2);

    return instance.id;
}

// WOWEE_SKY_M2_MAX_BATCH=<n> draws only the sky model's first n layers.
//
// Thirty-four of them, and every measurement says the set drawn is the same
// every frame - so if the flicker is one layer's doing, bisecting the count
// finds it in about five runs. That has worked three times on this fault where
// reading the code has worked none.
static bool skyBatchAllowed(bool skyMode, std::size_t index) {
    if (!skyMode) return true;
    static const int maxBatch = [] {
        const char* set = std::getenv("WOWEE_SKY_M2_MAX_BATCH");
        return set ? std::atoi(set) : -1;
    }();
    return maxBatch < 0 || static_cast<int>(index) < maxBatch;
}

float M2Renderer::instanceMaxDistSq(const M2Instance& inst) const {
    // Ground cover on a chunk is held back by its chunk's view depth alone
    // (0x007d3fe0, TerrainManager::updateDetailDoodads) and faded by its own
    // (0x007b15d0); the client tests no doodad of it by distance.
    if (inst.detailDoodad) {
        const float d = cappedViewDistance();
        return d * d;
    }
    if (inst.noDistanceCull && !inst.isGameObject) {
        // Flag 0x800: only the chunk walk holds it back (0x00799980), by the
        // chunks it is listed in.
        const glm::vec3 half = inst.cachedVisualExtent * 0.5f;
        glm::vec3 boxMin, boxMax;
        rendering::m2ChunkBoxAround(inst.cachedCullCenter - half, inst.cachedCullCenter + half, boxMin, boxMax);
        const float depth = rendering::m2ChunkViewDepth(boxMin, boxMax, cullViewPos_, cullViewDir_);
        if (inst.cachedSizeClass < rendering::m2ChunkMinSizeClass(depth, environmentDetail_)) return -1.0f;
    }
    return rendering::m2InstanceMaxDistSq(inst.cachedSizeClass, environmentDetail_,
                                          inst.isGameObject, cappedViewDistance(),
                                          inst.cachedIsGroundDetail, groundDetailMaxDistance_,
                                          inst.noDistanceCull);
}

float M2Renderer::instanceDistanceFade(const M2Instance& inst, float distSq) const {
    if (inst.isGameObject || inst.noDistanceCull) return 1.0f;
    const float dist = std::sqrt(distSq);
    // Ground cover placed on a chunk fades in its shader, by view depth.
    if (inst.detailDoodad) return 1.0f;
    if (inst.cachedIsGroundDetail && groundDetailMaxDistance_ > 0.0f) {
        // From 85% of groundEffectDist to all of it (0x007b15d0).
        const float d = groundDetailMaxDistance_;
        return std::clamp((d - dist) / (d * 0.15f), 0.0f, 1.0f);
    }
    return rendering::m2DoodadFade(dist, inst.cachedSizeClass, environmentDetail_);
}

void M2Renderer::update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection) {
    ZoneScopedN("M2Renderer::update");
    // On a frame worker, alongside the characters' animation on the main
    // thread; the main thread waits for it before the footprints.
    WOWEE_PROFILE_SCOPE("animation: m2 doodads", Worker);
    if (spatialIndexDirty_) {
        rebuildSpatialIndex();
    }

    float dtMs = deltaTime * 1000.0f;

    // Cache camera state for frustum-culling bone computation
    cachedCamPos_ = cameraPos;

    // Build frustum for culling bones
    Frustum updateFrustum;
    updateFrustum.extractFromMatrix(viewProjection);

    // --- Normal M2 animation update ---
    // Advance animTime for ALL instances (needed for texture UV animation on static doodads).
    // This is a tight loop touching only one float per instance - no hash lookups.
    //
    // An instance the bone loop below does not step is wrapped here, at the
    // length of its own sequence. A texture track is sampled against this
    // time and holds its last key once the time runs past the end, so a
    // waterfall with no bones of its own scrolled once after it loaded and
    // then stood still, and one with emitters - wrapped at a fixed 3.3 s
    // below, whatever its sequence - stopped for the rest of each lap.
    for (auto& instance : instances) {
        instance.animTime += dtMs;
        instance.globalSequenceTime += dtMs;
        const bool steppedBelow = instance.cachedHasAnimation && !instance.cachedDisableAnimation;
        if (!steppedBelow && instance.animDuration > 0.0f &&
            instance.animTime >= instance.animDuration) {
            instance.animTime = std::fmod(instance.animTime, instance.animDuration);
        }
    }

    // The sky model's clock, when this is the renderer that draws one.
    //
    // A report of the sky "playing an animation that brightens and dims, faster
    // when walking" survived three fixes to what chooses the sky, and the
    // lighting diagnostic then showed every input to it holding still while it
    // happened - the zone, the volumes, the model and the target colour. So
    // what is moving is this, and the two things worth telling apart are
    // whether the instance is being rebuilt (animTime back to zero) and
    // whether the clock runs at wall speed (animTime should advance by dtMs and
    // by nothing else).
    //
    // Only this renderer has skyMode_, and only on a change worth seeing, so it
    // is quiet unless asked for with WOWEE_LOG_LEVEL=info.
    if (skyMode_ && !instances.empty()) {
        const auto& sky = instances.front();
        const bool restarted = sky.animTime < skyDiagAnimTime_;
        if (restarted || sky.id != skyDiagInstanceId_ ||
            sky.animTime - skyDiagAnimTime_ > 1000.0f) {
            LOG_INFO("skyM2: instance=", sky.id, " instances=", instances.size(),
                     " animTime=", sky.animTime, " duration=", sky.animDuration,
                     " gsTime=", sky.globalSequenceTime, " dtMs=", dtMs,
                     restarted ? " RESTARTED" : "");
            skyDiagInstanceId_ = sky.id;
            skyDiagAnimTime_ = sky.animTime;
        }
    }
    // Wrap animTime for particle-only instances so emission rate tracks keep looping.
    // 3333ms chosen as a safe wrap period: long enough to cover the longest known M2
    // particle emission cycle (~3s for torch/campfire effects) while preventing float
    // precision loss that accumulates over hours of runtime.
    static constexpr float kParticleWrapMs = 3333.0f;
    // Only for those with no sequence length to wrap at; the rest were
    // wrapped at their own above.
    for (size_t idx : particleOnlyInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        if (instance.animDuration > 0.0f) continue;
        // Use iterative subtraction instead of fmod() to preserve precision
        while (instance.animTime > kParticleWrapMs) {
            instance.animTime -= kParticleWrapMs;
        }
    }

    boneWorkIndices_.clear();
    boneWorkIndices_.reserve(animatedInstanceIndices_.size());

    // Update animated instances (full animation state + bone computation culling)
    // Note: animTime was already advanced by dtMs in the global loop above.
    // Here we apply the speed factor: subtract the base dtMs and add dtMs*speed.
    // Ground clutter stops being stepped once it is past the distance it draws
    // at. There are hundreds of tufts to a tile and each one plays a sequence of
    // its own, so this list is mostly grass that nothing can see; the sequences
    // loop, so one that resumes from a stale time is indistinguishable from one
    // that never stopped.
    const float clutterAnimCutoffSq = (groundDetailMaxDistance_ > 0.0f)
        ? (groundDetailMaxDistance_ * groundDetailMaxDistance_) : 0.0f;

    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (clutterAnimCutoffSq > 0.0f && instance.cachedIsGroundDetail) {
            const glm::vec3 toCam = instance.position - cachedCamPos_;
            if (glm::dot(toCam, toCam) > clutterAnimCutoffSq) continue;
        }

        // Where the events were looked at last: before this step's time, or
        // below zero for a sequence just started (M2Instance::eventsFromStart).
        const float eventsFrom = instance.eventsFromStart ? -1.0f : instance.animTime - dtMs;
        instance.eventsFromStart = false;
        instance.animTime += dtMs * (instance.animSpeed - 1.0f);

        // For animation looping/variation, we need the actual model data.
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // The events of a sequence the animation passed from `from` to `to`,
        // once each and every lap where it loops (0x00832260, 0x00830fb0).
        // An event on a global sequence reads the first sequence's times.
        const bool steppedEvents = instance.eventOwner != m2_event::Owner::None && !model.events.empty();
        auto stepEvents = [&](int seq, float from, float to) {
            if (!steppedEvents || seq < 0 || seq >= static_cast<int>(model.sequences.size())) return;
            // Every lap of the sequence as it loops here (0x00830fb0 repeats
            // by its length; one with flag 0x1 the client holds at its end).
            const float lap = static_cast<float>(model.sequences[seq].duration);
            for (uint32_t e = 0; e < model.events.size(); ++e) {
                const auto& event = model.events[e];
                const size_t timeline = event.globalSequence >= 0 ? 0u : static_cast<size_t>(seq);
                if (timeline >= event.times.size()) continue;
                m2_event::forEachCrossed(event.times[timeline], from, to, lap, [&](uint32_t) {
                    steppedEvents_.push_back({idx, e});
                });
            }
        };
        const int eventsSequence = instance.currentSequenceIndex;

        // Validate sequence index
        if (instance.currentSequenceIndex < 0 ||
            instance.currentSequenceIndex >= static_cast<int>(model.sequences.size())) {
            instance.currentSequenceIndex = 0;
            if (!model.sequences.empty()) {
                instance.animDuration = static_cast<float>(model.sequences[0].duration);
            }
        }

        // Handle animation looping / variation transitions
        if (instance.animDuration <= 0.0f && instance.cachedHasParticleEmitters) {
            instance.animDuration = rendering::M2_DEFAULT_PARTICLE_ANIM_MS;
        }
        if (instance.animSpeed < 0.0f) {
            // Backwards, standing in for an animation the model lacks
            // (0x00826350): its events are passed from its end (0x00830fb0),
            // and at its start it ends as it would at its end.
            if (steppedEvents && eventsSequence >= 0 && eventsSequence < static_cast<int>(model.sequences.size())) {
                const uint32_t length = model.sequences[eventsSequence].duration;
                const float ran = static_cast<float>(length) - instance.animTime;
                const float ranBefore = eventsFrom < 0.0f ? -1.0f : static_cast<float>(length) - eventsFrom;
                const float lap = instance.holdAtEnd || instance.playingVariation ? 0.0f : static_cast<float>(length);
                for (uint32_t e = 0; e < model.events.size(); ++e) {
                    const auto& event = model.events[e];
                    const size_t timeline = event.globalSequence >= 0 ? 0u : static_cast<size_t>(eventsSequence);
                    if (timeline >= event.times.size()) continue;
                    m2_event::forEachCrossedReversed(event.times[timeline], length, ranBefore,
                                                     std::min(ran, lap > 0.0f ? ran : static_cast<float>(length)),
                                                     lap, [&](uint32_t) { steppedEvents_.push_back({idx, e}); });
                }
            }
            if (instance.animTime <= 0.0f) {
                if (instance.holdAtEnd) {
                    instance.animTime = 0.0f;
                    instance.animSpeed = 0.0f;
                } else if (instance.playingVariation) {
                    instance.eventsFromStart = true;
                    const int next = afterOneShot(instance, model);
                    blendIntoSequence(model, instance, next);
                    instance.playingVariation = false;
                    instance.currentSequenceIndex = next;
                    if (next < static_cast<int>(model.sequences.size()))
                        instance.animDuration =
                            static_cast<float>(model.sequences[next].duration);
                    instance.animTime = 0.0f;
                    instance.animSpeed = 1.0f;
                } else {
                    const float duration = std::max(1.0f, instance.animDuration);
                    while (instance.animTime <= 0.0f) instance.animTime += duration;
                }
            }
        } else if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration) {
            if (instance.holdAtEnd) {
                // Stay on the last frame. A door's open sequence ends with the
                // door open, and that is the pose the server is describing.
                stepEvents(eventsSequence, eventsFrom, instance.animDuration);
                instance.animTime = instance.animDuration;
                instance.animSpeed = 0.0f;
            } else if (instance.playingVariation) {
                stepEvents(eventsSequence, eventsFrom, instance.animDuration);
                instance.eventsFromStart = true;
                const int next = afterOneShot(instance, model);
                blendIntoSequence(model, instance, next);
                instance.playingVariation = false;
                instance.currentSequenceIndex = next;
                if (next < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[next].duration);
                }
                instance.animTime = 0.0f;
            } else {
                // Use iterative subtraction instead of fmod() to preserve precision
                float duration = std::max(1.0f, instance.animDuration);
                const float lapEnd = instance.animTime;
                while (instance.animTime >= duration) {
                    instance.animTime -= duration;
                }
                // At the end of each pass the client rolls the variation chain
                // of the animation again (FUN_00831fc0 -> FUN_00826e60), which
                // is how Stand alternates with its fidgets; it never cuts a
                // pass short on a timer.
                const int current = instance.currentSequenceIndex;
                if (current >= 0 && current < static_cast<int>(model.sequences.size())) {
                    int primary = current;
                    for (int i = 0; i < static_cast<int>(model.sequences.size()); ++i) {
                        if (model.sequences[i].id == model.sequences[current].id &&
                            model.sequences[i].variationIndex == 0) { primary = i; break; }
                    }
                    const int next = m2_track::pickSequenceVariation(
                        model.sequences, primary, static_cast<uint32_t>(std::rand()) & 0x7FFFu);
                    if (next != current && next >= 0 &&
                        next < static_cast<int>(model.sequences.size()) &&
                        model.sequences[next].duration > 0) {
                        // The pass ends, and the next sequence starts.
                        stepEvents(eventsSequence, eventsFrom, duration);
                        blendIntoSequence(model, instance, next);
                        instance.currentSequenceIndex = next;
                        instance.animDuration = static_cast<float>(model.sequences[next].duration);
                        instance.animTime = std::min(instance.animTime, instance.animDuration - 1.0f);
                        stepEvents(next, -1.0f, instance.animTime);
                    } else {
                        stepEvents(eventsSequence, eventsFrom, lapEnd);
                    }
                } else {
                    stepEvents(eventsSequence, eventsFrom, lapEnd);
                }
            }
        } else {
            stepEvents(eventsSequence, eventsFrom, instance.animTime);
        }

        // Frustum + distance cull: skip expensive bone computation for off-screen instances.
        // Both effectiveMaxDistSq and paddedRadius are precomputed per instance in
        // recomputeCachedCullFactors(); we only need the per-frame distance and frustum test.
        glm::vec3 toCam = instance.cachedCullCenter - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        if (distSq > instanceMaxDistSq(instance)) continue;
        float paddedRadius = instance.cachedPaddedRadius;
        if (paddedRadius > 0.0f && !updateFrustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;

        // Every frame, at every distance it is drawn: the client steps a
        // visible model's bones each frame, and updating every second or
        // fourth frame - or freezing past 150 yards - is what made distant
        // doodads move in visible steps.
        boneWorkIndices_.push_back(idx);
    }

    // Compute bone matrices (expensive, parallel if enough work)
    const size_t animCount = boneWorkIndices_.size();
    if (animCount > 0) {
        static const size_t minParallelAnimInstances = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_M2_ANIM_MT_MIN", 96));
        if (animCount < minParallelAnimInstances || numAnimThreads_ <= 1) {
            // Sequential - not enough work to justify thread overhead
            for (size_t i : boneWorkIndices_) {
                if (i >= instances.size()) continue;
                auto& inst = instances[i];
                if (!inst.cachedModel) continue;
                computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
            }
        } else {
            // Parallel - dispatch across worker threads
            static const size_t minAnimWorkPerThread = std::max<size_t>(
                16, envSizeOrDefault("WOWEE_M2_ANIM_WORK_PER_THREAD", 64));
            const size_t maxUsefulThreads = std::max<size_t>(
                1, (animCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
            const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);
            if (numThreads <= 1) {
                for (size_t i : boneWorkIndices_) {
                    if (i >= instances.size()) continue;
                    auto& inst = instances[i];
                    if (!inst.cachedModel) continue;
                    computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                }
            } else {
                const size_t chunkSize = animCount / numThreads;
                const size_t remainder = animCount % numThreads;

                auto processRange = [this](size_t begin, size_t end) {
                    for (size_t j = begin; j < end; ++j) {
                        size_t idx = boneWorkIndices_[j];
                        if (idx >= instances.size()) continue;
                        auto& inst = instances[idx];
                        if (!inst.cachedModel) continue;
                        computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
                    }
                };

                // Reuse persistent futures vector to avoid allocation
                animFutures_.clear();
                if (animFutures_.capacity() < numThreads) {
                    animFutures_.reserve(numThreads);
                }

                // Dispatch all but the last chunk to the shared pool; process the
                // last chunk on this thread so this call always makes progress even
                // when it itself runs on a pool worker (see ThreadPool docs).
                size_t start = 0;
                for (size_t t = 0; t + 1 < numThreads; ++t) {
                    size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                    animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                        [processRange, start, end]() { processRange(start, end); }));
                    start = end;
                }
                processRange(start, animCount);

                for (auto& f : animFutures_) {
                    f.get();
                }
            }
        }
    }

    // This step's events, where they are now the bones are posed: on the
    // event's bone, then in the world (0x004c21b0 with the bone's matrix and
    // the model's). Held for whoever plays them (M2EventSounds), and dropped
    // if nobody does.
    if (firedEvents_.size() > 4096) firedEvents_.clear();
    for (const SteppedEvent& stepped : steppedEvents_) {
        const M2Instance& inst = instances[stepped.instanceIndex];
        const auto& event = inst.cachedModel->events[stepped.eventIndex];
        glm::mat4 bone(1.0f);
        if (event.bone < inst.boneMatrices.size()) bone = inst.boneMatrices[event.bone];
        firedEvents_.push_back({.instanceId = inst.id, .owner = inst.eventOwner, .ownerArg = inst.eventOwnerArg,
                                .id = event.id, .data = event.data,
                                .position = glm::vec3(inst.modelMatrix * bone * glm::vec4(event.position, 1.0f))});
    }
    steppedEvents_.clear();

    // Particle update (sequential - uses RNG, not thread-safe)
    // Only iterate instances that have particle emitters (pre-built list).
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        // Distance cull: only update particles within visible range
        glm::vec3 toCam = instance.cachedCullCenter - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        if (distSq > instanceMaxDistSq(instance)) continue;
        if (!instance.cachedModel) continue;
        // A unit's twin steps once the unit is posed (stepPosedParticles).
        if (instance.externallyPosed) continue;
        emitParticles(instance, *instance.cachedModel, deltaTime);
        updateParticles(instance, deltaTime);
        if (!instance.cachedModel->ribbonEmitters.empty()) {
            updateClientRibbons(instance, *instance.cachedModel, deltaTime);
        }
    }

}

void M2Renderer::stepPosedParticles(float deltaTime) {
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        if (!instance.externallyPosed || !instance.cachedModel) continue;
        const glm::vec3 toCam = instance.cachedCullCenter - cachedCamPos_;
        if (glm::dot(toCam, toCam) > instanceMaxDistSq(instance)) continue;
        emitParticles(instance, *instance.cachedModel, deltaTime);
        updateParticles(instance, deltaTime);
        if (!instance.cachedModel->ribbonEmitters.empty()) {
            updateClientRibbons(instance, *instance.cachedModel, deltaTime);
        }
    }
}

// Diagnostic: WOWEE_M2_NO_SKINNING=1 renders every M2 in its bind pose by
// telling the shader to ignore bones, separating a skinning artifact from one
// drawn by the particle or ribbon systems.
static const bool kM2NoSkinning = envFlagEnabled("WOWEE_M2_NO_SKINNING");

void M2Renderer::prepareRender(uint32_t frameIndex) {
    if (!initialized_) return;
    // The chunked ground cover's store and this frame's list (main thread,
    // with this slot's last frame fenced): ranges freed long enough ago go
    // back to the store, a changed clutter distance into every entry, and
    // the list has room for every entry there is.
    reclaimClutterRanges();
    if (groundDetailMaxDistance_ != clutterWrittenDistance_) {
        clutterWrittenDistance_ = groundDetailMaxDistance_;
        for (const auto& [handle, chunk] : clutterChunks_) {
            for (uint32_t s = chunk.first; s < chunk.first + chunk.count; ++s) {
                clutterStaticCpu_[s].interiorDirect.w = groundDetailMaxDistance_;
                if (clutterStaticMapped_) {
                    std::memcpy(static_cast<M2InstanceGPU*>(clutterStaticMapped_) + s, &clutterStaticCpu_[s],
                                sizeof(M2InstanceGPU));
                }
            }
        }
    }
    ensureClutterList(frameIndex, clutterLive_);
    bindClutterBuffers(frameIndex);
    if (instances.empty()) return;
    WOWEE_PROFILE_SCOPE("prepare: m2 bones", Cpu);

    // --- Mega bone SSBO: assign ranges and upload all animated instance bones ---
    // Offset 0 is reserved as the identity/no-bones sentinel; animated instances
    // are packed after it at their own bone count, so a 300-bone creature gets
    // all 300 matrices instead of being truncated into a fixed stride and
    // reading into its neighbour's range.
    uint32_t nextOffset = 1;
    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (instance.boneMatrices.empty()) {
            instance.megaBoneOffset = 0;  // Use identity slot
            continue;
        }

        // A bone that has left the model behind.
        //
        // The vertex shader skins by these matrices, so one that translates far
        // outside the model's own bounds drags every vertex weighted to it out
        // with it. What that looks like on screen is not a bone problem: it is
        // a long flat triangle with the skin smeared across it, which reads as
        // a missing texture or a stray card. The Elemental Slave's white sheets
        // were reported three times as missing textures, and the texture paths,
        // the display skins, the particle emitters and the ribbons were all
        // measured and found correct before the geometry was suspected.
        //
        // Judged against the other bones, not against the model's origin, and
        // against the model's own bounding radius so a large creature is not
        // accused for being large. Measuring from the origin flags every
        // flying critter: a bird's flight path is authored into its root bone,
        // so the whole skeleton is sixteen yards out and nothing is stretched.
        // What a stretch looks like is one bone leaving the set behind.
        if (instance.cachedModel != nullptr) {
            const M2ModelGPU& m = *instance.cachedModel;
            const glm::vec3 extent = m.boundMax - m.boundMin;
            const float radius = 0.5f * glm::length(extent);
            // Examined once per model, not once per frame: this walks every
            // bone, and there are models with three hundred of them drawn
            // dozens at a time.
            static std::unordered_set<uint32_t> boneRangeChecked;
            if (radius > 0.01f && boneRangeChecked.size() < 64 &&
                boneRangeChecked.insert(instance.modelId).second) {
                glm::vec3 centre(0.0f);
                for (const auto& bone : instance.boneMatrices) centre += glm::vec3(bone[3]);
                centre /= static_cast<float>(instance.boneMatrices.size());

                const float limit = radius * 4.0f;
                for (size_t bi = 0; bi < instance.boneMatrices.size(); ++bi) {
                    const glm::vec3 t(instance.boneMatrices[bi][3]);
                    const float stray = glm::length(t - centre);
                    if (stray <= limit) continue;
                    LOG_WARNING("M2 '", m.name, "' bone ", bi, " of ",
                                instance.boneMatrices.size(), " sits ", stray,
                                " from where the rest of its skeleton is, in a model whose"
                                " bounding radius is ", radius,
                                " - anything weighted to it is drawn stretched");
                    break;
                }
            }
        }

        const uint32_t boneCount = static_cast<uint32_t>(instance.boneMatrices.size());
        if (boneCount > MEGA_BONE_MATRIX_CAPACITY - nextOffset) {
            instance.megaBoneOffset = 0;  // Overflow - use identity
            continue;
        }

        instance.megaBoneOffset = nextOffset;

        // Upload bone matrices to mega buffer - only when they were recomputed
        // since the last upload into this frame's buffer, or the instance's
        // slot moved (animated set changed). Most animated instances are
        // distance/frustum/frame-skip culled and keep their previous bones, so
        // skipping their memcpy avoids megabytes of redundant writes per frame.
        if (megaBoneMapped_[frameIndex] &&
            (instance.bonesDirty[frameIndex] ||
             instance.megaBoneUploadedSlot[frameIndex] != instance.megaBoneOffset)) {
            auto* dst = static_cast<glm::mat4*>(megaBoneMapped_[frameIndex]) + instance.megaBoneOffset;
            memcpy(dst, instance.boneMatrices.data(), boneCount * sizeof(glm::mat4));
            instance.bonesDirty[frameIndex] = false;
            instance.megaBoneUploadedSlot[frameIndex] = instance.megaBoneOffset;
        }

        nextOffset += boneCount;
    }
}

// Dispatch GPU frustum culling compute shader into the primary frame command
// buffer. render() consumes the completed output left in this frame slot from
// its previous use; this dispatch produces results for the slot's next reuse.
void M2Renderer::writeInstanceLight(M2InstanceGPU& e, const M2Instance& inst) const {
    // Every field, every time: the entry is in write-combined memory and is
    // never read back, so whatever the slot held last frame must not show.
    if (inst.detailDoodad) {
        e.flags = kInstanceDetailDoodad | (inst.interiorPass ? kInstanceInteriorPass : 0);
        e.interiorAmbient = inst.detailColor;
        e.interiorDirect = glm::vec4(inst.detailNormal, groundDetailMaxDistance_);
        return;
    }
    e.flags = (inst.interiorLit ? kInstanceInteriorLit : 0) | (inst.interiorPass ? kInstanceInteriorPass : 0) |
              (inst.worldObjectLit && !inst.interiorLit ? kInstanceWorldObject : 0) |
              (inst.worldObjectLit && inst.interiorLit ? kInstanceFloorLit : 0);
    // A game object outside takes the zone's light as the frame dims it -
    // the sun's glare, the Debug Graphics strengths - as the units and the
    // world around it do (wmo_doodad_light::drawnObjectLight).
    namespace wl = pipeline::wmo_doodad_light;
    const bool zoneLit = inst.worldObjectLit && !inst.interiorLit;
    e.interiorAmbient = glm::vec4(wl::drawnObjectLight(inst.interiorAmbient, zoneLit, frameAmbientScale_), 0.0f);
    e.interiorDirect = glm::vec4(wl::drawnObjectLight(inst.interiorDirect, zoneLit, frameDirectScale_),
                                 inst.interiorTowardSun);
}

void M2Renderer::gatherVertexLights(bool on, const std::function<void(m2_vertex_lights::Grid&)>& others) {
    // This frame's lights (m2_vertex_lights.hpp): every point light of a
    // model stepped this frame, while its visibility track holds (0x00828a00
    // -> 0x008356f0), its diffuse colour times its intensity times the
    // model's alpha (0x0082f0f0). Outside the ray traced lighting only; under
    // it the trace lights the models.
    ++vertexLightFrame_;
    if (vertexLightFrame_ == 0) vertexLightFrame_ = 1;
    vertexLightSetCount_.store(1, std::memory_order_relaxed);
    vertexLightGrid_.clear();
    vertexLightsWanted_ = on;
    if (!vertexLightsWanted_) return;
    if (others) others(vertexLightGrid_);
    for (size_t idx : boneWorkIndices_) {
        if (idx >= instances.size()) continue;
        const M2Instance& inst = instances[idx];
        const M2ModelGPU* model = inst.cachedModel;
        if (!model || model->lights.empty()) continue;
        const auto& gs = model->globalSequenceDurations;
        const float alpha = std::clamp(inst.fade, 0.0f, 1.0f);
        for (const auto& light : model->lights) {
            if (light.type != 1) continue;
            const int seq = inst.currentSequenceIndex;
            if (m2_track::sampleFloat(light.visibility, seq, inst.animTime, inst.globalSequenceTime, gs, 1.0f) == 0.0f)
                continue;
            const glm::vec3 color = m2_track::sampleVec3(light.diffuseColor, seq, inst.animTime,
                                                         inst.globalSequenceTime, gs, glm::vec3(0.0f));
            const float intensity =
                m2_track::sampleFloat(light.diffuseIntensity, seq, inst.animTime, inst.globalSequenceTime, gs, 0.0f);
            m2_vertex_lights::Light l;
            l.diffuse = glm::max(color, glm::vec3(0.0f)) * std::max(intensity, 0.0f) * alpha;
            if (std::max(l.diffuse.r, std::max(l.diffuse.g, l.diffuse.b)) <= 0.0f) continue;
            glm::mat4 toWorld = inst.modelMatrix;
            if (light.bone >= 0 && static_cast<size_t>(light.bone) < inst.boneMatrices.size()) {
                toWorld = toWorld * inst.boneMatrices[static_cast<size_t>(light.bone)];
            }
            l.position = glm::vec3(toWorld * glm::vec4(light.position, 1.0f));
            vertexLightGrid_.add(l);
        }
    }
}

void M2Renderer::writeInstanceVertexLights(uint32_t frameIndex, uint32_t slot, M2Instance& inst) {
    if (frameIndex >= 2 || !vertexLightMapped_[frameIndex] || slot >= MAX_INSTANCE_DATA) return;
    auto* setOf = static_cast<int32_t*>(vertexLightMapped_[frameIndex]);
    if (vertexLightGrid_.empty() || inst.detailDoodad) {
        setOf[slot] = 0;
        return;
    }
    // Picked once a frame per instance (0x00831af0 per model), then every
    // entry of it takes the same set.
    if (inst.vertexLightFrame != vertexLightFrame_) {
        inst.vertexLightFrame = vertexLightFrame_;
        inst.vertexLightSet = 0;
        const auto picked = vertexLightGrid_.pick(inst.cachedCullCenter);
        if (picked.count > 0) {
            const uint32_t set = vertexLightSetCount_.fetch_add(1, std::memory_order_relaxed);
            if (set < kMaxVertexLightSets) {
                auto* sets = reinterpret_cast<glm::vec4*>(setOf + MAX_INSTANCE_DATA);
                glm::vec4* dst = sets + size_t(set) * kVertexLightSetVec4s;
                for (int i = 0; i < m2_vertex_lights::kDrawn; ++i) {
                    const bool on = i < picked.count;
                    const auto& l = picked.lights[static_cast<size_t>(i)];
                    dst[2 * i] = on ? glm::vec4(l.position, 1.0f) : glm::vec4(0.0f);
                    dst[2 * i + 1] = on ? glm::vec4(l.diffuse, 0.0f) : glm::vec4(0.0f);
                }
                inst.vertexLightSet = set;
            } else if (!vertexLightSetsFull_) {
                vertexLightSetsFull_ = true;
                LOG_WARNING("M2 vertex lights: more than ", kMaxVertexLightSets,
                            " lit models this frame; the rest drawn without their point lights");
            }
        }
    }
    setOf[slot] = static_cast<int32_t>(inst.vertexLightSet);
}

void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                        const BlendedInterleave* interleave) {
    renderOpaque(cmd, perFrameSet, camera);
    renderBlended(cmd, perFrameSet, interleave);
}

void M2Renderer::renderOpaque(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                              bool gbuffer) {
    if (gbuffer && gbufferVariants_.empty()) return;
    if (instances.empty() || !opaquePipeline_) {
        return;
    }

    // Debug: log once when we start rendering
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOG_INFO("M2 render: ", instances.size(), " instances, ", models.size(), " models");
    }

    // Periodic diagnostic: report render pipeline stats every 10 seconds
    static int diagCounter = 0;
    if (++diagCounter == 600) { // ~10s at 60fps
        diagCounter = 0;
        uint32_t totalValid = 0, totalAnimated = 0, totalBonesReady = 0, totalMegaBoneOk = 0;
        for (const auto& inst : instances) {
            if (inst.cachedIsValid) totalValid++;
            if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
                totalAnimated++;
                if (!inst.boneMatrices.empty()) totalBonesReady++;
                if (inst.megaBoneOffset != 0) totalMegaBoneOk++;
            }
        }
        LOG_INFO("M2 diag: total=", instances.size(),
                 " valid=", totalValid,
                 " animated=", totalAnimated,
                 " bonesReady=", totalBonesReady,
                 " megaBoneOk=", totalMegaBoneOk,
                 " visible=", sortedVisible_.size(),
                 " draws=", lastDrawCallCount,
                 " clutterChunks=", clutterChunks_.size(),
                 " clutterEntries=", clutterLive_,
                 " clutterDrawn=", clutterVisible_);
    }

    lastDrawCallCount = 0;

    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    std::optional<core::ProfileScope> phaseScope;

    const glm::vec3 camPos = camera.getPosition();
    cullViewPos_ = camPos;
    cullViewDir_ = camera.getForward();

    // Where this pass's three milliseconds go.
    //
    // Recording the M2 secondary is the largest single piece of CPU work in
    // the frame - renderWorld waits on the slowest worker and this is always
    // it - and the stage around it cannot see whether that is the cull, the
    // sort or the recording itself.
    static const bool m2Profile = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    const auto m2T0 = std::chrono::steady_clock::now();

    // Build sorted visible instance list
    sortedVisible_.clear();
    transparentVisible_.clear();
    skyDiagDrawsOpaque_ = 0;
    skyDiagDrawsTransparent_ = 0;
    const size_t expectedVisible = std::min(instances.size() / 3, size_t(600));
    if (sortedVisible_.capacity() < expectedVisible) {
        sortedVisible_.reserve(expectedVisible);
    }
    if (transparentVisible_.capacity() < expectedVisible / 4)
        transparentVisible_.reserve(expectedVisible / 4);

    // What is drawn is decided here, by this frame's camera. A verdict recorded on
    // the GPU comes back two frames old, by the camera of then, and left what came
    // in at the screen's edge on a fast turn out of the frame.
    Frustum frustum;
    {
        const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(vp);
    }
    const float maxPossibleDistSq = cappedViewDistance() * cappedViewDistance();

    const uint32_t totalInstances = static_cast<uint32_t>(instances.size());
    struct VisibleChunk {
        std::vector<VisibleEntry> opaque;
        std::vector<VisibleEntry> transparent;
    };

    // Visibility classification is independent per instance and was the
    // remaining monolithic M2 CPU pass. Split dense scenes across a few pool
    // workers; the caller handles the final chunk so nested use from the M2
    // render worker cannot deadlock the shared pool.
    const uint32_t chunkCount = totalInstances >= 2048
        ? std::min<uint32_t>(4, (totalInstances + 1023) / 1024)
        : 1;
    phaseScope.reset();
    phaseScope.emplace("m2: classify visible", core::FrameProfiler::isMainThread() ? core::ProfileKind::Cpu : core::ProfileKind::Worker);
    std::vector<VisibleChunk> chunks(chunkCount);
    auto classifyRange = [&](uint32_t chunk, uint32_t begin, uint32_t end) {
        auto& out = chunks[chunk];
        out.opaque.reserve((end - begin) / 3);
        out.transparent.reserve((end - begin) / 12);
        for (uint32_t i = begin; i < end; ++i) {
            const auto& instance = instances[i];
            // Ground cover drawn by its chunk (renderClutterChunks).
            if (instance.clutterChunk != 0) continue;
            float distSq;
            float effectiveMaxDistSq;

            // A doodad by its size class, the rest as instanceMaxDistSq says.
            const float instanceMaxDistSq = this->instanceMaxDistSq(instance);

            if (forceNoCull_) {
                if (!instance.cachedIsValid) continue;
                glm::vec3 toCam = instance.cachedCullCenter - camPos;
                distSq = glm::dot(toCam, toCam);
                effectiveMaxDistSq = instanceMaxDistSq;
            } else {
                if (!instance.cachedIsValid || instance.cachedIsInvisibleTrap) continue;
                glm::vec3 toCam = instance.cachedCullCenter - camPos;
                distSq = glm::dot(toCam, toCam);
                if (distSq > maxPossibleDistSq) continue;
                effectiveMaxDistSq = instanceMaxDistSq;
                if (distSq > effectiveMaxDistSq) continue;
                float paddedRadius = instance.cachedPaddedRadius;
                if (paddedRadius > 0.0f && !frustum.intersectsSphere(instance.cachedCullCenter, paddedRadius)) continue;
            }

            VisibleEntry visible{.index = i, .modelId = instance.modelId, .distSq = distSq, .effectiveMaxDistSq = effectiveMaxDistSq};
            // A faded instance is drawn blended, all of it - see M2Instance::fade.
            const bool faded = instance.fade < 0.999f;
            if (!faded) out.opaque.push_back(visible);
            if (faded || (instance.cachedModel &&
                (instance.cachedModel->hasTransparentBatches || instance.cachedModel->isSpellEffect))) {
                out.transparent.push_back(visible);
            }
        }
    };

    std::vector<std::future<void>> visibilityFutures;
    visibilityFutures.reserve(chunkCount > 0 ? chunkCount - 1 : 0);
    const uint32_t chunkSize = (totalInstances + chunkCount - 1) / chunkCount;
    for (uint32_t chunk = 0; chunk + 1 < chunkCount; ++chunk) {
        const uint32_t begin = chunk * chunkSize;
        const uint32_t end = std::min(totalInstances, begin + chunkSize);
        visibilityFutures.push_back(core::ThreadPool::frameWorkers().submit(
            [&, chunk, begin, end]() { classifyRange(chunk, begin, end); }));
    }
    const uint32_t lastChunk = chunkCount - 1;
    classifyRange(lastChunk, lastChunk * chunkSize, totalInstances);
    for (auto& future : visibilityFutures) future.get();

    for (auto& chunk : chunks) {
        sortedVisible_.insert(sortedVisible_.end(),
                              std::make_move_iterator(chunk.opaque.begin()),
                              std::make_move_iterator(chunk.opaque.end()));
        transparentVisible_.insert(transparentVisible_.end(),
                                   std::make_move_iterator(chunk.transparent.begin()),
                                   std::make_move_iterator(chunk.transparent.end()));
    }

    // The furthest doodad actually drawn, for the diagnostic that sets it
    // against the furthest terrain chunk. See Renderer::logViewDistanceDiag.
    furthestDrawnSq_ = 0.0f;
    for (const auto& e : sortedVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;
    for (const auto& e : transparentVisible_)
        if (e.distSq > furthestDrawnSq_) furthestDrawnSq_ = e.distSq;

    // Whether the sky model survived culling this frame, when this is the
    // renderer that draws one.
    //
    // Reported as flickering while the camera turns and steady while it does
    // not, with everything upstream measured and holding still: the lighting
    // inputs, the model's clock, the frame time. What that leaves is the dome
    // being drawn on some frames and not others, and this says so in one line
    // per change rather than one per frame.
    // WOWEE_SKY_M2_OPAQUE_ONLY=1 drops the sky model's blended layers.
    //
    // The model is the culprit - the flicker goes with WOWEE_NO_SKY_M2 - and it
    // is not a small one: hellfireskybox.m2 carries 23 textures over 34 render
    // flags, with five transparency tracks and seven UV animations driven by
    // six global sequences. The brightening and dimming is those alpha tracks.
    // This says whether the flicker is in the blended layers or in the base the
    // opaque pass draws, which halves what is left to read.
    static const bool skyOpaqueOnly = std::getenv("WOWEE_SKY_M2_OPAQUE_ONLY") != nullptr;
    if (skyMode_ && skyOpaqueOnly) transparentVisible_.clear();

    if (skyMode_) {
        const bool drawn = !sortedVisible_.empty() || !transparentVisible_.empty();
        if (drawn != skyDiagWasDrawn_) {
            skyDiagWasDrawn_ = drawn;
            LOG_INFO("skyM2 cull: ", drawn ? "DRAWN" : "CULLED",
                     " opaque=", sortedVisible_.size(),
                     " transparent=", transparentVisible_.size(),
                     " instances=", instances.size());
        }
    }

    // Two-pass rendering: opaque/alpha-test first (depth write ON), then transparent/additive
    // (depth write OFF, sorted back-to-front) so transparent geometry composites correctly
    // against all opaque geometry rather than only against what was rendered before it.

    // Pass 1: sort by modelId for minimum buffer rebinds (opaque batches)
    //
    // Ordering the groups front-to-back was tried, on the theory that cutout
    // foliage cannot be rejected before it is shaded so the near trees should
    // lay depth down first. It moved nothing: doodads measured 14.66ms against
    // 16.55 and 13.38 either side of it, which is the middle of the spread.
    // What it did cost was a hash map over every visible instance, two index
    // vectors and a copy of the whole list, every frame, in the pass that
    // turned out to be the largest single piece of CPU work in the frame.
    phaseScope.reset();
    phaseScope.emplace("m2: sort", core::FrameProfiler::isMainThread() ? core::ProfileKind::Cpu : core::ProfileKind::Worker);
    const auto m2T1 = std::chrono::steady_clock::now();
    // Ground clutter last. The instance buffer held 65536 a frame, and a
    // dense Elwynn verge alone came near that: whatever sorted after the cap
    // was dropped, and by model id that was fences and signposts - doodads
    // that vanished for as long as the camera looked down the road. Behind
    // everything else, only the clutter can be what does not fit.
    std::sort(sortedVisible_.begin(), sortedVisible_.end(),
              [this](const VisibleEntry& a, const VisibleEntry& b) {
                  const bool ga = instances[a.index].cachedIsGroundDetail;
                  const bool gb = instances[b.index].cachedIsGroundDetail;
                  if (ga != gb) return gb;
                  return a.modelId < b.modelId;
              });
    const auto m2T2 = std::chrono::steady_clock::now();
    phaseScope.reset();
    phaseScope.emplace("m2: record draws", core::FrameProfiler::isMainThread() ? core::ProfileKind::Cpu : core::ProfileKind::Worker);
    // Reported at the end of the pass; see m2Profile above.
    struct M2Phases { double cull, sort, record; std::size_t visible, instances; };
    const auto sayPhases = [&](const M2Phases& p) {
        static auto lastSaid = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSaid <= std::chrono::seconds(10)) return;
        lastSaid = now;
        LOG_WARNING("  m2 record: cull ", p.cull, "ms, sort ", p.sort,
                    "ms, draws ", p.record, "ms over ", p.visible,
                    " visible of ", p.instances, " instances");
    };

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE - skipping M2 render");
        return;
    }

    // Bind per-frame descriptor set (set 0) - shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    currentPipeline = gbuffer ? gbufferVariant(opaquePipeline_, 0x04) : opaquePipeline_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);

    // Bind dummy bone set (set 2) so non-animated draws have a valid binding.
    // Bind mega bone SSBO instead - all instances index into one buffer via boneBase.
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }

    // Bind instance data SSBO (set 3) - per-instance transforms, fade, bones
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // Reset instance SSBO write cursor for this frame
    instanceDataCount_ = 0;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // =====================================================================
    // Opaque pass - instanced draws grouped by (modelId, LOD)
    // =====================================================================
    // sortedVisible_ is already sorted by modelId so consecutive entries share
    // the same vertex/index buffer.  Within each model group we sub-group by
    // targetLOD to guarantee all instances in one vkCmdDrawIndexed use the
    // same batch set.  Per-instance data (model matrix, fade, bones) is
    // written to the instance SSBO; the shader reads it via gl_InstanceIndex.
    {
        struct PendingInstance {
            uint32_t instanceIdx;
            float fadeAlpha;
            bool useBones;
            uint16_t targetLOD;
        };
        std::vector<PendingInstance> pending;
        pending.reserve(128);

        size_t visStart = 0;
        while (visStart < sortedVisible_.size()) {
            // Find group of consecutive entries with same modelId
            uint32_t groupModelId = sortedVisible_[visStart].modelId;
            size_t groupEnd = visStart;
            while (groupEnd < sortedVisible_.size() && sortedVisible_[groupEnd].modelId == groupModelId)
                groupEnd++;

            // Pull the model through the first entry's instance.cachedModel pointer
            // (set at addInstance) instead of doing models.find(groupModelId) per group.
            const auto& firstEntry = sortedVisible_[visStart];
            if (firstEntry.index >= instances.size() || !instances[firstEntry.index].cachedModel) {
                visStart = groupEnd;
                continue;
            }
            const M2ModelGPU& model = *instances[firstEntry.index].cachedModel;
            if (skipGroundDetail_ && model.isGroundDetail) {
                visStart = groupEnd;
                continue;
            }
            if (!model.vertexBuffer || !model.indexBuffer) {
                visStart = groupEnd;
                continue;
            }

            bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
            // Collect per-instance data for this model group
            pending.clear();
            for (size_t vi = visStart; vi < groupEnd; vi++) {
                const auto& entry = sortedVisible_[vi];
                if (entry.index >= instances.size()) continue;
                auto& instance = instances[entry.index];

                // The distance fade, as the client takes it (0x00791cb0).
                float fadeAlpha = instanceDistanceFade(instance, entry.distSq);
                // Ground detail used to be held at 0.82 here. This is the
                // opaque pass: nothing blended it, so it was a number with no
                // effect - and now that the cutout pipeline turns alpha into
                // coverage, keeping it would punch a fifth of the pixels out
                // of every tuft of grass in reach.
                float instanceFadeAlpha = fadeAlpha;

                // Bone readiness check
                if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
                bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
                if (needsBones && instance.megaBoneOffset == 0) continue;

                // LOD selection
                // Every section of the loaded skin is drawn: the client picks
                // detail by loading a different .skin, and draws all of its
                // sections (FUN_008203b0). A section's level is the high half
                // of its index start, not a level of detail.
                const uint16_t targetLOD = 0;

                pending.push_back({.instanceIdx = entry.index, .fadeAlpha = instanceFadeAlpha, .useBones = needsBones, .targetLOD = targetLOD});
            }

            if (pending.empty()) { visStart = groupEnd; continue; }

            // Clutter gets what is left once the second pass has its room -
            // every transparent instance draws again there - and keeps its
            // nearest tufts when that is not all of them.
            if (model.isGroundDetail) {
                const size_t reserved = transparentVisible_.size();
                const size_t room = MAX_INSTANCE_DATA > instanceDataCount_ + reserved
                                        ? MAX_INSTANCE_DATA - instanceDataCount_ - reserved : 0;
                if (pending.size() > room) {
                    std::nth_element(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(room), pending.end(),
                                     [&](const PendingInstance& a, const PendingInstance& b) {
                                         const glm::vec3 da = instances[a.instanceIdx].cachedCullCenter - camPos;
                                         const glm::vec3 db = instances[b.instanceIdx].cachedCullCenter - camPos;
                                         return glm::dot(da, da) < glm::dot(db, db);
                                     });
                    pending.resize(room);
                }
            }

            // Sort by targetLOD so each sub-group occupies a contiguous SSBO range
            std::sort(pending.begin(), pending.end(),
                      [](const PendingInstance& a, const PendingInstance& b) { return a.targetLOD < b.targetLOD; });

            // Bind vertex/index buffers once per model group
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Write base instance data to SSBO (uvOffset=0 - overridden for tex-anim batches)
            uint32_t baseSSBOOffset = instanceDataCount_;
            size_t writtenInstances = 0;
            for (const auto& p : pending) {
                if (instanceDataCount_ >= MAX_INSTANCE_DATA) break;
                auto& inst = instances[p.instanceIdx];
                auto& e = instSSBO[instanceDataCount_];
                e.model = inst.modelMatrix;
                e.uvOffset = glm::vec2(0.0f);
                e.uvLinear = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                e.uvOffset2 = glm::vec4(0.0f);
                e.uvLinear2 = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                e.colorMul = glm::vec4(1.0f, 1.0f, 1.0f, -1.0f);
                e.fadeAlpha = p.fadeAlpha;
                e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                e.highlight = inst.highlight;
                writeInstanceLight(e, inst);
                writeInstanceVertexLights(frameIndex, instanceDataCount_, inst);
                instanceDataCount_++;
                ++writtenInstances;
            }

            // Drop what did not fit. The loop above stops writing at the cap,
            // but the LOD sub-groups below are ranges over `pending` and were
            // still being drawn in full - groupSSBOOffset then runs past the
            // end of the buffer and the vertex shader reads instance data that
            // is not there. That is a real out-of-bounds read on the GPU, not a
            // missing model: it fires once per instance past the cap, hundreds
            // of times a frame, and the device is lost seconds later.
            //
            // Truncating is safe here precisely because `pending` was sorted by
            // LOD before the write: the sub-groups are contiguous and in the
            // same order, so cutting the tail cuts whole instances rather than
            // splitting a range.
            if (writtenInstances < pending.size()) {
                static bool warnedInstanceCap = false;
                if (!warnedInstanceCap) {
                    warnedInstanceCap = true;
                    LOG_WARNING("M2Renderer: instance buffer full at ", MAX_INSTANCE_DATA,
                                "; dropping ", pending.size() - writtenInstances,
                                " instances of '", model.name, "' this frame");
                }
                pending.resize(writtenInstances);
            }

            // Process LOD sub-groups within this model group
            size_t lodIdx = 0;
            while (lodIdx < pending.size()) {
                uint16_t lod = pending[lodIdx].targetLOD;
                size_t lodEnd = lodIdx + 1;
                while (lodEnd < pending.size() && pending[lodEnd].targetLOD == lod) lodEnd++;
                uint32_t groupSize = static_cast<uint32_t>(lodEnd - lodIdx);
                uint32_t groupSSBOOffset = baseSSBOOffset + static_cast<uint32_t>(lodIdx);

                // What each batch of a named model does at draw time, once.
                //
                // WOWEE_M2_BATCH_DIAG says what a batch IS, at load. It could
                // not say whether the batch was drawn, and a tree reported as
                // missing a section of trunk is exactly that question: four
                // readings of the model data in a row said every batch was
                // present and correct, which it is, so the answer has to come
                // from the frame rather than from the file.
                static const std::string kDrawDiag = [] {
                    const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
                    std::string t = v ? v : "";
                    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    return t;
                }();
                bool diagThisModel = false;
                if (!kDrawDiag.empty()) {
                    std::string lowerName = model.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    diagThisModel = lowerName.find(kDrawDiag) != std::string::npos;
                }
                // WOWEE_M2_ONLY_BATCH=<n> draws just that batch of the named
                // model and hides the rest.
                //
                // Everything checkable has now been checked on these trees and
                // every answer came back correct: the trunk mesh is continuous
                // from root to tip, its submesh level matches, its texture
                // loads, its mips preserve coverage, and the draw diagnostic
                // says every batch reaches the frame. A section still looks
                // missing. Seeing the trunk by itself is the difference
                // between the trunk being wrong and the canopy eating it, and
                // no amount of reading the files can tell those apart.
                static const int kOnlyBatch = [] {
                    const char* v = std::getenv("WOWEE_M2_ONLY_BATCH");
                    return (v && *v) ? std::atoi(v) : -1;
                }();
                for (size_t bi = 0; bi < model.batches.size(); bi++) {
                    const auto& batch = model.batches[bi];
                    const char* skipped = nullptr;
                    if (kOnlyBatch >= 0 && diagThisModel &&
                        bi != static_cast<size_t>(kOnlyBatch)) {
                        skipped = "WOWEE_M2_ONLY_BATCH";
                    }
                    else if (batch.indexCount == 0) skipped = "no indices";
                    else if (batch.batchOpacity < 0.01f) skipped = "opacity is zero";
                    else if (!skyBatchAllowed(skyMode_, bi)) skipped = "sky batch rule";
                    else if (suppressBakedStars_ && batch.starLayer) skipped = "baked star layer";
                    if (diagThisModel) {
                        static std::set<std::pair<const void*, size_t>> saidDraw;
                        if (saidDraw.size() < 64 && saidDraw.insert({&model, bi}).second) {
                            LOG_WARNING("M2 DRAW '", model.name, "' batch ", bi,
                                        " lod=", lod, " submeshLevel=", batch.submeshLevel,
                                        " idx=", batch.indexCount,
                                        " opacity=", batch.batchOpacity,
                                        (skipped ? "  SKIPPED: " : "  drawn"),
                                        (skipped ? skipped : ""));
                        }
                    }
                    if (skipped) continue;
                    // Opaque gate. The client draws every batch's mesh by its
                    // material and adds no glow sprite in place of or beside it.
                    // A spell's opaque layers are opaque like anyone else's:
                    // the client draws every batch by its blend mode.
                    if (batch.blendMode >= 2) continue;

                    // Handle texture animation: if this batch has per-instance uvOffset,
                    // write a separate SSBO range with the correct offsets.
                    bool hasBatchTexAnim = (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation)
                                           || batch.colorAnimated;
                    uint32_t drawOffset = groupSSBOOffset;
                    if (hasBatchTexAnim && instanceDataCount_ + groupSize <= MAX_INSTANCE_DATA) {
                        drawOffset = instanceDataCount_;
                        // Hoist per-batch lookups: the transform pointer is fixed for
                        // every instance in this group; only the sampled translation
                        // varies (per-instance animTime).
                        const pipeline::M2TextureTransform* tt = batchStageTransform(model, batch, 0);
                        const pipeline::M2TextureTransform* tt2 = batchStageTransform(model, batch, 1);
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            const auto& p = pending[j];
                            auto& inst = instances[p.instanceIdx];
                            glm::vec2 uvOffset(0.0f);
                            glm::vec4 uvLinear(1.0f, 0.0f, 0.0f, 1.0f);
                            glm::vec2 uvOffset2(0.0f);
                            glm::vec4 uvLinear2(1.0f, 0.0f, 0.0f, 1.0f);
                            if (tt) {
                                setUvTransform(uvLinear, uvOffset,
                                               m2_track::textureTransformMatrix(*tt, inst.currentSequenceIndex,
                                                               inst.animTime, inst.globalSequenceTime,
                                                               model.globalSequenceDurations));
                            }
                            if (tt2) {
                                setUvTransform(uvLinear2, uvOffset2,
                                               m2_track::textureTransformMatrix(*tt2, inst.currentSequenceIndex,
                                                               inst.animTime, inst.globalSequenceTime,
                                                               model.globalSequenceDurations));
                            }
                            // Rebuild the entry from CPU-side data rather than copying it
                            // out of the base entry. instSSBO lives in write-combined
                            // upload memory: writing it is cheap, but reading it back is
                            // uncached and costs microseconds per instance.
                            auto& e = instSSBO[instanceDataCount_];
                            e.model = inst.modelMatrix;
                            e.uvOffset = uvOffset;
                            e.uvLinear = uvLinear;
                            e.uvOffset2 = glm::vec4(uvOffset2, 0.0f, 0.0f);
                            e.uvLinear2 = uvLinear2;
                            e.colorMul = batchColorMul(model, batch, inst.currentSequenceIndex,
                                                       inst.animTime, inst.globalSequenceTime);
                            e.fadeAlpha = p.fadeAlpha;
                            e.useBones = (p.useBones && !kM2NoSkinning) ? 1 : 0;
                            e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                            e.boneCount = static_cast<int32_t>(inst.boneMatrices.size());
                            e.highlight = inst.highlight;
                            writeInstanceLight(e, inst);
                            writeInstanceVertexLights(frameIndex, instanceDataCount_, inst);
                            instanceDataCount_++;
                        }
                    }

                    // Pipeline selection (per-model/batch, not per-instance)
                    // The client alpha-tests alpha-key batches and nothing
                    // else (FUN_0081fe90): a blended batch is blended, and an
                    // opaque one is opaque whatever its texture's alpha. It
                    // forces no batch additive and never keys by colour,
                    // spell, fire and ground clutter alike: the client draws
                    // a batch by its material.
                    // Ground clutter is drawn by DetailDoodad's own state
                    // (0x007b2d30): GxBlend_Alpha with the 0x80 alpha ref the
                    // shader tests, depth written, nothing culled.
                    const bool detailGroup = instances[firstEntry.index].detailDoodad;
                    const bool forceCutout =
                        !detailGroup && !m2BlendIsAdditive(batch.blendMode) &&
                        m2BatchNeedsAlphaTest(batch.blendMode, batch.hasAlpha);
                    const uint8_t effectiveBlendMode = forceCutout ? 1 : batch.blendMode;

                    VkPipeline desiredPipeline = forceCutout ? cutoutPipeline_
                                                             : blendPipelineFor(effectiveBlendMode);
                    // Cull, depth test and depth write from the material's
                    // flags, as the client sets them for every blend mode.
                    desiredPipeline = detailGroup
                        ? pipelineVariant(alphaTestPipeline_, 0x04, false)
                        : pipelineVariant(desiredPipeline, batch.materialFlags, false);
                    if (gbuffer) {
                        desiredPipeline = detailGroup
                            ? gbufferVariant(alphaTestPipeline_, 0x04)
                            : gbufferVariant(forceCutout ? cutoutPipeline_ : blendPipelineFor(effectiveBlendMode),
                                             batch.materialFlags);
                        if (!desiredPipeline) continue;
                    }
                    if (desiredPipeline != currentPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                        currentPipeline = desiredPipeline;
                    }

                    // Update material UBO
                    if (batch.materialUBOMapped) {
                        auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                        mat->alphaTest = forceCutout ? 1 : 0;
                        mat->blendMode = batch.blendMode;
                    }

                    // Bind material descriptor set (set 1)
                    if (!batch.materialSet) continue;
                    if (batch.materialSet != currentMaterialSet) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                        currentMaterialSet = batch.materialSet;
                    }

                    // Push constants + instanced draw
                    M2PushConstants pc;
                    pc.texCoordSet = batch.coordSources;
                    fillModelPush(pc, skyMode_);
                    pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    if (skyMode_) ++skyDiagDrawsOpaque_;
                    lastDrawCallCount++;
                }

                lodIdx = lodEnd;
            }

            visStart = groupEnd;
        }
    }

    // Ground cover by chunk, last as the instance-at-a-time clutter was.
    {
        phaseScope.reset();
        phaseScope.emplace("m2: ground cover chunks",
                           core::FrameProfiler::isMainThread() ? core::ProfileKind::Cpu : core::ProfileKind::Worker);
        clutterVisible_ = renderClutterChunks(cmd, frustum, camPos, gbuffer, currentPipeline,
                                              currentMaterialSet, frameIndex);
        // Whether the one buffer every instance shared, as it was, would have
        // run out with these: the clutter it could not hold was dropped,
        // farthest first. Drawn from its chunks, none is.
        const size_t sharedNeed = instanceDataCount_ + transparentVisible_.size() + clutterVisible_;
        if (sharedNeed > MAX_INSTANCE_DATA) {
            static auto lastSaid = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSaid > std::chrono::seconds(10)) {
                lastSaid = now;
                LOG_WARNING("Ground cover: ", clutterVisible_, " tufts drawn from their chunks; the shared "
                            "instance buffer (", MAX_INSTANCE_DATA, ") would have dropped ",
                            sharedNeed - MAX_INSTANCE_DATA, " of them, the farthest");
            }
        }
    }
    phaseScope.reset();

    // The opaque half ends here, for the frame profiler. Taken and cleared so
    // the sky's model renderers, which share this code, never write it.
    if (gpuOpaqueMark_ != UINT32_MAX) {
        vkCtx_->gpuMarkAt(cmd, gpuOpaqueMark_, "m2 opaque");
        gpuOpaqueMark_ = UINT32_MAX;
    }

    if (m2Profile) {
        const auto m2T3 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        sayPhases({.cull = ms(m2T0, m2T1),
                   .sort = ms(m2T1, m2T2),
                   .record = ms(m2T2, m2T3),
                   .visible = sortedVisible_.size(),
                   .instances = instances.size()});
    }
}

// Whether a ground cover model can be drawn from the chunk store: nothing of
// what it draws with may change per frame but its bones. A batch whose colour
// or texture animates, or a blended layer drawn back to front with the rest,
// keeps the model on the instance-at-a-time path. Every detail model of
// Mulgore carries a texture transform, but one whose tracks hold still.
const char* M2Renderer::clutterUnchunkable(const M2ModelGPU& model) const {
    if (model.hasTransparentBatches) return "blended layers";
    if (model.isSpellEffect) return "spell effect";
    if (!model.vertexBuffer || !model.indexBuffer) return "no buffers";
    for (const auto& batch : model.batches) {
        if (batch.blendMode >= 2) continue;
        if (batch.colorAnimated) return "animated colour";
        for (uint32_t stage = 0; stage < 2; ++stage) {
            const auto* tt = batchStageTransform(model, batch, stage);
            if (tt && !textureTransformConstant(*tt)) return "texture animation";
        }
    }
    return nullptr;
}

uint32_t M2Renderer::renderClutterChunks(VkCommandBuffer cmd, const Frustum& frustum, const glm::vec3& camPos,
                                         bool gbuffer, VkPipeline& currentPipeline,
                                         VkDescriptorSet& currentMaterialSet, uint32_t frameIndex) {
    clutterDraws_.clear();
    if (clutterChunks_.empty() || frameIndex >= 2 || !clutterListMapped_[frameIndex]) return 0;

    // A chunk whose box is in the frustum and comes within the view distance;
    // within it, the shader drops each tuft whose centre is past that
    // distance, which is the test the instance-at-a-time cull made.
    const float viewDist = cappedViewDistance();
    const float distSq = viewDist * viewDist;
    for (const auto& [handle, chunk] : clutterChunks_) {
        const glm::vec3 nearest = glm::clamp(camPos, chunk.boxMin, chunk.boxMax);
        const glm::vec3 d = nearest - camPos;
        if (!forceNoCull_) {
            if (glm::dot(d, d) > distSq) continue;
            if (!frustum.intersectsAABB(chunk.boxMin, chunk.boxMax)) continue;
        }
        for (const auto& group : chunk.groups) clutterDraws_.push_back({group.modelId, &group});
    }
    if (clutterDraws_.empty()) return 0;
    std::sort(clutterDraws_.begin(), clutterDraws_.end(), [](const ClutterDraw& a, const ClutterDraw& b) {
        if (a.modelId != b.modelId) return a.modelId < b.modelId;
        return a.group->first < b.group->first;
    });

    auto* list = static_cast<glm::ivec2*>(clutterListMapped_[frameIndex]);
    const uint32_t capacity = clutterListCapacity_[frameIndex];
    uint32_t written = 0;
    for (size_t run = 0; run < clutterDraws_.size();) {
        const uint32_t modelId = clutterDraws_[run].modelId;
        size_t runEnd = run;
        while (runEnd < clutterDraws_.size() && clutterDraws_[runEnd].modelId == modelId) ++runEnd;
        const M2ModelGPU& model = *clutterDraws_[run].group->model;
        if (skipGroundDetail_ && model.isGroundDetail) { run = runEnd; continue; }

        // This model's entries, chunk after chunk; an animated tuft with its
        // bones' base this frame, or left out while it has none, as the
        // instance-at-a-time pass leaves it.
        const uint32_t listStart = written;
        for (size_t g = run; g < runEnd; ++g) {
            const ClutterGroup& group = *clutterDraws_[g].group;
            if (written + group.count > capacity) break;
            if (!group.animated) {
                for (uint32_t k = 0; k < group.count; ++k) list[written++] = glm::ivec2(group.first + k, 0);
                continue;
            }
            for (uint32_t k = 0; k < group.count; ++k) {
                auto idxIt = instanceIndexById.find(group.ids[k]);
                if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) continue;
                const M2Instance& inst = instances[idxIt->second];
                if (inst.boneMatrices.empty() || inst.megaBoneOffset == 0) continue;
                list[written++] = glm::ivec2(group.first + k, static_cast<int32_t>(inst.megaBoneOffset));
            }
        }
        const uint32_t count = written - listStart;
        run = runEnd;
        if (count == 0) continue;

        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
        vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        const glm::vec3 centre = (model.boundMin + model.boundMax) * 0.5f;
        for (const auto& batch : model.batches) {
            if (batch.indexCount == 0 || batch.batchOpacity < 0.01f) continue;
            if (suppressBakedStars_ && batch.starLayer) continue;
            if (batch.blendMode >= 2) continue;
            // DetailDoodad's own state (0x007b2d30), as for the instance-at-
            // a-time clutter: alpha-tested, depth written, nothing culled.
            VkPipeline desiredPipeline = gbuffer ? gbufferVariant(alphaTestPipeline_, 0x04)
                                                 : pipelineVariant(alphaTestPipeline_, 0x04, false);
            if (!desiredPipeline) continue;
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }
            if (batch.materialUBOMapped) {
                auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                mat->alphaTest = 0;
                mat->blendMode = batch.blendMode;
            }
            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                        &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }
            M2PushConstants pc;
            pc.texCoordSet = batch.coordSources;
            fillModelPush(pc, skyMode_);
            pc.instanceDataOffset = 0;
            pc.clutterList = static_cast<int32_t>(listStart);
            pc.clutterDistSq = forceNoCull_ ? std::numeric_limits<float>::max() : distSq;
            pc.centerX = centre.x;
            pc.centerY = centre.y;
            pc.centerZ = centre.z;
            // A texture track that holds still (clutterUnchunkable): its one
            // matrix, which the instance-at-a-time pass wrote for every tuft.
            if (const auto* tt = batchStageTransform(model, batch, 0)) {
                glm::vec2 off(0.0f);
                setUvTransform(pc.uvLinear, off,
                               m2_track::textureTransformMatrix(*tt, 0, 0.0f, 0.0f, model.globalSequenceDurations));
                pc.uvOffsets.x = off.x;
                pc.uvOffsets.y = off.y;
            }
            if (const auto* tt = batchStageTransform(model, batch, 1)) {
                glm::vec2 off(0.0f);
                setUvTransform(pc.uvLinear2, off,
                               m2_track::textureTransformMatrix(*tt, 0, 0.0f, 0.0f, model.globalSequenceDurations));
                pc.uvOffsets.z = off.x;
                pc.uvOffsets.w = off.y;
            }
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, batch.indexCount, count, batch.indexStart, 0, 0);
            lastDrawCallCount++;
        }
    }
    if (written >= capacity) {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARNING("Ground cover: the frame's chunk list is full at ", capacity, " entries");
        }
    }
    return written;
}

void M2Renderer::renderBlended(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                               const BlendedInterleave* interleave) {
    if (instances.empty() || !opaquePipeline_ || !perFrameSet) return;
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);
    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // =====================================================================
    // Pass 2: Transparent/additive batches - back-to-front per instance
    // =====================================================================
    // Transparent geometry must be drawn individually per instance in back-to-
    // front order for correct alpha compositing.  Each draw writes one
    // M2InstanceGPU entry and issues a single-instance indexed draw.
    std::sort(transparentVisible_.begin(), transparentVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.distSq > b.distSq; });

    // Everything the loop below keeps bound, bound again after another
    // pipeline layout has been drawn with in its middle.
    const auto rebindM2State = [&]() {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
        if (megaBoneSet_[frameIndex]) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
        } else if (dummyBoneSet_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
        }
        if (instanceSet_[frameIndex]) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
        }
        currentModelId = UINT32_MAX;
        currentModel = nullptr;
        currentModelValid = false;
        currentPipeline = VK_NULL_HANDLE;
        currentMaterialSet = VK_NULL_HANDLE;
    };
    // The opaque half may have been recorded into another buffer: nothing of
    // its state is taken for bound here.
    rebindM2State();

    // Another renderer's blended models that stand behind the next doodad (or
    // behind every one that is left) are drawn first. They bind a state of
    // their own, so everything this loop keeps bound is bound again after.
    // The particles go among the doodads too (see drawM2ParticleRuns).
    if (!skyMode_) prepareM2Particles();
    const auto drawInterleavedBehind = [&](float distSq, uint32_t ownerIndex) {
        bool drew = false;
        if (interleave && interleave->peek && interleave->draw) {
            float next = 0.0f;
            while (interleave->peek(next) && next >= distSq) {
                interleave->draw();
                drew = true;
            }
        }
        if (!skyMode_ && drawM2ParticleRuns(cmd, perFrameSet, distSq, ownerIndex)) drew = true;
        if (!drew) return;
        rebindM2State();
    };

    for (const auto& entry : transparentVisible_) {
        drawInterleavedBehind(entry.distSq, UINT32_MAX);
        if (entry.index >= instances.size()) continue;
        auto& instance = instances[entry.index];

        // Model boundary: read cachedModel off the instance - was doing a
        // per-boundary models.find() even though every instance already has
        // the pointer cached at addInstance time.
        if (entry.modelId != currentModelId) {
            currentModelId = entry.modelId;
            currentModelValid = false;
            currentModel = instance.cachedModel;
            if (!currentModel) continue;
            if (!currentModel->hasTransparentBatches && !currentModel->isSpellEffect) continue;
            if (!currentModel->vertexBuffer || !currentModel->indexBuffer) continue;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        // The distance fade, as the client takes it (0x00791cb0).
        float fadeAlpha = instanceDistanceFade(instance, entry.distSq);
        float instanceFadeAlpha = fadeAlpha * instance.fade;
        const bool instanceFaded = instance.fade < 0.999f;

        bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
        bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
        if (needsBones && instance.megaBoneOffset == 0) continue;

        // The model's blended batches on priority plane 0, then its particles,
        // then the batches above 0: the demon crystal's smoke goes over its
        // shell (plane 0) and under the eyes on its face (plane 8).
        for (int plane = 0; plane < 2; ++plane) {
        if (plane == 1 && !skyMode_ &&
            drawM2ParticleRuns(cmd, perFrameSet, entry.distSq, static_cast<uint32_t>(entry.index))) {
            rebindM2State();
            currentModelId = entry.modelId;
            currentModel = instance.cachedModel;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        for (const auto& batch : model.batches) {
            if ((batch.priorityPlane > 0) != (plane == 1)) continue;
            if (batch.indexCount == 0) continue;
            if (batch.batchOpacity < 0.01f) continue;
            if (!skyBatchAllowed(skyMode_, static_cast<std::size_t>(&batch - model.batches.data()))) continue;

            // Pass 2 gate: only transparent/additive batches - or, for a faded
            // instance, every batch, since the opaque pass left it out.
            {
                const bool rawTransparent = (batch.blendMode >= 2) || instanceFaded;
                if (!rawTransparent) continue;
            }

            // WOWEE_SKY_M2_SKIP_BLEND=<n> drops the sky's layers of one blend
            // mode. Its thirty-four batches are six opaque, ten alpha-blended
            // (2) and eighteen additive (4), and the two behave differently in
            // a way that matters: additive is order-independent and alpha
            // blending is not. Dropping ten or dropping eighteen says which
            // half the flicker lives in, the same way NO_SKY_M2 and
            // OPAQUE_ONLY narrowed it to the blended layers at all.
            if (skyMode_) {
                static const int skipBlend = [] {
                    const char* set = std::getenv("WOWEE_SKY_M2_SKIP_BLEND");
                    return set ? std::atoi(set) : -1;
                }();
                if (skipBlend >= 0 && batch.blendMode == skipBlend) continue;
            }

            // Compute UV offset for this instance + batch
            //
            // WOWEE_SKY_M2_NO_TEXANIM=1 freezes the sky's scrolling. Nine of
            // its eighteen additive layers carry a texture animation, and
            // those offsets are the one thing about those layers that changes
            // between frames at all - so if the flicker survives freezing them
            // it is not in what the layers sample, it is in the geometry they
            // are drawn on.
            static const bool skyNoTexAnim =
                std::getenv("WOWEE_SKY_M2_NO_TEXANIM") != nullptr;
            glm::vec2 uvOffset(0.0f);
            glm::vec4 uvLinear(1.0f, 0.0f, 0.0f, 1.0f);
            glm::vec2 uvOffset2(0.0f);
            glm::vec4 uvLinear2(1.0f, 0.0f, 0.0f, 1.0f);
            if (!(skyMode_ && skyNoTexAnim)) {
                if (const auto* tt = batchStageTransform(model, batch, 0)) {
                    setUvTransform(uvLinear, uvOffset,
                                   m2_track::textureTransformMatrix(*tt,
                                                   instance.currentSequenceIndex,
                                                   instance.animTime, instance.globalSequenceTime,
                                                   model.globalSequenceDurations));
                }
                if (const auto* tt2 = batchStageTransform(model, batch, 1)) {
                    setUvTransform(uvLinear2, uvOffset2,
                                   m2_track::textureTransformMatrix(*tt2,
                                                   instance.currentSequenceIndex,
                                                   instance.animTime, instance.globalSequenceTime,
                                                   model.globalSequenceDurations));
                }
            }

            // Write single instance entry to SSBO
            if (instanceDataCount_ >= MAX_INSTANCE_DATA) continue;
            uint32_t drawOffset = instanceDataCount_;
            auto& e = instSSBO[instanceDataCount_];
            e.model = instance.modelMatrix;
            e.uvOffset = uvOffset;
            e.uvLinear = uvLinear;
            e.uvOffset2 = glm::vec4(uvOffset2, 0.0f, 0.0f);
            e.uvLinear2 = uvLinear2;
            e.colorMul = batchColorMul(model, batch, instance.currentSequenceIndex,
                                       instance.animTime, instance.globalSequenceTime);
            e.fadeAlpha = instanceFadeAlpha;
            e.useBones = (needsBones && !kM2NoSkinning) ? 1 : 0;
            e.boneBase = needsBones ? static_cast<int32_t>(instance.megaBoneOffset) : 0;
            e.boneCount = static_cast<int32_t>(instance.boneMatrices.size());
            e.highlight = instance.highlight;
            writeInstanceLight(e, instance);
            writeInstanceVertexLights(frameIndex, instanceDataCount_, instance);
            instanceDataCount_++;

            // Pipeline selection: the batch's own blend mode, spell or not.
            VkPipeline desiredPipeline = blendPipelineFor(batch.blendMode);
            // An opaque layer of a faded instance: blended by its fade.
            if (instanceFaded && batch.blendMode <= 1) desiredPipeline = alphaPipeline_;
            // Cull, depth test and depth write from the material's flags, as
            // the client sets them for every blend mode.
            desiredPipeline = pipelineVariant(desiredPipeline, batch.materialFlags, false);
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }

            if (batch.materialUBOMapped) {
                auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                mat->blendMode = batch.blendMode;
            }

            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }

            // Push constants + single-instance draw
            M2PushConstants pc;
            pc.texCoordSet = batch.coordSources;
            fillModelPush(pc, skyMode_);
            pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            if (skyMode_) ++skyDiagDrawsTransparent_;
            lastDrawCallCount++;
        }
        }
    }

    // How many of the sky's layers were actually drawn this frame.
    //
    // The instance-level report says DRAWN every frame, which is a different
    // question: it counts instances that survived culling, not batches that
    // reached a draw call. Every per-batch gate between the two is meant to be
    // constant here - the material flags are static, the LOD is chosen by a
    // distance that does not change for a dome centred on the camera, and the
    // batch opacity is baked at load - so this number should never move. If it
    // moves while the camera turns, the flicker is layers appearing and
    // disappearing and one of those gates is not as constant as it reads.
    if (skyMode_ && (skyDiagDrawsOpaque_ != skyDiagLastOpaque_ ||
                     skyDiagDrawsTransparent_ != skyDiagLastTransparent_)) {
        LOG_INFO("skyM2 draws: opaque=", skyDiagDrawsOpaque_,
                 " transparent=", skyDiagDrawsTransparent_,
                 " (was ", skyDiagLastOpaque_, "/", skyDiagLastTransparent_, ")");
        skyDiagLastOpaque_ = skyDiagDrawsOpaque_;
        skyDiagLastTransparent_ = skyDiagDrawsTransparent_;
    }

}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // The set the shadow pass binds, built the same way for all four renderers.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowParamsUBO),
                               whiteTexture_->getImageView(),
                              whiteTexture_->getSampler(), "M2Renderer", shadowParams_)) {
        return false;
    }

    // Per-frame pools for foliage shadow texture sets (one per frame-in-flight, reset each frame)
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{};
        texPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Create shadow pipeline layout: set 1 = shadowParams_.layout, push constants = 128 bytes
    VkPushConstantRange pc{};
    // The fragment stage reads the alpha-test flags out of the same block.
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParams_.layout}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow fragment shader");
        return false;
    }

    // M2 vertex layout: 18 floats = 72 bytes stride
    // loc0=pos(off0), loc1=normal(off12), loc2=texCoord0(off24), loc5=texCoord1(off32),
    // loc3=boneWeights(off40), loc4=boneIndices(off56)
    // Shadow shader locations: 0=aPos, 1=aTexCoord, 2=aBoneWeights, 3=aBoneIndicesF
    // useBones=0 so locations 2,3 are never used
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = 18 * sizeof(float);
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},                     // aPos       -> position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 6 * sizeof(float)},     // aTexCoord  -> texCoord0
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 10 * sizeof(float)},    // aBoneWeights
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 14 * sizeof(float)},    // aBoneIndicesF
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    if (shadowPipeline_ &&
        !initializeInstancedShadow(shadowRenderPass,
                                   fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
                                   vertBind, vertAttrs)) {
        LOG_WARNING("M2Renderer: instanced shadow path unavailable; casters draw one "
                    "instance at a time");
    }

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("M2Renderer shadow pipeline initialized");
    return true;
}

bool M2Renderer::initializeInstancedShadow(
        VkRenderPass shadowRenderPass, const VkPipelineShaderStageCreateInfo& fragStage,
        const VkVertexInputBindingDescription& vertBind,
        const std::vector<VkVertexInputAttributeDescription>& vertAttrs) {
    VkDevice device = vkCtx_->getDevice();
    if (!instanceSetLayout_) return false;

    // The plain shadow layout plus the instance buffer at set 1. The push
    // range is the same, which is what keeps set 0 shared between the two.
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);
    shadowInstancedLayout_ =
        createPipelineLayout(device, {shadowParams_.layout, instanceSetLayout_}, {pc});
    if (!shadowInstancedLayout_) return false;

    VkShaderModule vertShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/m2_shadow.vert.spv")) {
        LOG_WARNING("M2Renderer: could not load m2_shadow.vert.spv");
        return false;
    }
    shadowInstancedPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), fragStage,
        vertBind, vertAttrs, shadowInstancedLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());
    // Optional: without it the solid casters use the pipeline above.
    shadowInstancedDepthOnlyPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), noFragmentStage(),
        vertBind, vertAttrs, shadowInstancedLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());
    vertShader.destroy();
    if (!shadowInstancedPipeline_) return false;
    // Half-built is not built: without its pipeline renderShadow never looks
    // at the buffers, and what was made is freed in shutdown as usual.
    auto fail = [&]() {
        vkDestroyPipeline(device, shadowInstancedPipeline_, nullptr);
        shadowInstancedPipeline_ = VK_NULL_HANDLE;
        return false;
    };

    const VkDeviceSize bufSize = kMaxShadowInstances * sizeof(glm::mat4);
    // Three buffers a set: the layout is the main pass's set 3, whose ground
    // cover bindings this shader does not read.
    VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 6};
    VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = 2;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolCi, nullptr, &shadowInstanceDescPool_) != VK_SUCCESS) {
        shadowInstanceDescPool_ = VK_NULL_HANDLE;
        return fail();
    }
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bufSize;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocInfo{};
        if (vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &shadowInstanceBuffer_[i],
                            &shadowInstanceAlloc_[i], &allocInfo) != VK_SUCCESS) {
            return fail();
        }
        shadowInstanceMapped_[i] = allocInfo.pMappedData;

        VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAi.descriptorPool = shadowInstanceDescPool_;
        setAi.descriptorSetCount = 1;
        setAi.pSetLayouts = &instanceSetLayout_;
        if (vkAllocateDescriptorSets(device, &setAi, &shadowInstanceSet_[i]) != VK_SUCCESS) {
            shadowInstanceSet_[i] = VK_NULL_HANDLE;
            return fail();
        }
        VkDescriptorBufferInfo bufInfo{.buffer = shadowInstanceBuffer_[i], .offset = 0, .range = bufSize};
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = shadowInstanceSet_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    LOG_INFO("M2Renderer instanced shadow pipeline initialized");
    return true;
}

void M2Renderer::beginShadowFrame(uint32_t frameIndex) {
    // A shadow that flickers is a caster that was drawn last frame and is not
    // drawn this one. The cull is in light space and the light turns with the
    // hour, so a tree on the boundary can cross it while the player stands
    // still - and from the ground that reads as the shadow blinking. Said at
    // most once a second, and only when the count actually swings, so an
    // ordinary walk through a forest stays quiet. Counted over the whole
    // frame, every cascade together, and compared here, where the frame
    // before is finished: compared per call it would be one cascade's count
    // against another's.
    {
        const uint32_t foliageNow = shadowCastersThisFrame_[1];
        const uint32_t foliageWas = lastFoliageCasters_;
        lastFoliageCasters_ = foliageNow;
        const uint32_t larger = std::max(foliageNow, foliageWas);
        const uint32_t delta = larger - std::min(foliageNow, foliageWas);
        if (foliageWas != 0 && larger >= 8 && delta * 10 > larger) {
            static std::chrono::steady_clock::time_point lastLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                LOG_WARNING("Shadow casters swung ", foliageWas, " -> ", foliageNow,
                            " foliage instances in one frame (", shadowCastersThisFrame_[0],
                            " solid). A shadow that blinks with the player standing "
                            "still is one of these crossing the light-space cull.");
            }
        }
    }
    shadowCastersThisFrame_[0] = shadowCastersThisFrame_[1] = 0;
    shadowCandidatesValid_ = false;
    shadowInstancesUsed_ = 0;

    // This frame slot's fence was waited on in beginFrame, so the texture sets
    // its last shadow pass used are finished with. Not per cascade: the sets a
    // cascade binds have to outlive the cascades recorded after it.
    if (frameIndex < kShadowTexPoolFrames && shadowTexPool_[frameIndex]) {
        vkResetDescriptorPool(vkCtx_->getDevice(), shadowTexPool_[frameIndex], 0);
    }
    // Cache: texture imageView -> allocated descriptor set (avoids duplicates
    // within the frame, across its cascades too). The pool reset above has
    // invalidated what it held.
    shadowTexSetCache_.clear();
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                              const glm::vec3& /*shadowCenter*/, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || models.empty()) return;

    // The pool and the cache were reset in beginShadowFrame; this cascade adds
    // to them.
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    VkDescriptorPool curShadowTexPool = frameIdx < kShadowTexPoolFrames ? shadowTexPool_[frameIdx]
                                                                        : VK_NULL_HANDLE;
    auto& texSetCache = shadowTexSetCache_;

    auto getTexDescSet = [&](VkTexture* tex) -> VkDescriptorSet {
        VkImageView iv = tex->getImageView();
        // Not uploaded yet: no set at all, and the caller skips the batch. A
        // set naming the null view is a read of nothing on the GPU
        // (VUID-vkCmdDrawIndexed-None-08114, first seen when the .mdx ground
        // cover began to load), and the white set instead would cast the
        // solid quad below.
        if (iv == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        auto cacheIt = texSetCache.find(iv);
        if (cacheIt != texSetCache.end()) return cacheIt->second;

        if (curShadowTexPool == VK_NULL_HANDLE) return shadowParams_.set;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = curShadowTexPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shadowParams_.layout;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
            return shadowParams_.set; // fallback to white texture
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = iv;
        imgInfo.sampler = tex->getSampler();
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = shadowParams_.ubo;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(ShadowParamsUBO);
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        texSetCache[iv] = set;
        return set;
    };

    // How many casters each pass drew, so a shadow that comes and goes can say
    // whether its caster was culled or its texture was missing.
    uint32_t castersDrawn[2] = {0, 0};

    // Cull once, into the two passes, grouped by model.
    //
    // This walked every instance in the world twice - once per pass - doing a
    // light-space transform for each, then drew them in whatever order they
    // sat in: the vertex and index buffers were rebound whenever consecutive
    // instances came from different models, which unsorted is most of them,
    // and a batch's texture was bound once per instance rather than once per
    // batch. With tens of thousands of instances resident that is the cost of
    // this pass, and none of it is drawing.
    {
    // Filed with whichever thread records the pass - the shadow worker in the
    // world, the main thread without one - so the profile shows it under the
    // pass that contains it.
    const core::ProfileScope cullScope("m2 shadow: cull + sort",
        core::FrameProfiler::isMainThread() ? core::ProfileKind::Cpu : core::ProfileKind::Worker);
    shadowCasters_[0].clear();
    shadowCasters_[1].clear();
    // The test a caster passes, the same for the full walk and the filter.
    const auto castsHere = [&](const ShadowCullEntry& e) {
        const glm::vec4 clip = lightSpaceMatrix * glm::vec4(e.position, 1.0f);
        const float margin = e.radius / shadowRadius * 1.5f;
        if (std::abs(clip.x) > 1.0f + margin || std::abs(clip.y) > 1.0f + margin) return false;
        return !(clip.z < -margin || clip.z > 1.0f + margin);
    };
    if (shadowCandidatesValid_) {
        // A later, smaller cascade: the first one's list holds every caster
        // it can have, already sorted by model, and filtering keeps the order.
        // Walking all instances again for each cascade was most of what the
        // shadow pass cost the main thread - some 6 of its 8 ms with three.
        for (int b = 0; b < 2; ++b) {
            for (const auto& entry : shadowCandidates_[b]) {
                if (entry.second >= shadowCull_.size()) continue;
                if (castsHere(shadowCull_[entry.second])) shadowCasters_[b].push_back(entry);
            }
            // The ground clutter, in a cascade small enough to show it: few
            // enough here that sorting them in afresh costs nothing.
            if (shadowRadius <= kGroundCoverShadowRadius && !shadowGroundCandidates_[b].empty()) {
                for (const auto& entry : shadowGroundCandidates_[b]) {
                    if (entry.second >= shadowCull_.size()) continue;
                    if (castsHere(shadowCull_[entry.second])) shadowCasters_[b].push_back(entry);
                }
                std::sort(shadowCasters_[b].begin(), shadowCasters_[b].end());
            }
        }
    } else {
        // The packed copy (see shadowCull_), brought up to date first.
        if (shadowCullDirty_ || shadowCull_.size() != instances.size()) rebuildShadowCull();
        shadowGroundCandidates_[0].clear();
        shadowGroundCandidates_[1].clear();
        const bool groundCasts = shadowRadius <= kGroundCoverShadowRadius;
        for (const auto& e : shadowCull_) {
            if (!e.casts || !castsHere(e)) continue;
            // Ground clutter only where it is close enough to see its shadow
            // (kGroundCoverShadowRadius); kept for the cascades after this.
            // In the far cascade it was 33,000 of 35,000 casters, sorted and
            // drawn into a map where each tuft is under a texel.
            if (e.groundCover && !groundCasts) {
                shadowGroundCandidates_[e.foliage ? 1 : 0].emplace_back(e.modelId, e.index);
                continue;
            }
            shadowCasters_[e.foliage ? 1 : 0].emplace_back(e.modelId, e.index);
        }
        for (auto& bucket : shadowCasters_) {
            std::sort(bucket.begin(), bucket.end());
        }
        shadowCandidates_[0] = shadowCasters_[0];
        shadowCandidates_[1] = shadowCasters_[1];
        shadowCandidatesValid_ = true;
    }
    }

    // Instanced where it can be: every copy of a model is one draw per batch,
    // each reading its model matrix out of this frame's shadow instance buffer
    // (m2_shadow.vert). A forest was a draw call per tree per batch. Models
    // that no longer fit in the buffer, or every model when the instanced
    // pipeline could not be built, draw one instance at a time as before.
    //
    // The buffer is the frame's, shared by its cascades: this one writes after
    // what the cascades before it wrote, which their draws still have to read.
    auto* shadowModels = (shadowInstancedPipeline_ && frameIdx < 2 &&
                          shadowInstanceSet_[frameIdx])
        ? static_cast<glm::mat4*>(shadowInstanceMapped_[frameIdx])
        : nullptr;
    const uint32_t firstInstanceThisCall = shadowInstancesUsed_;
    uint32_t shadowInstanceCount = shadowInstancesUsed_;

    // Helper lambda to draw one of the two passes: solid casters, or foliage
    // whose leaf cards are cut out of their texture by the alpha test.
    auto drawPass = [&](bool foliagePass) {
        // What this pass is, carried with each draw rather than written into a
        // buffer both passes share. The uniform buffer this used to be is read
        // when a draw executes, not when it is recorded, so writing it again
        // for the foliage pass decided what the solid pass's draws saw as
        // well - and one mapped copy across frames in flight let a CPU write
        // land inside the previous frame's reads. A foliage batch that lost
        // its alpha test that way casts the whole leaf quad, which is the
        // canopy's outline in solid black instead of its cutout.
        const glm::ivec4 passFlags{foliagePass ? 1 : 0, foliagePass ? 1 : 0, 0, 0};

        // Set 0 is laid out alike in both pipeline layouts, with the same push
        // range, so what is bound there survives switching between the two.
        // It is always bound through the layout of the pipeline in use: bound
        // through the plain one, which has no set 1, it would disturb the
        // instance buffer the instanced pipeline reads there.
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkPipelineLayout boundLayout = shadowPipelineLayout_;
        auto usePipeline = [&](bool instanced) {
            VkPipeline want = instanced ? shadowInstancedPipeline_ : shadowPipeline_;
            if (instanced && !foliagePass && shadowInstancedDepthOnlyPipeline_) {
                want = shadowInstancedDepthOnlyPipeline_;
            }
            if (want == boundPipeline) return;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            boundLayout = instanced ? shadowInstancedLayout_ : shadowPipelineLayout_;
            if (instanced) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    shadowInstancedLayout_, 1, 1, &shadowInstanceSet_[frameIdx], 0, nullptr);
            }
            boundPipeline = want;
        };
        usePipeline(shadowModels != nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boundLayout,
            0, 1, &shadowParams_.set, 0, nullptr);

        const auto& casters = shadowCasters_[foliagePass ? 1 : 0];
        for (std::size_t g = 0; g < casters.size();) {
            const uint32_t modelId = casters[g].first;
            std::size_t groupEnd = g;
            while (groupEnd < casters.size() && casters[groupEnd].first == modelId) ++groupEnd;

            const auto& firstInstance = instances[casters[g].second];
            if (!firstInstance.cachedModel) { g = groupEnd; continue; }
            const M2ModelGPU& model = *firstInstance.cachedModel;

            // Once per model, not once per instance.
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // This model's instances into the buffer once, for all its batches.
            const auto groupSize = static_cast<uint32_t>(groupEnd - g);
            const bool instanced = shadowModels &&
                                   shadowInstanceCount + groupSize <= kMaxShadowInstances;
            const uint32_t firstShadowSlot = shadowInstanceCount;
            if (instanced) {
                for (std::size_t k = g; k < groupEnd; ++k) {
                    shadowModels[shadowInstanceCount++] = instances[casters[k].second].modelMatrix;
                }
            }
            usePipeline(instanced);

            for (const auto& batch : model.batches) {
                // A blended or additive layer - smoke, a glow, a spell's
                // card - covers nothing, so it casts nothing; the ray-traced
                // scene leaves the same layers out.
                if (batch.blendMode >= 2) continue;
                // A leaf card with no texture is a shadow with no caster.
                //
                // The else below binds the white set, whose alpha test passes
                // everywhere, so a batch that arrived without its texture casts
                // the shadow of a solid quad - while the main pass skips that
                // same batch for want of a material set. The result is a black
                // rectangle on the ground under a tree that looks fine, coming
                // and going as the texture cache lets the sheet back in.
                if (foliagePass && !batch.texture) {
                    if (!warnedShadowNoTexture_) {
                        warnedShadowNoTexture_ = true;
                        LOG_WARNING("Shadow pass: a foliage batch of ", model.name,
                                    " has no texture; skipping it rather than casting a "
                                    "solid quad. The texture cache is the usual reason.");
                    }
                    continue;
                }
                // Once per batch, where it used to be once per batch per
                // instance: the texture is the batch's, and every instance of
                // this model shares it.
                if (foliagePass && batch.hasAlpha && batch.texture) {
                    VkDescriptorSet texSet = getTexDescSet(batch.texture);
                    if (texSet == VK_NULL_HANDLE) continue;  // its texture is still uploading
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        boundLayout, 0, 1, &texSet, 0, nullptr);
                } else if (foliagePass) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        boundLayout, 0, 1, &shadowParams_.set, 0, nullptr);
                }

                if (instanced) {
                    // The light's matrix alone: the model is the instance's.
                    // flags.w is where this model's instances start.
                    ShadowPush push{
                        .lightSpaceModel = lightSpaceMatrix,
                        .flags = glm::ivec4(passFlags.x, passFlags.y, passFlags.z,
                                            static_cast<int>(firstShadowSlot))};
                    vkCmdPushConstants(cmd, shadowInstancedLayout_,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(ShadowPush), &push);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    continue;
                }
                for (std::size_t k = g; k < groupEnd; ++k) {
                    const auto& instance = instances[casters[k].second];
                    ShadowPush push{
                        .lightSpaceModel = lightSpaceMatrix * instance.modelMatrix,
                        .flags = passFlags};
                    vkCmdPushConstants(cmd, shadowPipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,  // as the layout declares
                                       0, sizeof(ShadowPush), &push);
                    vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
                }
            }
            castersDrawn[foliagePass ? 1 : 0] += static_cast<uint32_t>(groupEnd - g);
            g = groupEnd;
        }
    };

    // Pass 1: solid casters
    drawPass(false);
    // Pass 2: foliage, with per-batch alpha-tested textures so a leaf card
    // casts its cutout rather than its quad
    drawPass(true);

    if (shadowInstanceCount > firstInstanceThisCall) {
        // What this cascade wrote. A no-op on coherent memory, which
        // CPU_TO_GPU nearly always is.
        vmaFlushAllocation(vkCtx_->getAllocator(), shadowInstanceAlloc_[frameIdx],
                           firstInstanceThisCall * sizeof(glm::mat4),
                           (shadowInstanceCount - firstInstanceThisCall) * sizeof(glm::mat4));
    }
    shadowInstancesUsed_ = shadowInstanceCount;
    shadowCastersThisFrame_[0] += castersDrawn[0];
    shadowCastersThisFrame_[1] += castersDrawn[1];
}

} // namespace rendering
} // namespace wowee

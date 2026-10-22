#include <cstdlib>
#include "rendering/m2_renderer.hpp"
#include "core/frame_profiler.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/animation/animation_data.hpp"
#include "rendering/blob_shadow.hpp"
#include "rendering/movement_limits.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <set>
#include <mutex>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace wowee {
namespace rendering {

// Thread-local scratch buffers for collision queries (moved from header to
// avoid inline thread_local TLS init linker errors on Windows ARM64 / LLD).
namespace m2_internal {
thread_local std::vector<size_t> tl_m2_candidateScratch;
thread_local std::unordered_set<uint32_t> tl_m2_candidateIdScratch;
thread_local std::vector<uint32_t> tl_m2_collisionTriScratch;
} // namespace m2_internal

namespace {
/// An index list after a swap-remove of `idx`: it goes, and `last`, moved
/// into its place, is renamed to it.
void followSwapRemove(std::vector<size_t>& v, size_t idx, size_t last) {
    for (size_t k = 0; k < v.size();) {
        if (v[k] == idx) {
            v[k] = v.back();
            v.pop_back();
            continue;
        }
        ++k;
    }
    if (idx < last) {
        for (size_t& x : v) {
            if (x == last) x = idx;
        }
    }
}
}  // namespace

void M2Renderer::followShadowCullSwap(size_t idx, size_t last) {
    if (shadowCullDirty_ || shadowCull_.size() != last + 1) {
        shadowCullDirty_ = true;
        return;
    }
    if (idx < last) {
        shadowCull_[idx] = shadowCull_[last];
        shadowCull_[idx].index = static_cast<uint32_t>(idx);
    }
    shadowCull_.pop_back();
}

void M2Renderer::rebuildShadowCull() {
    shadowCull_.resize(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) refreshShadowCullEntry(i);
    shadowCullDirty_ = false;
}

void M2Renderer::refreshShadowCullEntry(size_t index) {
    if (index >= shadowCull_.size() || index >= instances.size()) return;
    const M2Instance& inst = instances[index];
    ShadowCullEntry& e = shadowCull_[index];
    const bool casts = inst.cachedIsValid && !inst.cachedIsInvisibleTrap && inst.cachedModel;
    e.position = inst.position;
    e.radius = casts ? inst.cachedModel->boundRadius * inst.scale : 0.0f;
    e.modelId = inst.modelId;
    e.index = static_cast<uint32_t>(index);
    e.foliage = (casts && inst.cachedModel->shadowWindFoliage) ? 1u : 0u;
    e.groundCover = (inst.cachedIsGroundDetail || inst.detailDoodad) ? 1u : 0u;
    e.casts = casts ? 1u : 0u;
}

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return;
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // The box the instance is filed under, which is what it has to be taken
    // out of once it has moved.
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;

    inst.position = position;
    inst.updateModelMatrix();
    // Use cachedModel instead of a fresh models.find() - the pointer was set
    // at addInstance and stays valid as long as the instance exists.
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getCollisionQueryBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    refreshShadowCullEntry(idxIt->second);
}

void M2Renderer::setInstanceFade(uint32_t instanceId, float alpha) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].fade = std::clamp(alpha, 0.0f, 1.0f);
}

void M2Renderer::setInstanceDetailShade(uint32_t instanceId, const glm::vec4& color,
                                        const glm::vec3& normal) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.detailDoodad = true;
    inst.detailColor = color;
    inst.detailNormal = normal;
}

void M2Renderer::setInstanceInteriorLight(uint32_t instanceId, const glm::vec3& ambient,
                                          const glm::vec3& direct) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.interiorLit = true;
    inst.interiorAmbient = ambient;
    inst.interiorDirect = direct;
}

void M2Renderer::refreshGameObjectLights(const FloorQuery& floorAt, const InteriorPassQuery& inInteriorPass,
                                         uint64_t passGeneration, const glm::vec3& zoneAmbient,
                                         const glm::vec3& zoneDirect, float seconds, uint64_t floorGeneration) {
    namespace wl = pipeline::wmo_doodad_light;
    const auto toBytes = [](const glm::vec3& c) {
        return glm::ivec3(glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    // A doodad stays where it is: asked again only when the pass changes,
    // a quarter of the list a frame. Walked whole, at once, on every change -
    // and a new instance counted as one, which in a city is every frame - it
    // was most of this function's cost. Instances added since are met as the
    // walk reaches the end of the list.
    if (passGeneration != lightsPassGeneration_) {
        lightsPassGeneration_ = passGeneration;
        lightsCursor_ = 0;
    }
    if (lightsCursor_ < instances.size()) {
        WOWEE_PROFILE_SCOPE("lighting: doodad interior pass", Cpu);
        const size_t end = std::min(instances.size(), lightsCursor_ + instances.size() / 4 + 1);
        for (size_t i = lightsCursor_; i < end; ++i) {
            auto& inst = instances[i];
            if (inst.isGameObject || inst.interiorPassGeneration == passGeneration) continue;
            inst.interiorPassGeneration = passGeneration;
            inst.interiorPass = inInteriorPass && inInteriorPass(inst.position);
        }
        lightsCursor_ = end;
    }
    // A game object that moved asks for its floor again - three a frame, the
    // rest keeping theirs a frame or two longer. Each is a ray through the
    // world's geometry, and a city has dozens of objects that shift a little
    // every frame.
    int floorBudget = 3;
    // And not more than four times a second for any one of them. Something
    // that never stops - a ship at its dock, an elevator, a turning wheel -
    // took the three every frame: two and a half milliseconds of the main
    // thread in Orgrimmar, for a light that eases over a second anyway.
    constexpr double kMovedFloorInterval = 0.25;
    lightsClock_ += std::max(0.0f, seconds);
    // Once the buildings have changed, every object asks again, a few a
    // frame: one placed before the building it stands in was loaded found no
    // floor and kept the zone's light (and the probes' ambient) indoors - the
    // inn's chairs and chests, which never move to ask again.
    int changedBudget = 16;
    size_t keep = 0;
    for (size_t k = 0; k < gameObjectInstanceIds_.size(); ++k) {
        const uint32_t id = gameObjectInstanceIds_[k];
        auto idxIt = instanceIndexById.find(id);
        if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size() ||
            !instances[idxIt->second].isGameObject) {
            continue;  // gone, or no longer one: dropped from the list
        }
        gameObjectInstanceIds_[keep++] = id;
        auto& inst = instances[idxIt->second];
        inst.interiorPassGeneration = passGeneration;
        inst.interiorPass = inInteriorPass && inInteriorPass(inst.position);
        const glm::vec3 d = inst.position - inst.floorQueryPos;
        const bool moved = glm::dot(d, d) >= 0.25f * 0.25f;
        // A mover asked a moment ago keeps that floor until its next turn.
        const bool movedDue = moved && lightsClock_ - inst.floorQueryAt >= kMovedFloorInterval;
        const bool buildingsChanged = inst.floorGeneration != floorGeneration;
        if (!inst.floorQueried || (movedDue && floorBudget > 0) || (!moved && buildingsChanged && changedBudget > 0)) {
            if (inst.floorQueried) --(moved ? floorBudget : changedBudget);
            inst.floorQueried = true;
            inst.floorQueryPos = inst.position;
            inst.floorQueryAt = lightsClock_;
            inst.floorGeneration = floorGeneration;
            WOWEE_PROFILE_SCOPE("lighting: game object floor", Cpu);
            inst.floor = floorAt(inst.position, wl::gameObjectFloorStart(inst.position.z, inst.worldBoundsMax.z));
        }
        const auto& floorLight = inst.floor.light;
        wl::ObjectLight light{zoneAmbient, zoneDirect, 0.0f};
        if (floorLight) light = wl::floorLightInZone(*floorLight, zoneAmbient, zoneDirect);
        // From the zone's ambient into the floor's (0x00781a10, 0x007a1e90),
        // the direct light toward half in the baked shadow (0x007a1bc0).
        if (!inst.worldObjectLit) {
            inst.worldObjectLit = true;
            inst.easedAmbient = toBytes(zoneAmbient);
            inst.directScale = 1.0f;
        }
        inst.easedAmbient = wl::easeAmbient(inst.easedAmbient, toBytes(light.ambient), seconds);
        inst.directScale = wl::easeDirectScale(
            inst.directScale,
            wl::directScaleTarget(floorLight.has_value(), floorLight ? floorLight->towardOutside : 0,
                                  inst.floor.inBakedShadow),
            seconds);
        inst.interiorLit = floorLight.has_value();
        inst.interiorAmbient = glm::vec3(inst.easedAmbient) / 255.0f;
        inst.interiorDirect = light.direct * inst.directScale;
        inst.interiorTowardSun = light.towardSun;
    }
    gameObjectInstanceIds_.resize(keep);
}

void M2Renderer::setInstanceHighlight(uint32_t instanceId, float amount) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    instances[idxIt->second].highlight = amount;
}

void M2Renderer::clearInstanceHighlights() {
    // A walk of every instance rather than a record of the lit one, because a
    // press can end with the instance gone - the object despawned, the tile
    // unloaded - and a remembered id would then light whatever took its slot.
    for (auto& inst : instances) inst.highlight = 0.0f;
}

void M2Renderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animSpeed = frozen ? 0.0f : 1.0f;
    inst.holdAtEnd = false;
    if (frozen) {
        inst.animTime = 0.0f;  // Reset to bind pose
    }
}

void M2Renderer::setInstanceAnimationFraction(uint32_t instanceId, float fraction) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animSpeed = 0.0f;
    inst.holdAtEnd = false;
    if (inst.animDuration > 0.0f) {
        const float f = std::clamp(fraction, 0.0f, 1.0f);
        inst.animTime = std::min(f * inst.animDuration, inst.animDuration - 1.0f);
    }
}

void M2Renderer::restartInstanceAnimation(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animTime = 0.0f;
    inst.eventsFromStart = true;
    if (inst.cachedModel) computeBoneMatrices(*inst.cachedModel, inst, &cachedCamPos_);
}

void M2Renderer::setInstanceEventOwner(uint32_t instanceId, m2_event::Owner owner, uint32_t ownerArg) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.eventOwner = owner;
    inst.eventOwnerArg = ownerArg;
}

std::vector<M2Renderer::FiredEvent> M2Renderer::takeFiredEvents() {
    std::vector<FiredEvent> out;
    out.swap(firedEvents_);
    return out;
}

std::optional<glm::vec3> M2Renderer::getInstancePosition(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    return glm::vec3(instances[idxIt->second].modelMatrix[3]);
}

std::optional<uint32_t> M2Renderer::soleSequenceId(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel || inst.cachedModel->sequences.size() != 1) return std::nullopt;
    return inst.cachedModel->sequences[0].id;
}

bool M2Renderer::getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& out) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    const M2ModelGPU* model = inst.cachedModel;
    if (!model) return false;
    const pipeline::M2Attachment* point = nullptr;
    if (attachmentId < model->attachmentLookup.size()) {
        const uint16_t index = model->attachmentLookup[attachmentId];
        if (index < model->attachments.size()) point = &model->attachments[index];
    }
    if (!point) return false;
    glm::mat4 bone(1.0f);
    if (point->bone < inst.boneMatrices.size()) bone = inst.boneMatrices[point->bone];
    out = inst.modelMatrix * bone * glm::translate(glm::mat4(1.0f), point->position);
    return true;
}

namespace {
// 0x00832ab0 -> 0x00826350: the sequence a model plays for an animation -
// the first with its id, else the first of the one along AnimationData's
// Fallbacks that stands in for it - and how that plays: forward, backwards
// from its end, or held on its first or last frame. Without AnimationData
// (the editor) only the model's own.
struct ShownSequence {
    int index = -1;
    rendering::anim::PlayMode mode = rendering::anim::PlayMode::Forward;
};
ShownSequence shownSequence(const std::vector<pipeline::M2Sequence>& seqs, uint32_t animationId) {
    namespace anim = rendering::anim;
    const auto has = [&seqs](uint32_t id) {
        return std::any_of(seqs.begin(), seqs.end(), [id](const pipeline::M2Sequence& s) { return s.id == id; });
    };
    const auto& data = anim::animationData();
    const anim::ModelAnimation shown =
        data.empty() ? anim::ModelAnimation{animationId, anim::PlayMode::Forward}
                     : anim::modelAnimation(data, animationId, has, seqs.empty() ? 0u : seqs[0].id);
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id == shown.id) return {i, shown.mode};
    }
    return {};
}
}  // namespace

void M2Renderer::setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                          bool skipToEnd) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    const ShownSequence shown = shownSequence(seqs, animationId);
    const int i = shown.index;
    if (i < 0) return;
    // A door jumped straight to its end state has no old pose to leave.
    if (!skipToEnd) blendIntoSequence(*inst.cachedModel, inst, i);
    inst.currentSequenceIndex = i;
    inst.animDuration = static_cast<float>(seqs[i].duration);
    inst.playingVariation = false;   // not a variation: it does not go back
    inst.holdAtEnd = true;
    // Its end is its first frame where it stands in reversed; one held
    // stands where it is held.
    const bool reversed = shown.mode == anim::PlayMode::Reversed;
    if (skipToEnd || shown.mode == anim::PlayMode::HoldEnd || shown.mode == anim::PlayMode::HoldStart) {
        inst.animTime = shown.mode == anim::PlayMode::HoldStart || (skipToEnd && reversed) ? 0.0f : inst.animDuration;
        inst.animSpeed = 0.0f;
        inst.eventsFromStart = false;
    } else {
        inst.animTime = reversed ? inst.animDuration : 0.0f;
        inst.animSpeed = reversed ? -1.0f : 1.0f;
        inst.eventsFromStart = true;
    }
}

void M2Renderer::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    const ShownSequence shown = shownSequence(seqs, animationId);
    const int i = shown.index;
    if (i < 0) return;
    blendIntoSequence(*inst.cachedModel, inst, i);
    inst.currentSequenceIndex = i;
    inst.animDuration = static_cast<float>(seqs[i].duration);
    // Use playingVariation=true for one-shot (returns to idle when done)
    inst.playingVariation = !loop;
    inst.holdAtEnd = false;
    switch (shown.mode) {
        case anim::PlayMode::Forward:
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
            inst.eventsFromStart = true;
            break;
        case anim::PlayMode::Reversed:
            // From its end back to its start.
            inst.animTime = inst.animDuration;
            inst.animSpeed = -1.0f;
            inst.eventsFromStart = true;
            break;
        case anim::PlayMode::HoldStart:
        case anim::PlayMode::HoldEnd:
            // Held where it stands in: it neither runs nor ends.
            inst.animTime = shown.mode == anim::PlayMode::HoldEnd ? inst.animDuration : 0.0f;
            inst.animSpeed = 0.0f;
            inst.eventsFromStart = false;
            inst.playingVariation = false;
            inst.holdAtEnd = true;
            break;
    }
}

bool M2Renderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    for (const auto& seq : inst.cachedModel->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

bool M2Renderer::sequencePlaysOnce(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    // The one setInstanceAnimation plays for it.
    const auto& seqs = inst.cachedModel->sequences;
    const int i = shownSequence(seqs, animationId).index;
    return i >= 0 && (seqs[static_cast<size_t>(i)].flags & 0x1u) != 0;
}

uint32_t M2Renderer::shownAnimationId(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return animationId;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return animationId;
    const auto& seqs = inst.cachedModel->sequences;
    const int i = shownSequence(seqs, animationId).index;
    return i >= 0 ? seqs[static_cast<size_t>(i)].id : animationId;
}

float M2Renderer::getInstanceAnimDuration(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return 0.0f;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return 0.0f;
    const auto& seqs = inst.cachedModel->sequences;
    if (seqs.empty()) return 0.0f;
    int seqIdx = inst.currentSequenceIndex;
    if (seqIdx < 0 || seqIdx >= static_cast<int>(seqs.size())) seqIdx = 0;
    return seqs[seqIdx].duration; // in milliseconds
}

std::optional<M2Renderer::AnimPlayback> M2Renderer::getInstanceAnimPlayback(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    const auto& inst = instances[idxIt->second];
    return AnimPlayback{inst.animTime, inst.animDuration, inst.animSpeed};
}

void M2Renderer::setInstanceAnimTime(uint32_t instanceId, float timeMs) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animTime = std::clamp(timeMs, 0.0f, std::max(inst.animDuration, 0.0f));
    // Started part-way: what it starts past is behind it.
    if (inst.animTime > 0.0f) inst.eventsFromStart = false;
}

bool M2Renderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    outCenter = inst.cachedCullCenter;
    outRadius = inst.cachedVisualRadius;
    return outRadius > 0.0f;
}

bool M2Renderer::instanceDrawsNothing(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    auto modelIt = models.find(instances[idxIt->second].modelId);
    if (modelIt == models.end()) return false;
    // A model of emitters alone (UndeadFireLarge.m2) has no triangle to meet.
    return modelIt->second.isInvisibleTrap || modelIt->second.indexCount == 0 ||
           modelIt->second.vertexCount == 0;
}

bool M2Renderer::getInstanceHeight(uint32_t instanceId, glm::vec3& outPosition, float& outScale,
                                   float& outModelHeight) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    auto modelIt = models.find(inst.modelId);
    if (modelIt == models.end()) return false;
    outPosition = inst.position;
    outScale = inst.scale;
    const auto& model = modelIt->second;
    outModelHeight = model.hasVertexBox ? model.vertexBoxMax.z - model.vertexBoxMin.z
                                        : model.boundMax.z - model.boundMin.z;
    return true;
}

void M2Renderer::setModelParticlesOnly(uint32_t modelId) {
    shadowCullDirty_ = true;
    auto it = models.find(modelId);
    if (it == models.end()) return;
    // Every path that would draw, shadow or collide with it already skips an
    // invisible trap, and the particle passes do not.
    it->second.isInvisibleTrap = true;
    it->second.disableAnimation = true;
}

void M2Renderer::setInstanceExternalPose(uint32_t instanceId, const glm::mat4& model,
                                         const std::vector<glm::mat4>& bones, int sequenceIndex,
                                         float animTimeMs, float globalTimeMs) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    setInstanceTransform(instanceId, model);
    auto& inst = instances[idxIt->second];
    inst.scale = glm::length(glm::vec3(model[0]));
    inst.boneMatrices = bones;
    refreshShadowCullEntry(idxIt->second);
    inst.currentSequenceIndex = sequenceIndex;
    inst.animTime = animTimeMs;
    inst.globalSequenceTime = globalTimeMs;
    if (inst.cachedModel && sequenceIndex >= 0 &&
        sequenceIndex < static_cast<int>(inst.cachedModel->sequences.size())) {
        inst.animDuration = static_cast<float>(inst.cachedModel->sequences[sequenceIndex].duration);
    }
}

void M2Renderer::setInstanceParticleColors(uint32_t instanceId, const std::array<glm::vec3, 9>& colors) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.particleColors = colors;
    inst.hasParticleColors = true;
}

std::optional<glm::mat4> M2Renderer::getInstanceModelMatrix(uint32_t instanceId) const {
    const auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return std::nullopt;
    return instances[idxIt->second].modelMatrix;
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    // Reject NaN matrix - would propagate into the model matrix uniform
    // and the spatial-grid bounds, leaving stale grid cells pointing at
    // a NaN-bounded instance.
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!std::isfinite(transform[c][r])) return;
    auto& inst = instances[idxIt->second];

    // Remove old grid cells before updating bounds
    const glm::vec3 oldBoundsMin = inst.worldBoundsMin;
    const glm::vec3 oldBoundsMax = inst.worldBoundsMax;
    const glm::vec3 oldPosition = inst.position;

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position from transform for bounds
    inst.position = glm::vec3(transform[3]);
    refreshShadowCullEntry(idxIt->second);

    // The dedup map is keyed on position, so it has to move with the instance.
    // It did not, and only rebuildSpatialIndex ever put it right - which this
    // path deliberately avoids. A ship's doodads are created at the origin and
    // moved here a frame later, so the origin key stayed pointing at them and
    // the next ship of the same class was handed the first ship's sails instead
    // of its own. Both hulls then wrote their transform to the one instance, so
    // it rendered at whichever ship updated last, and whichever hull unloaded
    // first destroyed it for the other.
    if (!inst.cachedIsGroundDetail) {
        auto keyFor = [&](const glm::vec3& p) {
            return DedupKey{.modelId = inst.modelId,
                            .qx = static_cast<int32_t>(std::round(p.x * 10.0f)),
                            .qy = static_cast<int32_t>(std::round(p.y * 10.0f)),
                            .qz = static_cast<int32_t>(std::round(p.z * 10.0f))};
        };
        const DedupKey oldKey = keyFor(oldPosition);
        const DedupKey newKey = keyFor(inst.position);
        if (!(oldKey == newKey)) {
            auto oldIt = instanceDedupMap_.find(oldKey);
            // Only drop the entry if it still names this instance.
            if (oldIt != instanceDedupMap_.end() && oldIt->second == instanceId) {
                instanceDedupMap_.erase(oldIt);
            }
            instanceDedupMap_.emplace(newKey, instanceId);
        }
    }

    // Update bounds via the cached model pointer
    if (inst.cachedModel) {
        glm::vec3 localMin, localMax;
        getCollisionQueryBounds(*inst.cachedModel, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
        inst.recomputeCachedCullFactors();
    }

    // Incrementally update spatial grid (remove old cells, add new cells)
    refileBounds(spatialGrid, oldBoundsMin, oldBoundsMax,
                 inst.worldBoundsMin, inst.worldBoundsMax, instanceId);
    // No spatialIndexDirty_ = true - handled incrementally
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= instances.size()) return;

    auto& inst = instances[idx];

    // Remove from spatial grid incrementally (same pattern as the move-update path)
    eraseBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, instanceId);

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{.modelId = inst.modelId,
                    .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        instanceDedupMap_.erase(dk);
    }

    destroyInstanceBones(inst, /*defer=*/true);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    const size_t last = instances.size() - 1;
    followShadowCullSwap(idx, last);
    instanceIndexById.erase(instanceId);
    if (idx < instances.size() - 1) {
        uint32_t movedId = instances.back().id;
        instances[idx] = std::move(instances.back());
        instances.pop_back();
        instanceIndexById[movedId] = idx;
    } else {
        instances.pop_back();
    }

    // The index lists follow the swap, as the shadow cull copy did above:
    // both used to be rebuilt by walking every instance - 120 000 in a city,
    // on each removal, and a city removes spell effects every frame.
    followSwapRemove(animatedInstanceIndices_, idx, last);
    followSwapRemove(particleOnlyInstanceIndices_, idx, last);
    followSwapRemove(particleInstanceIndices_, idx, last);
}

void M2Renderer::setInstanceIsGameObject(uint32_t instanceId, bool isGameObject) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return;
    auto& inst = instances[idxIt->second];
    if (isGameObject && !inst.isGameObject) gameObjectInstanceIds_.push_back(instanceId);
    inst.isGameObject = isGameObject;
}

void M2Renderer::setInstanceNoDistanceCull(uint32_t instanceId, bool noDistanceCull) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return;
    instances[idxIt->second].noDistanceCull = noDistanceCull;
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    for (auto& inst : instances) {
        if (inst.id == instanceId) {
            inst.skipCollision = skip;
            return;
        }
    }
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || instances.empty()) {
        return;
    }

    // A tile unload hands over a few thousand ids out of the tens of thousands
    // a crowded zone holds. Rebuilding the whole spatial grid and dedup map for
    // that cost 35-70 ms a tile on the main thread - a hitch at every tile
    // border - so only what the removed instances touched is taken out.
    std::unordered_set<uint32_t> toRemove;
    toRemove.reserve(instanceIds.size());
    // Cells are swept once each after the loop: ground clutter puts thousands
    // of ids in one cell, and erasing them one at a time is quadratic.
    std::unordered_set<GridCell, GridCellHash> touchedCells;
    std::vector<size_t> removedIndices;
    removedIndices.reserve(instanceIds.size());
    for (uint32_t id : instanceIds) {
        auto idxIt = instanceIndexById.find(id);
        if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) continue;
        if (!toRemove.insert(id).second) continue;
        const size_t idx = idxIt->second;
        removedIndices.push_back(idx);
        auto& inst = instances[idx];
        const GridCell minCell = toSpatialCell(inst.worldBoundsMin);
        const GridCell maxCell = toSpatialCell(inst.worldBoundsMax);
        for (int z = minCell.z; z <= maxCell.z; z++)
            for (int y = minCell.y; y <= maxCell.y; y++)
                for (int x = minCell.x; x <= maxCell.x; x++)
                    touchedCells.insert(GridCell{.x = x, .y = y, .z = z});
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            auto dedupIt = instanceDedupMap_.find(dk);
            if (dedupIt != instanceDedupMap_.end() && dedupIt->second == id) {
                instanceDedupMap_.erase(dedupIt);
            }
        }
        destroyInstanceBones(inst, /*defer=*/true);
        instanceIndexById.erase(idxIt);
    }
    if (toRemove.empty()) {
        return;
    }
    for (const GridCell& cell : touchedCells) {
        auto cellIt = spatialGrid.find(cell);
        if (cellIt == spatialGrid.end()) continue;
        auto& ids = cellIt->second;
        ids.erase(std::remove_if(ids.begin(), ids.end(),
                                 [&toRemove](uint32_t id) { return toRemove.count(id) != 0; }),
                  ids.end());
    }

    // Swap-removed, as removeInstance does: compacting in order slid every
    // later instance - a few hundred bytes each, tens of thousands of them -
    // down over the holes, which was most of what was left of the cost.
    std::sort(removedIndices.begin(), removedIndices.end(), std::greater<size_t>());
    // A handful - a spell's effects going - follow each swap in the index
    // lists; a tile's thousands rebuild them once below.
    const bool fewRemoved = removedIndices.size() <= 32;
    for (size_t idx : removedIndices) {
        const size_t last = instances.size() - 1;
        followShadowCullSwap(idx, last);
        if (fewRemoved) {
            followSwapRemove(animatedInstanceIndices_, idx, last);
            followSwapRemove(particleOnlyInstanceIndices_, idx, last);
            followSwapRemove(particleInstanceIndices_, idx, last);
        }
        if (idx + 1 < instances.size()) {
            instances[idx] = std::move(instances.back());
            instanceIndexById[instances[idx].id] = idx;
        }
        instances.pop_back();
    }
    if (fewRemoved) return;

    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    for (size_t i = 0; i < instances.size(); i++) {
        const auto& ri = instances[i];
        if (ri.cachedHasParticleEmitters) particleInstanceIndices_.push_back(i);
        if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
            animatedInstanceIndices_.push_back(i);
        else if (ri.cachedHasParticleEmitters)
            particleOnlyInstanceIndices_.push_back(i);
    }
}

void M2Renderer::clear() {
    if (vkCtx_) {
        vkDeviceWaitIdle(vkCtx_->getDevice());
        for (auto& [id, model] : models) {
            destroyModelGPU(model);
        }
        for (auto& inst : instances) {
            destroyInstanceBones(inst);
        }
        // Reset descriptor pools so new allocations succeed after reload.
        // destroyModelGPU/destroyInstanceBones don't free individual sets,
        // so the pools fill up across map changes without this reset. What
        // is still queued to free sets from them runs first (see
        // WMORenderer::clearAll).
        vkCtx_->flushDeferredCleanup();
        VkDevice device = vkCtx_->getDevice();
        if (materialDescPool_) {
            vkResetDescriptorPool(device, materialDescPool_, 0);
        }
        // Their sets went with the pool and their textures go below.
        stripTextures_.clear();
        clientStrips_.clear();
        clientStripSets_.clear();
        if (boneDescPool_) {
            if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
            vkResetDescriptorPool(device, boneDescPool_, 0);
            // Re-allocate the dummy bone set (invalidated by pool reset)
            dummyBoneSet_ = allocateBoneSet();
            if (dummyBoneSet_ && dummyBoneBuffer_) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = dummyBoneBuffer_;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(glm::mat4);
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = dummyBoneSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
            // Re-allocate mega bone sets (invalidated by pool reset)
            for (int i = 0; i < 2; i++) {
                megaBoneSet_[i] = allocateBoneSet();
                if (megaBoneSet_[i] && megaBoneBuffer_[i]) {
                    VkDescriptorBufferInfo mbInfo{};
                    mbInfo.buffer = megaBoneBuffer_[i];
                    mbInfo.offset = 0;
                    mbInfo.range = VkDeviceSize(MEGA_BONE_MATRIX_CAPACITY) * sizeof(glm::mat4);
                    VkWriteDescriptorSet mw{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    mw.dstSet = megaBoneSet_[i];
                    mw.dstBinding = 0;
                    mw.descriptorCount = 1;
                    mw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    mw.pBufferInfo = &mbInfo;
                    vkUpdateDescriptorSets(device, 1, &mw, 0, nullptr);
                }
            }
        }
    }
    models.clear();
    pinnedModelIds_.clear();
    instances.clear();
    shadowCullDirty_ = true;
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    for (auto& ids : cullSubmittedIds_) ids.clear();
    for (auto& ids : cullReadableIds_) ids.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    // Clear texture cache so stale textures don't block loads for the next
    // character/map.  Without this, the old session's textures fill the cache
    // budget and failedTextureRetryAt_ blocks legitimate reloads, causing an
    // infinite model-load loop on character switch.
    textureCache.clear();
    texturePropsByPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
}

void M2Renderer::clearInstances() {
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    for (auto& inst : instances) destroyInstanceBones(inst);
    instances.clear();
    shadowCullDirty_ = true;
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    for (auto& ids : cullSubmittedIds_) ids.clear();
    for (auto& ids : cullReadableIds_) ids.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
}

void M2Renderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocus.set(worldPos, radius);
}

void M2Renderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

void M2Renderer::rebuildSpatialIndex() {
    shadowCullDirty_ = true;
    spatialGrid.clear();
    instanceIndexById.clear();
    boneSeedInstanceByModel_.clear();
    instanceDedupMap_.clear();
    instanceIndexById.reserve(instances.size());
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    for (size_t i = 0; i < instances.size(); i++) {
        auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        // Re-cache model pointer (may have changed after model map modifications)
        auto mdlIt = models.find(inst.modelId);
        inst.cachedModel = (mdlIt != models.end()) ? &mdlIt->second : nullptr;

        // Rebuild dedup map (skip ground detail)
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{.modelId = inst.modelId,
                        .qx = static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        .qy = static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        .qz = static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            instanceDedupMap_[dk] = inst.id;
        }

        if (inst.cachedHasParticleEmitters) {
            particleInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
            animatedInstanceIndices_.push_back(i);
        } else if (inst.cachedHasParticleEmitters) {
            particleOnlyInstanceIndices_.push_back(i);
        }

        insertBounds(spatialGrid, inst.worldBoundsMin, inst.worldBoundsMax, inst.id);
    }
    spatialIndexDirty_ = false;
}

void M2Renderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                  std::vector<size_t>& outIndices) const {
    outIndices.clear();

    gatherIds(spatialGrid, queryMin, queryMax, tl_m2_candidateIdScratch,
              [&](uint32_t id) {
                  auto idxIt = instanceIndexById.find(id);
                  if (idxIt != instanceIndexById.end()) {
                      outIndices.push_back(idxIt->second);
                  }
              });

    // No candidates means nothing stands there. The grid is kept as instances
    // are made, moved and removed (insertBounds, refileBounds, eraseBounds);
    // spatialIndexDirty_ only marks the cached model pointers and fast-path
    // lists for refreshing, not the grid's ids. This used to hand back every
    // instance for an empty answer - the usual answer on open ground - which
    // made a floor query in an empty field a scan of the whole loaded world,
    // a quarter of a millisecond each. A weather change asks hundreds a frame
    // for the ground its drops land on: over a hundred milliseconds a frame,
    // for seconds, each time the weather changed.
}

void M2Renderer::setModelPinned(uint32_t modelId, bool pinned) {
    if (pinned) {
        pinnedModelIds_.insert(modelId);
        modelUnusedSince_.erase(modelId);
    } else {
        pinnedModelIds_.erase(modelId);
    }
}

void M2Renderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : instances) {
        usedModelIds.insert(instance.modelId);
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(60);

    // Find models with no instances that have exceeded the grace period.
    // Models that just lost their last instance get tracked but not evicted
    // immediately - this prevents thrashing when GO models are briefly
    // instance-free between despawn and respawn cycles.
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : models) {
        if (usedModelIds.find(id) != usedModelIds.end() ||
            pinnedModelIds_.find(id) != pinnedModelIds_.end()) {
            // Model still in use or pinned - clear any pending unused timestamp
            modelUnusedSince_.erase(id);
            continue;
        }
        auto unusedIt = modelUnusedSince_.find(id);
        if (unusedIt == modelUnusedSince_.end()) {
            // First cycle with no instances - start the grace timer
            modelUnusedSince_[id] = now;
        } else if (now - unusedIt->second >= kGracePeriod) {
            // Grace period expired - mark for removal
            toRemove.push_back(id);
            modelUnusedSince_.erase(unusedIt);
        }
    }

    // Delete GPU resources and remove from map.
    // Wait for the GPU to finish all in-flight frames before destroying any
    // buffers - the previous frame's command buffer may still be referencing
    // vertex/index buffers that are about to be freed. Without this wait,
    // the GPU reads freed memory, which can cause VK_ERROR_DEVICE_LOST.
    // Suspected (not yet confirmed) contributor to multi-second freezes seen
    // right after taxi landings - timed here so the next repro pins it down.
    if (!toRemove.empty() && vkCtx_) {
        const auto waitStart = std::chrono::steady_clock::now();
        vkDeviceWaitIdle(vkCtx_->getDevice());
        const float waitMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        LOG_DEBUG("M2 cleanup: vkDeviceWaitIdle took ", waitMs, "ms (", toRemove.size(), " models to remove)");
    }
    for (uint32_t id : toRemove) {
        auto it = models.find(id);
        if (it != models.end()) {
            destroyModelGPU(it->second);
            models.erase(it);
            // Record the eviction so owners caching "uploaded" model IDs can
            // drop it; otherwise their next spawn skips the load as a stale hit
            // and pushes an empty placeholder (missing doodads on revisit).
            reapedModelIds_.push_back(id);
        }
    }

    if (!toRemove.empty()) {
        LOG_INFO("M2 cleanup: removed ", toRemove.size(), " unused models, ", models.size(), " remaining");
    }
}

std::vector<uint32_t> M2Renderer::drainReapedModelIds() {
    std::vector<uint32_t> out;
    out.swap(reapedModelIds_);
    return out;
}

void M2Renderer::unloadModel(uint32_t modelId) {
    shadowCullDirty_ = true;
    auto it = models.find(modelId);
    if (it == models.end()) return;
    if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
    destroyModelGPU(it->second);
    models.erase(it);
    modelUnusedSince_.erase(modelId);
    pinnedModelIds_.erase(modelId);
    reapedModelIds_.push_back(modelId);
}

size_t M2Renderer::evictUnreferencedTextures(size_t bytesNeeded) {
    if (textureCache.empty() || bytesNeeded == 0) return 0;

    // Free a slab rather than exactly what was asked for.
    //
    // Working out what is evictable means walking every loaded model, because
    // the in-use set is what those models point at. Doing that per texture is
    // a walk per load for as long as the cache sits at its ceiling - which is
    // exactly when textures are streaming in fastest. One sweep that frees a
    // slab serves the next few hundred loads instead.
    constexpr size_t kEvictionSlab = 64ull * 1024 * 1024;
    bytesNeeded = std::max(bytesNeeded, kEvictionSlab);

    // What a loaded model still points at. A batch, a particle emitter and a
    // ribbon each keep a raw pointer into this cache, and the per-frame
    // particle and ribbon groups are rebuilt from those - so anything a model
    // refers to is off limits however old it is.
    std::unordered_set<const VkTexture*> inUse;
    inUse.reserve(textureCache.size());
    for (const auto& [modelId, model] : models) {
        for (const auto& batch : model.batches) {
            if (batch.texture) inUse.insert(batch.texture);
        }
        for (const VkTexture* tex : model.particleTextures) {
            if (tex) inUse.insert(tex);
        }
        for (const VkTexture* tex : model.ribbonTextures) {
            if (tex) inUse.insert(tex);
        }
    }
    for (const auto& [path, strip] : stripTextures_) {
        if (strip.texture) inUse.insert(strip.texture);
    }

    std::vector<std::pair<uint64_t, std::string>> candidates;
    candidates.reserve(textureCache.size());
    for (const auto& [key, entry] : textureCache) {
        if (!entry.texture || inUse.count(entry.texture.get()) != 0) continue;
        candidates.emplace_back(entry.lastUse, key);
    }
    if (candidates.empty()) return 0;
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t freed = 0;
    size_t dropped = 0;
    for (const auto& [lastUse, key] : candidates) {
        if (freed >= bytesNeeded) break;
        auto it = textureCache.find(key);
        if (it == textureCache.end()) continue;
        freed += it->second.approxBytes;
        textureCacheBytes_ -= std::min(textureCacheBytes_, it->second.approxBytes);
        texturePropsByPtr_.erase(it->second.texture.get());
        // A command buffer submitted a frame or two ago may still be reading
        // it, so the texture outlives this call and dies once every frame slot
        // has fenced. std::function needs a copyable capture, hence the shared
        // pointer around what was a unique one.
        auto doomed = std::shared_ptr<VkTexture>(it->second.texture.release());
        vkCtx_->deferAfterAllFrameFences([doomed]() mutable { doomed.reset(); });
        textureCache.erase(it);
        ++dropped;
    }

    if (freed > 0) {
        // Whatever was refused while the cache was full can be asked for again
        // now. Only those: a texture that failed because its file is missing
        // is not worth retrying every time something else is evicted.
        for (const auto& key : budgetRejected_) {
            failedTextureCache_.erase(key);
            failedTextureRetryAt_.erase(key);
        }
        budgetRejected_.clear();
        LOG_INFO("M2 texture cache: evicted ", dropped, " unreferenced texture(s), freed ",
                 freed / (1024 * 1024), " MB (now ", textureCacheBytes_ / (1024 * 1024),
                 " MB / ", textureCacheBudgetBytes_ / (1024 * 1024), " MB)");
    }
    return freed;
}

VkTexture* M2Renderer::loadTexture(const std::string& path, uint32_t texFlags) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = normalizeKey(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;
    // The wrap mode belongs to the sampler, and models share a file with
    // different wrap flags; keyed on the path alone, whichever loaded it
    // first decided how every later one tiles.
    const std::string cacheKey = key + "#wrap" + std::to_string(texFlags & 0x3);

    // Check cache
    auto it = textureCache.find(cacheKey);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blp = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blp.isValid()) {
        // M2 skins are sampled and their transparency is read from the
        // blocks, so they need no decode.
        blp = assetManager->loadTexture(key, true);
    }
    if (!blp.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("M2: Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    const size_t approxBytes = blp.approxUploadBytes();
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        // Make room before giving up. lastUse has been recorded on every hit
        // and every insert since this cache was written and nothing ever read
        // it: over budget, the newest texture the player looked at was refused
        // and handed back the white one, permanently, while whatever filled
        // the cache first kept its place. A tree whose leaf sheet was refused
        // renders nothing in the main pass and casts the shadow of a solid
        // quad, and the retry timer below is what made that come and go.
        evictUnreferencedTextures(approxBytes);
    }
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        budgetRejected_.insert(key);
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Cache budget-rejected keys too; without this we repeatedly decode/load
            // the same textures every frame once budget is saturated.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            LOG_WARNING("M2 texture cache full (", textureCacheBytes_ / (1024 * 1024),
                        " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB), rejecting texture: ", path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    // Whether the texture actually uses alpha. Reads the DXT blocks when the
    // loader kept them, and every fourth decoded byte when it did not; the two
    // agree, which tests/test_blp_alpha_scan.cpp checks against the assets.
    // This was the only thing forcing M2 textures to be decoded.
    const bool hasAlpha = blp.hasTransparency();

    // And whether that alpha is a silhouette or an atlas leftover, which is
    // what an opaque batch has to know before anything keys on it.
    const bool alphaIsSilhouette = blp.alphaIsSilhouette();

    // Create Vulkan texture
    auto tex = std::make_unique<VkTexture>();
    tex->uploadBLP(*vkCtx_, blp);

    // M2Texture flags: bit 0 = WrapS (1=repeat, 0=clamp), bit 1 = WrapT
    VkSamplerAddressMode wrapS = (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode wrapT = (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // WOWEE_SKY_MIP_BIAS pushes the sky's textures toward smaller mips.
    //
    // Every per-frame quantity behind the sky has now been measured and holds
    // still while it flickers: the lighting, the clock, the culling, the frame
    // time, and every one of the thirty-four batches' material flags. Nothing
    // is alpha tested, colour keyed, or drawn as a glow card. What is left is
    // the texture sample, and the sky is the one model where that is loud:
    // eighteen of its layers are additive, so each layer's sampling noise adds
    // to the last rather than replacing it, and the dome is at its most
    // foreshortened exactly where a turn sweeps fastest.
    //
    // A knob rather than a fixed value, because if this is the cause the right
    // amount is a thing to measure and not to guess.
    static const float skyMipBias = [] {
        const char* set = std::getenv("WOWEE_SKY_MIP_BIAS");
        return set ? std::strtof(set, nullptr) : 0.0f;
    }();
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, wrapS, wrapT,
                       16.0f, skyMode_ ? skyMipBias : 0.0f);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    textureCache[cacheKey] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha,
                                  .alphaIsSilhouette = alphaIsSilhouette};
    LOG_DEBUG("M2: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");

    return texPtr;
}

uint32_t M2Renderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : instances) {
        if (instance.cachedModel) {
            total += instance.cachedModel->indexCount / 3;
        }
    }
    return total;
}

void M2Renderer::debugLogCollidersNear(const glm::vec3& pos, float radius) const {
    for (const auto& instance : instances) {
        if (!instance.cachedModel || !instance.cachedModel->collision.valid()) continue;
        if (glm::length(glm::vec2(instance.position - pos)) > radius) continue;
        LOG_WARNING("COLLIDER '", instance.cachedModel->name, "' pos ", instance.position.x, " ",
                    instance.position.y, " ", instance.position.z, " rot ", instance.rotation.x,
                    " ", instance.rotation.y, " ", instance.rotation.z, " scale ", instance.scale,
                    " tris ", instance.cachedModel->collision.triCount);
    }
}

void M2Renderer::debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const {
    LOG_WARNING("=== M2 Floor Debug at render(", glX, ", ", glY, ", ", glZ, ") ===");
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);
    int reported = 0;
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        const char* rejected = nullptr;
        if (collisionFocus.excludes(instance.worldBoundsMin, instance.worldBoundsMax)) {
            rejected = "outside the collision focus";
        } else if (!instance.cachedModel) {
            rejected = "no model";
        } else if (instance.scale <= 0.001f) {
            rejected = "zero scale";
        } else if (instance.skipCollision) {
            rejected = "skipCollision";
        }
        const M2ModelGPU* model = instance.cachedModel;
        const bool authored = model && model->collision.valid();
        if (!rejected && model) {
            if ((model->collisionNoBlock && !authored) || model->isInvisibleTrap ||
                model->isSpellEffect) {
                rejected = "classified as non-blocking";
            }
        }
        if (++reported > 12) { LOG_WARNING("  ... and more"); break; }
        LOG_WARNING("  '", model ? model->name : std::string("?"),
                    "' isGameObject=", instance.isGameObject ? 1 : 0,
                    " authoredCollision=", authored ? 1 : 0,
                    " tris=", authored ? model->collision.triCount : 0u,
                    " bounds z ", instance.worldBoundsMin.z, "..", instance.worldBoundsMax.z,
                    (rejected ? "  REJECTED: " : "  considered"), (rejected ? rejected : ""));
    }
    if (reported == 0) {
        LOG_WARNING("  nothing at all - no doodad instance overlaps this spot,"
                    " so a deck underfoot is not in the spatial grid here");
    }
    LOG_WARNING("=== Total: ", reported, " M2 candidates ===");
}

bool M2Renderer::getInstanceCollisionBox(uint32_t instanceId, glm::vec3& outMin,
                                         glm::vec3& outMax) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    outMin = inst.cachedModel->collisionBoxMin;
    outMax = inst.cachedModel->collisionBoxMax;
    return true;
}

std::optional<float> M2Renderer::getInstanceFloorHeight(uint32_t instanceId,
                                                       float glX, float glY,
                                                       float glZ) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end() || idxIt->second >= instances.size()) {
        return std::nullopt;
    }
    const auto& instance = instances[idxIt->second];
    if (!instance.cachedModel || instance.scale <= 0.001f) return std::nullopt;
    const M2ModelGPU& model = *instance.cachedModel;
    if (!model.collision.valid()) return std::nullopt;

    // Same cast as getFloorHeight: world-down through the instance transform,
    // so a car placed with any pitch still sees a ray along gravity.
    const glm::vec3 localRayOrigin = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
    const glm::vec3 localRayEnd = glm::vec3(
        instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
    const glm::vec3 localRayVector = localRayEnd - localRayOrigin;
    const float localRayLength = glm::length(localRayVector);
    if (localRayLength <= 1e-5f) return std::nullopt;
    const glm::vec3 localRayDir = localRayVector / localRayLength;

    model.collision.getFloorTrisInRange(
        std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
        std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
        std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
        std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
        tl_m2_collisionTriScratch);

    std::optional<float> best;
    for (uint32_t ti : tl_m2_collisionTriScratch) {
        if (ti >= model.collision.triCount) continue;
        const auto& verts = model.collision.vertices;
        const auto& idx = model.collision.indices;
        const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir,
                                                verts[idx[ti * 3]],
                                                verts[idx[ti * 3 + 1]],
                                                verts[idx[ti * 3 + 2]]);
        if (tHit < 0.0f || tHit > localRayLength) continue;
        const glm::vec3 worldHit = glm::vec3(
            instance.modelMatrix * glm::vec4(localRayOrigin + localRayDir * tHit, 1.0f));
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (worldHit.z <= glZ && (!best || worldHit.z > *best)) best = worldHit.z;
    }
    return best;
}

void M2Renderer::gatherBlobShadowGround(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                        std::vector<glm::vec3>& out) const {
    gatherCollisionTriangles(boxMin, boxMax, out, true);
}

void M2Renderer::gatherCollisionTriangles(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                          std::vector<glm::vec3>& out, bool facingUpOnly) const {
    std::vector<size_t> candidates;
    gatherCandidates(boxMin, boxMax, candidates);
    std::vector<uint32_t> tris, walls;
    for (size_t idx : candidates) {
        const auto& instance = instances[idx];
        // The map's doodads, in chunks and in buildings: not a game object,
        // which is an entity of its own, and not the ground clutter.
        if (!instance.cachedModel || instance.isGameObject || instance.detailDoodad) continue;
        if (instance.scale <= 0.001f || instance.skipCollision) continue;
        if (boxMax.x < instance.worldBoundsMin.x || boxMin.x > instance.worldBoundsMax.x ||
            boxMax.y < instance.worldBoundsMin.y || boxMin.y > instance.worldBoundsMax.y ||
            boxMax.z < instance.worldBoundsMin.z || boxMin.z > instance.worldBoundsMax.z) {
            continue;
        }
        const M2ModelGPU& model = *instance.cachedModel;
        if (!model.collision.valid() || model.isInvisibleTrap || model.isSpellEffect) continue;

        // The box in the model's own space, for the mesh's cell grid.
        glm::vec2 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? boxMax.x : boxMin.x, (c & 2) ? boxMax.y : boxMin.y,
                                   (c & 4) ? boxMax.z : boxMin.z);
            const glm::vec3 local(instance.invModelMatrix * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, glm::vec2(local));
            hi = glm::max(hi, glm::vec2(local));
        }
        // Every collision triangle the box reaches, floor or wall alike; for
        // a blob shadow, facing up is the only test (0x007e32f0). Each query
        // clears its list.
        model.collision.getFloorTrisInRange(lo.x, lo.y, hi.x, hi.y, tris);
        model.collision.getWallTrisInRange(lo.x, lo.y, hi.x, hi.y, walls);
        tris.insert(tris.end(), walls.begin(), walls.end());
        const auto& verts = model.collision.vertices;
        const auto& indices = model.collision.indices;
        for (uint32_t ti : tris) {
            if (ti >= model.collision.triCount) continue;
            const glm::vec3& a = verts[indices[ti * 3]];
            const glm::vec3& b = verts[indices[ti * 3 + 1]];
            const glm::vec3& c = verts[indices[ti * 3 + 2]];
            if (facingUpOnly && !blob_shadow::facesUp(instance.modelMatrix, a, b, c)) continue;
            const glm::vec3 wa(instance.modelMatrix * glm::vec4(a, 1.0f));
            const glm::vec3 wb(instance.modelMatrix * glm::vec4(b, 1.0f));
            const glm::vec3 wc(instance.modelMatrix * glm::vec4(c, 1.0f));
            const glm::vec3 tMin = glm::min(wa, glm::min(wb, wc));
            const glm::vec3 tMax = glm::max(wa, glm::max(wb, wc));
            if (glm::any(glm::lessThan(tMax, boxMin)) || glm::any(glm::greaterThan(tMin, boxMax))) continue;
            out.push_back(wa);
            out.push_back(wb);
            out.push_back(wc);
        }
    }
}

namespace {
// WOWEE_COLLISION_TRACE: each model whose underside the floor query met with
// no top above it, once - where a two-sided query would have stood someone
// on a face the client passes through.
const bool kCollisionTraceFloor = std::getenv("WOWEE_COLLISION_TRACE") != nullptr;
void noteUndersideOnlyFloor(const std::string& modelName, float undersideZ, float probeZ) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen.insert(modelName).second) return;
    LOG_WARNING("M2 floor: only the underside of '", modelName, "' under the probe (z ",
                undersideZ, ", probe ", probeZ, ") - not stood on");
}
}  // namespace

std::optional<float> M2Renderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);
    return floorAmong(tl_m2_candidateScratch, glX, glY, glZ, outNormalZ);
}

void M2Renderer::gatherFloorCandidates(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                       std::vector<uint32_t>& outIds) const {
    // What floorAmong would pass over at every point of the box, left out
    // here once: the instances not over it, and those with nothing to stand on.
    outIds.clear();
    gatherCandidates(boxMin - glm::vec3(2.0f, 2.0f, 6.0f), boxMax + glm::vec3(2.0f, 2.0f, 8.0f),
                     tl_m2_candidateScratch);
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& inst = instances[idx];
        if (inst.worldBoundsMax.x < boxMin.x || inst.worldBoundsMin.x > boxMax.x ||
            inst.worldBoundsMax.y < boxMin.y || inst.worldBoundsMin.y > boxMax.y) {
            continue;
        }
        if (!inst.cachedModel || inst.scale <= 0.001f || inst.skipCollision) continue;
        const M2ModelGPU& model = *inst.cachedModel;
        if (!model.collision.valid() || model.isInvisibleTrap || model.isSpellEffect) continue;
        outIds.push_back(inst.id);
    }
}

std::optional<float> M2Renderer::getFloorHeightAmong(const std::vector<uint32_t>& ids, float glX, float glY,
                                                     float glZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    thread_local std::vector<size_t> indices;
    indices.clear();
    for (uint32_t id : ids) {
        auto it = instanceIndexById.find(id);
        if (it != instanceIndexById.end()) indices.push_back(it->second);
    }
    return floorAmong(indices, glX, glY, glZ, nullptr);
}

std::optional<float> M2Renderer::floorAmong(const std::vector<size_t>& candidates, float glX, float glY, float glZ,
                                            float* outNormalZ) const {
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;  // Default to flat

    for (size_t idx : candidates) {
        const auto& instance = instances[idx];
        // Not under it, not met: both the mesh's faces and the box's top
        // below lie inside the world bounds (getCollisionQueryBounds takes
        // in the whole mesh), and the ray here is straight down. Asked
        // first, before the model is looked at, so the doodads of a 64-yard
        // grid cell that are nowhere near cost a comparison each rather than
        // a transform and a gather of their faces.
        constexpr float kBoundsSlop = 0.01f;
        if (glX < instance.worldBoundsMin.x - kBoundsSlop || glX > instance.worldBoundsMax.x + kBoundsSlop ||
            glY < instance.worldBoundsMin.y - kBoundsSlop || glY > instance.worldBoundsMax.y + kBoundsSlop) {
            continue;
        }

        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        if (!instance.cachedModel) continue;
        if (instance.scale <= 0.001f) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        // And one that ships none does not block or bear weight at all, as
        // in the client, whatever its name or bounds: a doodad collides by
        // its M2's own collision triangles and nothing else. A box made up
        // from the bounds of everything the name test did not call soft is
        // how a campfire - elwynncampfire.m2, with no collision triangles -
        // came to stop a character the client walks straight through.
        if (!authoredCollision || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;

        // --- Mesh-based floor: vertical ray vs collision triangles ---
        // A successful authored-mesh hit wins; AABB remains the fallback on a miss.
        if (model.collision.valid()) {
            // Cast world-down through the instance transform. Using local -Z
            // only works for yaw-only placements; pitched flat models (notably
            // Exodarplatform01, placed as the Azuremyst flight-master ramp)
            // otherwise see a ray that is diagonal relative to world gravity.
            const glm::vec3 localRayOrigin = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ + 5.0f, 1.0f));
            const glm::vec3 localRayEnd = glm::vec3(
                instance.invModelMatrix * glm::vec4(glX, glY, glZ - 10.0f, 1.0f));
            glm::vec3 localRayVector = localRayEnd - localRayOrigin;
            const float localRayLength = glm::length(localRayVector);
            if (localRayLength <= 1e-5f) continue;
            const glm::vec3 localRayDir = localRayVector / localRayLength;

            model.collision.getFloorTrisInRange(
                std::min(localRayOrigin.x, localRayEnd.x) - 1.0f,
                std::min(localRayOrigin.y, localRayEnd.y) - 1.0f,
                std::max(localRayOrigin.x, localRayEnd.x) + 1.0f,
                std::max(localRayOrigin.y, localRayEnd.y) + 1.0f,
                tl_m2_collisionTriScratch);

            float bestWorldHitZ = -std::numeric_limits<float>::max();
            float bestHitNormalZ = 1.0f;
            bool hitAny = false;
            // The ray met the underside of the mesh within reach: whatever is
            // under the feet here, it is not this model's top.
            bool hitUnderside = false;
            float bestUndersideZ = -std::numeric_limits<float>::max();

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                // The intersection is already two-sided, so the reversed
                // winding this used to retry answers the same thing.
                const float tHit = rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2);
                if (tHit < 0.0f || tHit > localRayLength) continue;

                const glm::vec3 localHit = localRayOrigin + localRayDir * tHit;
                const glm::vec3 worldHit = glm::vec3(
                    instance.modelMatrix * glm::vec4(localHit, 1.0f));

                // Only a face that looks up holds the feet. The client finds
                // the ground by sweeping the character down, and a sweep only
                // tests a face whose plane - the file's normal, turned with
                // the instance (0x0082ec30) - opposes the motion (0x0075c5a0
                // skips n.dir > -1e-5). Taken from either side, the underside
                // of a door rising past the character was a floor, and it
                // lifted them with it.
                const glm::vec3 localN = collisionFaceNormal(
                    model.collision.normals, ti, v0, v1, v2);
                glm::vec3 worldN = glm::vec3(instance.modelMatrix * glm::vec4(localN, 0.0f));
                const float wnLen = glm::length(worldN);
                if (wnLen <= 1e-6f) continue;
                worldN /= wnLen;
                if (!faceHoldsDownwardSweep(worldN)) {
                    if (worldHit.z <= glZ + 3.0f) {
                        hitUnderside = true;
                        bestUndersideZ = std::max(bestUndersideZ, worldHit.z);
                    }
                    continue;
                }
                if (worldN.z < 0.35f) continue; // too steep (~70° max slope)

                if (worldHit.z <= glZ + 3.0f && worldHit.z > bestWorldHitZ) {
                    bestWorldHitZ = worldHit.z;
                    hitAny = true;
                    bestHitNormalZ = worldN.z;
                }
            }

            if (hitAny) {
                if (!bestFloor || bestWorldHitZ > *bestFloor) {
                    bestFloor = bestWorldHitZ;
                    bestNormalZ = bestHitNormalZ;
                }
                // A successful authored-mesh hit is more accurate than the
                // model AABB top, especially for pitched ramps where the AABB
                // projection is not a world-vertical intersection.
                continue;
            }
            // Only the underside was met: the feet are under or inside the
            // model - a door that has risen past them - and the box's top is
            // no floor for them either. Before faces were one-sided the
            // underside answered here and this was never reached; falling to
            // the box would lift the character onto the door just the same.
            if (hitUnderside) {
                if (kCollisionTraceFloor && bestUndersideZ > -1e30f) {
                    noteUndersideOnlyFloor(model.name, bestUndersideZ, glZ);
                }
                continue;
            }
            // Fall through to AABB floor - both contribute, highest wins
        }

        float zMargin = model.collisionBridge ? 25.0f : 2.0f;
        if (glX < instance.worldBoundsMin.x || glX > instance.worldBoundsMax.x ||
            glY < instance.worldBoundsMin.y || glY > instance.worldBoundsMax.y ||
            glZ < instance.worldBoundsMin.z - zMargin || glZ > instance.worldBoundsMax.z + zMargin) {
            continue;
        }
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);

        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

        // Must be within doodad footprint in local XY.
        // Stepped low platforms get a small pad so walk-up snapping catches edges.
        float footprintPad = 0.0f;
        if (model.collisionSteppedLowPlatform) {
            footprintPad = model.collisionPlanter ? 0.22f : 0.16f;
            if (model.collisionBridge) {
                footprintPad = 0.35f;
            }
        }
        if (localPos.x < localMin.x - footprintPad || localPos.x > localMax.x + footprintPad ||
            localPos.y < localMin.y - footprintPad || localPos.y > localMax.y + footprintPad) {
            continue;
        }

        // Construct "top" point at queried XY in local space, then transform back.
        float localTopZ = getEffectiveCollisionTopLocal(model, localPos, localMin, localMax);
        glm::vec3 localTop(localPos.x, localPos.y, localTopZ);
        glm::vec3 worldTop = glm::vec3(instance.modelMatrix * glm::vec4(localTop, 1.0f));

        // Reachability filter: allow a bit more climb for stepped low platforms.
        float maxStepUp = 1.0f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            maxStepUp = 2.0f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 3.0f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        if (worldTop.z > glZ + maxStepUp) continue;

        if (!bestFloor || worldTop.z > *bestFloor) {
            bestFloor = worldTop.z;
        }
    }

    // Output surface normal if requested
    if (outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

bool M2Renderer::checkCollision(const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjustedPos, float playerRadius) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;
    bool collided = false;

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(7.0f, 7.0f, 5.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    // Check against all M2 instances in local space (rotation-aware).
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        const float broadMargin = playerRadius + 1.0f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && adjustedPos.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && adjustedPos.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && adjustedPos.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && adjustedPos.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + 2.5f && adjustedPos.z > instance.worldBoundsMax.z + 2.5f) continue;
        if (from.z + 2.5f < instance.worldBoundsMin.z && adjustedPos.z + 2.5f < instance.worldBoundsMin.z) continue;

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        // And one that ships none does not block or bear weight at all, as
        // in the client, whatever its name or bounds: a doodad collides by
        // its M2's own collision triangles and nothing else. A box made up
        // from the bounds of everything the name test did not call soft is
        // how a campfire - elwynncampfire.m2, with no collision triangles -
        // came to stop a character the client walks straight through.
        if (!authoredCollision || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;
        if (instance.scale <= 0.001f) continue;

        // --- Mesh-based wall collision ---
        if (model.collision.valid()) {
            glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
            glm::vec3 localPos  = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
            float localRadius = playerRadius / instance.scale;

            model.collision.getWallTrisInRange(
                std::min(localFrom.x, localPos.x) - localRadius - 1.0f,
                std::min(localFrom.y, localPos.y) - localRadius - 1.0f,
                std::max(localFrom.x, localPos.x) + localRadius + 1.0f,
                std::max(localFrom.y, localPos.y) + localRadius + 1.0f,
                tl_m2_collisionTriScratch);

            const float localHeight = 2.0f / instance.scale;
            // A face no higher than a step is stepped onto, not walked into -
            // the floor query lifts the feet onto the top - but only where
            // there is a top to land on just past the leading edge of the
            // body, as the client's step (0x00761b00) lands on walkable ground
            // or does not climb. Without the landing a low face is a wall.
            const float localStepUp = movement::kMaxStepUp / instance.scale;
            std::optional<bool> stepLands;
            auto lowFaceIsStep = [&]() -> bool {
                if (stepLands) return *stepLands;
                stepLands = false;
                glm::vec2 dir(localPos.x - localFrom.x, localPos.y - localFrom.y);
                const float dirLen = glm::length(dir);
                if (dirLen < 1e-5f) return false;
                dir /= dirLen;
                // The client looks as far ahead as max(radius, 1.19 x step)
                // and needs to get more than a radius onto the top.
                const float reach = std::max(localRadius, 1.1917536f * localStepUp);
                static thread_local std::vector<uint32_t> floorTris;
                for (const float ahead : {localRadius + 0.05f, 0.5f * (localRadius + reach)}) {
                    const glm::vec2 p = glm::vec2(localPos) + dir * ahead;
                    model.collision.getFloorTrisInRange(p.x - 0.01f, p.y - 0.01f,
                                                        p.x + 0.01f, p.y + 0.01f, floorTris);
                    if (stepLandingTop(p, localPos.z - 0.25f / instance.scale,
                                       localPos.z + localStepUp, model.collision.vertices,
                                       model.collision.indices, model.collision.normals,
                                       floorTris, movement::kMinWalkableNormalZ)) {
                        stepLands = true;
                        break;
                    }
                }
                return *stepLands;
            };
            bool pushed = false;

            // Each face holds back what is in front of it and takes away all
            // of its overlap (holdCylinderOffWallTriangle). This was a push of
            // 8% of the overlap, at most 0.015 a face and 0.02 an instance - a
            // sixth of a running character's step - so every authored
            // collision mesh was something to walk through a little slower:
            // the Undercity lift doors, a box 2.7 deep, were crossed at full
            // speed from either side. A second pass settles a corner, where
            // taking one face's overlap away can push into the other.
            for (int pass = 0; pass < 2; ++pass) {
                bool movedThisPass = false;
                for (uint32_t ti : tl_m2_collisionTriScratch) {
                    if (ti >= model.collision.triCount) continue;
                    const auto& tb = model.collision.triBounds[ti];
                    if (localPos.z + localHeight < tb.minZ || localPos.z > tb.maxZ) continue;
                    if (tb.maxZ <= localPos.z + localStepUp && lowFaceIsStep()) continue;

                    const auto& verts = model.collision.vertices;
                    const auto& idx   = model.collision.indices;
                    if (holdCylinderOffWallTriangle(localFrom, localPos, localRadius, localHeight,
                                                    verts[idx[ti * 3]], verts[idx[ti * 3 + 1]],
                                                    verts[idx[ti * 3 + 2]])) {
                        pushed = true;
                        movedThisPass = true;
                    }
                }
                if (!movedThisPass) break;
            }

            if (pushed) {
                glm::vec3 worldPos = glm::vec3(instance.modelMatrix * glm::vec4(localPos, 1.0f));
                adjustedPos.x = worldPos.x;
                adjustedPos.y = worldPos.y;
                collided = true;
                // Which doodad is in the way, once per model.
                //
                // A model that blocks and should not is reported as "I am
                // stuck on this bush", and the one thing needed to fix it -
                // the model's name - is the one thing nobody can see. The
                // classifier decides collision from that name, so without it
                // the only way forward is guessing tokens, which is how the
                // folder-token regression happened.
                //
                // Once per distinct model, not per frame: walking into
                // something touches it every frame for as long as you lean on
                // it.
                static std::set<std::string> saidBlocked;
                if (!model.name.empty() && saidBlocked.insert(model.name).second) {
                    LOG_WARNING("Collision: blocked by '", model.name,
                                "' - if this should be walked through, that is "
                                "the name the classifier needs");
                }
            }
            continue;
        }

        glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
        float radiusScale = model.collisionNarrowVerticalProp ? 0.45f : 1.0f;
        float localRadius = (playerRadius * radiusScale) / instance.scale;

        glm::vec3 rawMin, rawMax;
        getTightCollisionBounds(model, rawMin, rawMax);
        glm::vec3 localMin = rawMin - glm::vec3(localRadius);
        glm::vec3 localMax = rawMax + glm::vec3(localRadius);
        float effectiveTop = getEffectiveCollisionTopLocal(model, localPos, rawMin, rawMax) + localRadius;
        glm::vec2 localCenter((localMin.x + localMax.x) * 0.5f, (localMin.y + localMax.y) * 0.5f);
        float fromR = glm::length(glm::vec2(localFrom.x, localFrom.y) - localCenter);
        float toR = glm::length(glm::vec2(localPos.x, localPos.y) - localCenter);

        // Feet-based vertical overlap test: ignore objects fully above/below us.
        constexpr float PLAYER_HEIGHT = 2.0f;
        if (localPos.z + PLAYER_HEIGHT < localMin.z || localPos.z > effectiveTop) {
            continue;
        }

        bool fromInsideXY =
            (localFrom.x >= localMin.x && localFrom.x <= localMax.x &&
             localFrom.y >= localMin.y && localFrom.y <= localMax.y);
        bool fromInsideZ = (localFrom.z + PLAYER_HEIGHT >= localMin.z && localFrom.z <= effectiveTop);
        bool escapingOverlap = (fromInsideXY && fromInsideZ && (toR > fromR + 1e-4f));
        bool allowEscapeRelax = escapingOverlap && !model.collisionSmallSolidProp;

        // Swept hard clamp for taller blockers only.
        // Low/stepable objects should be climbable and not "shove" the player off.
        float maxStepUp = 1.20f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            // Keep box/crate-class props hard-solid to prevent phase-through.
            maxStepUp = 0.75f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 2.8f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        bool stepableLowObject = (effectiveTop <= localFrom.z + maxStepUp);
        bool climbingAttempt = (localPos.z > localFrom.z + 0.18f);
        bool nearTop = (localFrom.z >= effectiveTop - 0.30f);
        float climbAllowance = model.collisionPlanter ? 0.95f : 0.60f;
        if (model.collisionSteppedLowPlatform && !model.collisionPlanter) {
            // Let low curb/planter blocks be stepable without sticky side shoves.
            climbAllowance = 1.00f;
        }
        if (model.collisionBridge) {
            climbAllowance = 3.0f;
        }
        if (model.collisionSmallSolidProp) {
            climbAllowance = 1.05f;
        }
        bool climbingTowardTop = climbingAttempt && (localFrom.z + climbAllowance >= effectiveTop);
        bool forceHardLateral =
            model.collisionSmallSolidProp &&
            !nearTop && !climbingTowardTop;
        if ((!stepableLowObject || forceHardLateral) && !allowEscapeRelax) {
            float tEnter = 0.0f;
            glm::vec3 sweepMax = localMax;
            sweepMax.z = std::min(sweepMax.z, effectiveTop);
            if (segmentIntersectsAABB(localFrom, localPos, localMin, sweepMax, tEnter)) {
                float tSafe = std::clamp(tEnter - 0.03f, 0.0f, 1.0f);
                glm::vec3 localSafe = localFrom + (localPos - localFrom) * tSafe;
                glm::vec3 worldSafe = glm::vec3(instance.modelMatrix * glm::vec4(localSafe, 1.0f));
                adjustedPos.x = worldSafe.x;
                adjustedPos.y = worldSafe.y;
                collided = true;
                // The other way a doodad blocks, and the one that was missing.
                //
                // The first of these lines went inside the branch for models
                // that carry a collision mesh. A model without one is stopped
                // by its bounding box here instead, so the doodads reported as
                // wrongly solid - grass among them - were exactly the ones the
                // diagnostic could not see.
                static std::set<std::string> saidBoxBlocked;
                if (!model.name.empty() && saidBoxBlocked.insert(model.name).second) {
                    LOG_WARNING("Collision: blocked by '", model.name,
                                "' (bounding box, no collision mesh) - if this "
                                "should be walked through, that is the name the "
                                "classifier needs");
                }
                continue;
            }
        }

        if (localPos.x < localMin.x || localPos.x > localMax.x ||
            localPos.y < localMin.y || localPos.y > localMax.y) {
            continue;
        }

        float pushLeft  = localPos.x - localMin.x;
        float pushRight = localMax.x - localPos.x;
        float pushBack  = localPos.y - localMin.y;
        float pushFront = localMax.y - localPos.y;

        float minPush = std::min({pushLeft, pushRight, pushBack, pushFront});
        if (allowEscapeRelax) {
            continue;
        }
        if (stepableLowObject && localFrom.z >= effectiveTop - 0.35f) {
            // Already on/near top surface: don't apply lateral push that ejects
            // the player from the object (carpets, platforms, etc).
            continue;
        }
        // Gentle fallback push for overlapping cases.
        float pushAmount;
        if (model.collisionNarrowVerticalProp) {
            pushAmount = std::clamp(minPush * 0.10f, 0.001f, 0.010f);
        } else if (model.collisionSteppedLowPlatform) {
            if (model.collisionPlanter && stepableLowObject) {
                pushAmount = std::clamp(minPush * 0.06f, 0.001f, 0.006f);
            } else {
            pushAmount = std::clamp(minPush * 0.12f, 0.003f, 0.012f);
            }
        } else if (stepableLowObject) {
            pushAmount = std::clamp(minPush * 0.12f, 0.002f, 0.015f);
        } else {
            pushAmount = std::clamp(minPush * 0.28f, 0.010f, 0.045f);
        }
        glm::vec3 localPush(0.0f);
        if (minPush == pushLeft) {
            localPush.x = -pushAmount;
        } else if (minPush == pushRight) {
            localPush.x = pushAmount;
        } else if (minPush == pushBack) {
            localPush.y = -pushAmount;
        } else {
            localPush.y = pushAmount;
        }

        glm::vec3 worldPush = glm::vec3(instance.modelMatrix * glm::vec4(localPush, 0.0f));
        adjustedPos.x += worldPush.x;
        adjustedPos.y += worldPush.y;
        collided = true;
    }

    return collided;
}

float M2Renderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocus.excludes(instance.worldBoundsMin,
                                    instance.worldBoundsMax)) {
            continue;
        }

        // Cheap world-space broad-phase.
        float tEnter = 0.0f;
        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.35f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.35f);
        if (!segmentIntersectsAABB(origin, origin + direction * maxDistance, worldMin, worldMax, tEnter)) {
            continue;
        }

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        // A model that ships collision geometry blocks, whatever its name
        // suggests. collisionNoBlock is a guess from the name and the unscaled
        // bounds, and it is only there to stop collision being *invented* for
        // something soft - it has no business discarding what the artist
        // authored.
        //
        // It was discarding a great deal. hellfiretreethorns03 carries 15,376
        // collision triangles and is 2.87 wide unscaled, so it failed the
        // treeWithTrunk gate of horiz > 6, came out a softTree, and was walked
        // through. Every bush and rug beside it ships zero collision triangles,
        // so they stay soft on their own evidence and need no rule at all.
        const bool authoredCollision = model.collision.valid();
        // And one that ships none does not block or bear weight at all, as
        // in the client, whatever its name or bounds: a doodad collides by
        // its M2's own collision triangles and nothing else. A box made up
        // from the bounds of everything the name test did not call soft is
        // how a campfire - elwynncampfire.m2, with no collision triangles -
        // came to stop a character the client walks straight through.
        if (!authoredCollision || model.isInvisibleTrap || model.isSpellEffect) continue;
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);
        // Skip tiny doodads for camera occlusion; they cause jitter and false hits.
        glm::vec3 extents = (localMax - localMin) * instance.scale;
        if (glm::dot(extents, extents) < 0.5625f) continue;

        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));
        if (!std::isfinite(localDir.x) || !std::isfinite(localDir.y) || !std::isfinite(localDir.z)) {
            continue;
        }

        // Local-space AABB slab intersection.
        glm::vec3 invDir = 1.0f / localDir;
        glm::vec3 tMin = (localMin - localOrigin) * invDir;
        glm::vec3 tMax = (localMax - localOrigin) * invDir;
        glm::vec3 t1 = glm::min(tMin, tMax);
        glm::vec3 t2 = glm::max(tMin, tMax);

        float tNear = std::max({t1.x, t1.y, t1.z});
        float tFar = std::min({t2.x, t2.y, t2.z});
        if (tNear > tFar || tFar <= 0.0f) continue;

        float tHit = tNear > 0.0f ? tNear : tFar;
        glm::vec3 localHit = localOrigin + localDir * tHit;
        glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
        float worldDist = glm::length(worldHit - origin);
        if (worldDist > 0.0f && worldDist < closestHit) {
            closestHit = worldDist;
        }
    }

    return closestHit;
}

void M2Renderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layouts)
    destroy(device, opaquePipeline_);
    destroy(device, cutoutPipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, noAlphaAddPipeline_);
    destroy(device, modPipeline_);
    destroy(device, mod2xPipeline_);
    destroyPipelineVariants();
    destroy(device, particlePipeline_);
    destroy(device, particleAdditivePipeline_);
    destroy(device, particleNoAlphaAddPipeline_);
    destroy(device, particleModPipeline_);
    destroy(device, particleMod2xPipeline_);
    destroyRibbonPipelines();

    // The same pipelines initialize() builds, built by the same
    // function. The layouts are untouched above, so it makes none.
    buildMainPassPipelines(perFrameLayout_);

    core::Logger::getInstance().info("M2Renderer: pipelines recreated");
}

void M2Renderer::collectGrassClearings(float minX, float minY, float maxX, float maxY,
                                       std::vector<pipeline::GrassClearingSource>& out) const {
    constexpr float kEase = 4.0f;
    for (const auto& inst : instances) {
        // The ground detail IS the grass's kin - clutter clearing the ground
        // around clutter would eat the whole field.
        if (inst.cachedIsGroundDetail) continue;

        // Footprint radius from the horizontal bounds, capped: a fence post
        // clears its foot, a wagon its wheelbase, and a tree its trunk - not
        // the shadow of its crown, or every forest would be bald.
        const float ex = inst.worldBoundsMax.x - inst.worldBoundsMin.x;
        const float ey = inst.worldBoundsMax.y - inst.worldBoundsMin.y;
        const float clearing = std::clamp(0.35f * std::max(ex, ey), 0.4f, 3.0f);
        const float reach = clearing + kEase;

        const float cx = 0.5f * (inst.worldBoundsMin.x + inst.worldBoundsMax.x);
        const float cy = 0.5f * (inst.worldBoundsMin.y + inst.worldBoundsMax.y);
        if (cx + reach < minX || cx - reach > maxX ||
            cy + reach < minY || cy - reach > maxY) {
            continue;
        }
        out.push_back({cx, cy, cx, cy, clearing, kEase});
    }
}

} // namespace rendering
} // namespace wowee

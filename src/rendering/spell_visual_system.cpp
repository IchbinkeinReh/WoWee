#include "rendering/spell_visual_system.hpp"
#include "rendering/spell_missile.hpp"
#include "rendering/placement_transform.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_engine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
}

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first.
    if (!cachedAssetManager_) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svPrecastKitField = svLayout  ? (*svLayout)["PrecastKit"]    : 1;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Kit effect fields to probe, in priority order.
    // SpecialEffect0 > BaseEffect > LeftHand > RightHand > Chest > Head > Breath
    struct KitField { const char* name; uint32_t fallback; };
    static constexpr KitField kitFieldDefs[] = {
        {.name = "SpecialEffect0",  .fallback = 11}, {.name = "BaseEffect",       .fallback = 5},
        {.name = "LeftHandEffect",   .fallback = 6}, {.name = "RightHandEffect",  .fallback = 7},
        {.name = "ChestEffect",      .fallback = 4}, {.name = "HeadEffect",       .fallback = 3},
        {.name = "BreathEffect",     .fallback = 8}, {.name = "SpecialEffect1",  .fallback = 12},
        {.name = "SpecialEffect2",  .fallback = 13},
    };
    constexpr size_t numKitFields = sizeof(kitFieldDefs) / sizeof(kitFieldDefs[0]);
    uint32_t kitFields[numKitFields];
    for (size_t k = 0; k < numKitFields; ++k)
        kitFields[k] = kitLayout ? kitLayout->field(kitFieldDefs[k].name) : kitFieldDefs[k].fallback;

    // Load SpellVisualEffectName.dbc - ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    // Scale is what the client sizes a missile by (FUN_00732ff0 reads it
    // beside the path); the kits here do not use it.
    const uint32_t fxScaleField = fxLayout ? fxLayout->tryField("Scale") : 0xFFFFFFFFu;
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    std::unordered_map<uint32_t, float> effectScales;      // effectNameId → Scale
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) {
            // The DBC stores the extension the art was authored with; what
            // shipped is .m2.
            p = pipeline::modelPathToM2(p);
            effectPaths[id] = p;
            if (fxScaleField < fxDbc->getFieldCount())
                effectScales[id] = fxDbc->getFloat(i, fxScaleField);
        }
    }

    // Load SpellVisualKit.dbc - kitId → best SpellVisualEffectName ID
    // Probes all effect slots in priority order and keeps the first valid hit.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    std::unordered_map<uint32_t, uint32_t> kitToEffectName; // kitId → effectNameId
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            uint32_t eff = 0;
            for (size_t k = 0; k < numKitFields && !eff; ++k) {
                if (kitFields[k] < fc)
                    eff = kitDbc->getUInt32(i, kitFields[k]);
            }
            if (eff) kitToEffectName[kitId] = eff;
        }
    }

    // SpellVisualKit SoundID (column 15, 0x00745230 reads it at +0x3c): the
    // sound the kit plays with its models.
    std::unordered_map<uint32_t, uint32_t> kitSounds;  // kitId → SoundEntries id
    if (kitDbc && kitDbc->isLoaded()) {
        const uint32_t kitSoundField = kitLayout ? kitLayout->tryField("SoundID") : 0xFFFFFFFFu;
        if (kitSoundField < kitDbc->getFieldCount()) {
            for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
                const uint32_t sound = kitDbc->getUInt32(i, kitSoundField);
                if (sound != 0) kitSounds[kitDbc->getUInt32(i, 0)] = sound;
            }
        }
    }

    // Helper: resolve path for a given kit ID
    auto kitPath = [&](uint32_t kitId) -> std::string {
        if (!kitId) return {};
        auto kitIt = kitToEffectName.find(kitId);
        if (kitIt == kitToEffectName.end()) return {};
        auto fxIt = effectPaths.find(kitIt->second);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };
    auto missilePath = [&](uint32_t effId) -> std::string {
        if (!effId) return {};
        auto fxIt = effectPaths.find(effId);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };

    // Load SpellVisual.dbc - visualId → cast/impact M2 paths via kit chain
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    // The missile columns (3.3.5a's SpellVisual has 32, FUN_008b93f0). Only
    // the WotLK layout names them; elsewhere the missile keeps its defaults.
    auto svColumn = [&](const char* name) -> uint32_t {
        const uint32_t f = svLayout ? svLayout->tryField(name) : 0xFFFFFFFFu;
        return f < svFc ? f : 0xFFFFFFFFu;
    };
    const uint32_t svFlagsField      = svColumn("Flags");
    const uint32_t svMissileAttField = svColumn("MissileAttachment");
    const uint32_t svMissileDstField = svColumn("MissileDestinationAttachment");
    const uint32_t svMissileSoundField = svColumn("MissileSound");
    const uint32_t svCastOffField[3] = {svColumn("MissileCastOffsetX"), svColumn("MissileCastOffsetY"),
                                        svColumn("MissileCastOffsetZ")};
    const uint32_t svImpactOffField[3] = {svColumn("MissileImpactOffsetX"), svColumn("MissileImpactOffsetY"),
                                          svColumn("MissileImpactOffsetZ")};
    // Where the missile flies, its model is the missile and nothing else: the
    // client never plays it as a cast or an impact. Elsewhere it stands in
    // for a kit the visual lacks, as it always has here.
    const bool missilesFly = svMissileDstField != 0xFFFFFFFFu;
    auto svInt = [&](uint32_t row, uint32_t field, int32_t fallback) {
        return field != 0xFFFFFFFFu ? svDbc->getInt32(row, field) : fallback;
    };
    auto svVec = [&](uint32_t row, const uint32_t (&fields)[3]) {
        glm::vec3 v(0.0f);
        for (int axis = 0; axis < 3; ++axis)
            if (fields[axis] != 0xFFFFFFFFu) v[axis] = svDbc->getFloat(row, fields[axis]);
        return v;
    };
    uint32_t loadedPrecast = 0, loadedCast = 0, loadedImpact = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;

        // Missile: MissileModel names a SpellVisualEffectName row. Negative
        // values are the caster's weapons and ammunition (FUN_00732ff0's -1
        // to -5), which this does not fly.
        if (svMissileField < svFc) {
            const int32_t missileModel = svDbc->getInt32(i, svMissileField);
            auto fxIt = missileModel > 0 ? effectPaths.find(static_cast<uint32_t>(missileModel))
                                         : effectPaths.end();
            if (fxIt != effectPaths.end()) {
                MissileVisual missile;
                missile.modelPath = fxIt->second;
                auto scaleIt = effectScales.find(static_cast<uint32_t>(missileModel));
                if (scaleIt != effectScales.end() && scaleIt->second > 0.0f)
                    missile.scale = scaleIt->second;
                missile.flags = static_cast<uint32_t>(svInt(i, svFlagsField, 0));
                missile.sourceAttachment = spell_missile::m2AttachmentFor(
                    svInt(i, svMissileAttField, -1), missile.flags);
                missile.destinationAttachment = spell_missile::m2AttachmentFor(
                    svInt(i, svMissileDstField, -1), missile.flags);
                missile.castOffset = spell_missile::attachmentOffset(svVec(i, svCastOffField));
                missile.impactOffset = spell_missile::attachmentOffset(svVec(i, svImpactOffField));
                missile.soundId = static_cast<uint32_t>(std::max(svInt(i, svMissileSoundField, 0), 0));
                missileVisuals_[vid] = std::move(missile);
            }
        }

        // Precast path: PrecastKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svPrecastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svPrecastKitField));
            if (!path.empty()) { spellVisualPrecastPath_[vid] = path; ++loadedPrecast; }
        }
        // Cast path: CastKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        // where the missile does not fly (a layout without its columns).
        {
            std::string path;
            if (svCastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svCastKitField));
            if (path.empty() && !missilesFly && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualCastPath_[vid] = path; ++loadedCast; }
        }
        // The impact kit's sound.
        if (svImpactKitField < svFc) {
            auto soundIt = kitSounds.find(svDbc->getUInt32(i, svImpactKitField));
            if (soundIt != kitSounds.end()) impactKitSounds_[vid] = soundIt->second;
        }
        // Impact path: ImpactKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svImpactKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svImpactKitField));
            if (path.empty() && !missilesFly && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualImpactPath_[vid] = path; ++loadedImpact; }
        }
    }
    LOG_INFO("SpellVisual: loaded precast=", loadedPrecast, " cast=", loadedCast, " impact=", loadedImpact,
             " missile=", missileVisuals_.size(),
             " visual\u2192M2 mappings (of ", svDbc->getRecordCount(), " records)");
}

// ---------------------------------------------------------------------------
// Classify model path to a character attachment point for bone tracking
// ---------------------------------------------------------------------------
uint32_t SpellVisualSystem::classifyAttachmentId(const std::string& modelPath) {
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects track the right hand (attachment 1)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos)
        return 1; // RightHand
    // "chest" effects track chest/torso (attachment 5 in M2 spec)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos)
        return 5; // Chest
    // "head" effects track head (attachment 11)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos)
        return 11; // Head
    return 0; // No bone tracking (static position or base effect)
}

// ---------------------------------------------------------------------------
// Height offset for spell effect placement (fallback when no bone tracking)
// ---------------------------------------------------------------------------
glm::vec3 SpellVisualSystem::applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath) {
    // Lowercase the path for case-insensitive matching
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects go at hand height (~0.8m above feet)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 0.8f);
    }
    // "chest" effects go at chest height (~1.0m above feet)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.0f);
    }
    // "head" effects go at head height (~1.6m above feet)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.6f);
    }
    // "base" / "feet" / ground effects stay at ground level
    return basePos;
}

uint32_t SpellVisualSystem::acquireEffectModel(const std::string& modelPath) {
    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return 0;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: model in failed-cache, skipping: ", modelPath);
        return 0;
    }
    if (m2Renderer_->hasModel(modelId)) return modelId;

    auto m2Data = cachedAssetManager_->readFile(modelPath);
    if (m2Data.empty()) {
        LOG_WARNING("SpellVisual: could not read model: ", modelPath);
        spellVisualFailedModels_.insert(modelId);
        return 0;
    }
    pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
    if (model.name.empty()) model.name = modelPath;
    LOG_INFO("SpellVisual: M2 parsed: verts=", model.vertices.size(),
             " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
             " ribbons=", model.ribbonEmitters.size());
    if (model.vertices.empty() && model.particleEmitters.empty()) {
        LOG_WARNING("SpellVisual: empty model: ", modelPath);
        spellVisualFailedModels_.insert(modelId);
        return 0;
    }
    // Load skin file for WotLK-format M2s
    if (model.version >= 264) {
        std::string skinPath = pipeline::skinPathForM2(modelPath);
        auto skinData = cachedAssetManager_->readFile(skinPath);
        if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
    }
    if (!m2Renderer_->loadModel(model, modelId)) {
        LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
        spellVisualFailedModels_.insert(modelId);
        return 0;
    }
    m2Renderer_->markModelAsSpellEffect(modelId);
    LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    return modelId;
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisualPrecast visualId=", visualId,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z,
             ") castTimeMs=", castTimeMs);
    if (!m2Renderer_ || visualId == 0) {
        LOG_WARNING("SpellVisual: playSpellVisualPrecast early-out: m2Renderer_=", (m2Renderer_ ? "yes" : "null"),
                    " visualId=", visualId);
        return;
    }

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) { LOG_WARNING("SpellVisual: no AssetManager"); return; }

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Try precast path first, fall back to cast path
    auto pathIt = spellVisualPrecastPath_.find(visualId);
    if (pathIt == spellVisualPrecastPath_.end()) {
        // No precast kit - fall back to playing cast kit
        playSpellVisual(visualId, worldPosition, false, attachInstanceId);
        return;
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: precast path resolved to: ", modelPath);

    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: precast model in failed-cache, skipping: ", modelPath);
        return;
    }

    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            // Fall back to cast kit
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        LOG_INFO("SpellVisual: precast M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.name.empty()) model.name = modelPath;
        LOG_INFO("SpellVisual: precast M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size(),
                 " globalSeqs=", model.globalSequenceDurations.size(),
                 " sequences=", model.sequences.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        if (model.version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) {
                pipeline::M2Loader::loadSkin(skinData, model);
                LOG_INFO("SpellVisual: loaded skin, indices=", model.indices.size());
            }
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load precast model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false, attachInstanceId);
            return;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        LOG_INFO("SpellVisual: loaded precast model id=", modelId, " path=", modelPath);
    }

    // Determine attachment point for bone tracking (hand/chest/head → follow
    // the CASTER's bones; attachInstanceId=0 means a non-tracked caster, so
    // the effect stays static at their world position).
    uint32_t attachId = classifyAttachmentId(modelPath);
    if (attachInstanceId == 0) attachId = 0;
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: createInstance returned 0 for precast model=", modelPath);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);

    // Duration: prefer server cast time if available (long casts like Hearthstone=10s),
    // otherwise fall back to M2 animation duration, then default.
    float duration;
    if (castTimeMs >= 500) {
        // Server cast time available - precast should last the full cast duration
        duration = std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f);
    } else {
        float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        duration = (animDurMs > 100.0f)
            ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
            : SPELL_VISUAL_DEFAULT_DURATION;
    }
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    followUnitFromSpawn(spawnPos);
    LOG_INFO("SpellVisual: spawned precast visualId=", visualId, " instanceId=", instanceId,
             " duration=", duration, "s castTimeMs=", castTimeMs, " attach=", attachId,
             " model=", modelPath,
             " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = true, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId) {
    LOG_INFO("SpellVisual: playSpellVisual visualId=", visualId, " impact=", useImpactKit,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z, ")");
    if (!m2Renderer_ || visualId == 0) return;

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // The impact kit's sound plays with it, where it plays, model or none
    // (0x00745230 flags the kit to play its SoundID). For a missile that is
    // on arrival, as the kit is (0x00700e20).
    if (useImpactKit) {
        auto soundIt = impactKitSounds_.find(visualId);
        if (const LoadedSound* sound = soundIt != impactKitSounds_.end() ? soundEntry(soundIt->second) : nullptr)
            audio::AudioEngine::instance().playSound3D(sound->data, worldPosition, sound->volume);
    }

    // Select cast or impact path map; fall back to the other if missing
    auto& primaryMap = useImpactKit ? spellVisualImpactPath_ : spellVisualCastPath_;
    auto& fallbackMap = useImpactKit ? spellVisualCastPath_ : spellVisualImpactPath_;
    auto pathIt = primaryMap.find(visualId);
    if (pathIt == primaryMap.end()) {
        pathIt = fallbackMap.find(visualId);
        if (pathIt == fallbackMap.end()) {
            return;
        }
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: ", (useImpactKit ? "impact" : "cast"), " path resolved to: ", modelPath);

    const uint32_t modelId = acquireEffectModel(modelPath);
    if (modelId == 0) return;

    // Determine attachment point for bone tracking on cast effects. Only the
    // caster identified by attachInstanceId may be tracked - never default to
    // the local player (that glued every nearby unit's cast kit to the
    // player's hands).
    uint32_t attachId = 0;
    if (!useImpactKit && attachInstanceId != 0) {
        attachId = classifyAttachmentId(modelPath);
    }
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        if (charRenderer) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    // Spawn instance at world position
    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create instance for visualId=", visualId);
        return;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);
    // Determine lifetime from M2 animation duration (clamp to reasonable range)
    float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    float duration = (animDurMs > 100.0f)
        ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
        : SPELL_VISUAL_DEFAULT_DURATION;
    activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = attachId, .attachInstanceId = attachInstanceId});
    followUnitFromSpawn(spawnPos);
    LOG_INFO("SpellVisual: spawned ", (useImpactKit ? "impact" : "cast"), " visualId=", visualId,
             " instanceId=", instanceId, " duration=", duration, "s animDurMs=", animDurMs,
             " attach=", attachId, " model=", modelPath, " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the caster's left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(attachInstanceId, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            m2Renderer_->restartInstanceAnimation(leftId);
            activeSpellVisuals_.push_back({.instanceId = leftId, .elapsed = 0.0f, .duration = duration, .isPrecast = false, .attachmentId = 2 /* LeftHand */, .attachInstanceId = attachInstanceId});
        }
    }
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin) {
    if (!renderer_ || modelPath.empty()) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    const std::string cacheKey = modelPath + "|" + texturePath;
    uint32_t modelId = 0;
    auto cached = projectileModelIds_.find(cacheKey);
    if (cached != projectileModelIds_.end()) {
        modelId = cached->second;
    } else {
        if (nextProjectileModelId_ >= 999000) return;
        modelId = nextProjectileModelId_++;
        projectileModelIds_[cacheKey] = modelId;
    }

    if (!characterRenderer->getModelData(modelId)) {
        auto bytes = cachedAssetManager_->readFile(modelPath);
        if (bytes.empty()) return;
        pipeline::M2Model model = pipeline::M2Loader::load(bytes);
        if (model.name.empty()) model.name = modelPath;
        if (model.version >= 264) {
            const std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skin = cachedAssetManager_->readFile(skinPath);
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
        }
        if (model.vertices.empty() || !characterRenderer->loadModel(model, modelId)) return;
        if (!texturePath.empty()) {
            if (auto* texture = characterRenderer->loadTexture(texturePath))
                characterRenderer->setModelTexture(modelId, 0, texture);
        }
    }

    const glm::vec3 delta = end - start;
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(delta.y, delta.x);
    rotation.y = -std::atan2(delta.z, std::max(horizontal, 0.001f));

    const uint32_t instanceId = characterRenderer->createInstance(modelId, start, rotation, 1.0f);
    if (instanceId == 0) return;
    physicalProjectiles_.push_back({.instanceId = instanceId, .start = start, .end = end, .rotation = rotation, .elapsed = 0.0f,
                                    .duration = std::max(duration, 0.05f), .spin = spin});
}

const SpellVisualSystem::MissileVisual* SpellVisualSystem::findMissileVisual(uint32_t visualId) {
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = missileVisuals_.find(visualId);
    return it != missileVisuals_.end() ? &it->second : nullptr;
}

// Where the caster lets the missile go, as FUN_00720bf0 finds it. A visual
// naming MissileAttachment leaves from that attachment, carried by
// MissileCastOffset in its frame; one naming only an offset carries it from
// the directed spell hand (38, DAT_00ADAA50). Neither, and the client takes
// the cast model's $CSL, $CSR or $CST event position, in that order, then
// the unit's own position (FUN_0071a720). Those events sit in the spell
// hands, and WoWee does not read M2 events, so the hands stand in for them.
glm::vec3 SpellVisualSystem::missileSource(const MissileVisual& visual, const MissileEnd& from) const {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || from.renderInstanceId == 0) return from.position;
    glm::mat4 attachMat;
    const bool hasOffset = visual.castOffset != glm::vec3(0.0f);
    if (visual.sourceAttachment >= 0 || hasOffset) {
        const int32_t attachment = visual.sourceAttachment >= 0 ? visual.sourceAttachment : 38;
        if (charRenderer->getAttachmentTransform(from.renderInstanceId,
                                                 static_cast<uint32_t>(attachment), attachMat)) {
            return glm::vec3(attachMat * glm::vec4(visual.castOffset, 1.0f));
        }
    }
    for (uint32_t hand : {21u /* SpellLeftHand */, 22u /* SpellRightHand */}) {
        if (charRenderer->getAttachmentTransform(from.renderInstanceId, hand, attachMat))
            return glm::vec3(attachMat[3]);
    }
    return from.position;
}

// Where the missile aims this frame (FUN_006ff320): the target's attachment
// carrying the impact offset while the target exists, and once it has gone
// the last point it was seen at, which is where the missile then lands.
glm::vec3 SpellVisualSystem::missileTargetPoint(ActiveMissile& missile) const {
    if (missile.targetInstanceId == 0) return missile.lastTarget;
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::mat4 frame;
    const bool found = charRenderer &&
        (missile.targetAttachment >= 0
             ? charRenderer->getAttachmentTransform(missile.targetInstanceId,
                                                    static_cast<uint32_t>(missile.targetAttachment), frame)
             : charRenderer->getInstanceFrame(missile.targetInstanceId, frame));
    if (!found) {
        missile.targetInstanceId = 0;
        return missile.lastTarget;
    }
    missile.lastTarget = glm::vec3(frame * glm::vec4(missile.impactOffset, 1.0f));
    return missile.lastTarget;
}

bool SpellVisualSystem::launchSpellMissile(uint32_t visualId, float speed, const MissileEnd& from,
                                           const MissileEnd& to, std::vector<MissileEnd> impacts) {
    if (!m2Renderer_ || visualId == 0 || !(speed > 0.0f)) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return false;
    const MissileVisual* visual = findMissileVisual(visualId);
    if (!visual) return false;
    const uint32_t modelId = acquireEffectModel(visual->modelPath);
    if (modelId == 0) return false;

    ActiveMissile missile;
    missile.visualId = visualId;
    missile.speed = speed;
    missile.scale = visual->scale;
    missile.impacts = std::move(impacts);
    missile.impactOffset = visual->impactOffset;
    missile.position = missileSource(*visual, from);
    missile.lastTarget = to.position;

    // The attachment is settled once, at launch, as FUN_007022d0 settles it:
    // the one the visual names if the target's model has it, else the first
    // fallback it has, else none - the target's origin.
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::mat4 probe;
    if (charRenderer && to.renderInstanceId != 0 && charRenderer->getInstanceFrame(to.renderInstanceId, probe)) {
        missile.targetInstanceId = to.renderInstanceId;
        if (visual->destinationAttachment >= 0 &&
            charRenderer->getAttachmentTransform(to.renderInstanceId,
                                                 static_cast<uint32_t>(visual->destinationAttachment), probe)) {
            missile.targetAttachment = visual->destinationAttachment;
        } else {
            for (int32_t fallback : spell_missile::kDestinationFallbacks) {
                if (charRenderer->getAttachmentTransform(to.renderInstanceId,
                                                         static_cast<uint32_t>(fallback), probe)) {
                    missile.targetAttachment = fallback;
                    break;
                }
            }
        }
    }
    const glm::vec3 target = missileTargetPoint(missile);
    const float flight = spell_missile::flightTime(glm::length(target - missile.position), speed);
    // A missile chasing a unit that runs from it takes longer than this, so
    // the cap is generous; it only stops one that can never arrive.
    missile.maxLifetime = flight * 3.0f + 5.0f;

    missile.instanceId = m2Renderer_->createInstance(
        modelId, missile.position, spell_missile::facingEuler(target - missile.position), missile.scale);
    if (missile.instanceId == 0) return false;
    m2Renderer_->restartInstanceAnimation(missile.instanceId);
    missile.soundHandle = startMissileSound(visual->soundId, missile.position);
    activeMissiles_.push_back(missile);
    LOG_DEBUG("SpellVisual: missile visualId=", visualId, " speed=", speed, " flight=", flight,
              "s model=", visual->modelPath, " target attach=", missile.targetAttachment);
    return true;
}

// A SoundEntries row as a spell's sounds play it: the row's file at its
// VolumeFloat. Null when it has none this install can read.
const SpellVisualSystem::LoadedSound* SpellVisualSystem::soundEntry(uint32_t soundId) {
    if (soundId == 0 || !cachedAssetManager_) return nullptr;
    auto it = soundEntries_.find(soundId);
    if (it == soundEntries_.end()) {
        LoadedSound loaded;
        auto dbc = cachedAssetManager_->loadDBC("SoundEntries.dbc");
        const int32_t idx = dbc && dbc->isLoaded() ? dbc->findRecordById(soundId) : -1;
        // 3.3.5a: 3..12 File, 23 DirectoryBase, 24 VolumeFloat (0x008b69c0).
        if (idx >= 0 && dbc->getFieldCount() > 24) {
            const uint32_t row = static_cast<uint32_t>(idx);
            std::array<std::string, 10> files;
            for (uint32_t f = 0; f < files.size(); ++f) files[f] = dbc->getString(row, 3 + f);
            const std::string path = spell_missile::soundEntryFile(dbc->getString(row, 23), files);
            if (!path.empty()) loaded.data = cachedAssetManager_->readFile(path);
            const float volume = dbc->getFloat(row, 24);
            if (volume > 0.0f) loaded.volume = volume;
        }
        // Remembered either way, so a row without a file is read once.
        it = soundEntries_.emplace(soundId, std::move(loaded)).first;
    }
    return it->second.data.empty() ? nullptr : &it->second;
}

// The missile's sound (FUN_007022d0): looping, at the missile.
uint32_t SpellVisualSystem::startMissileSound(uint32_t soundId, const glm::vec3& position) {
    const LoadedSound* sound = soundEntry(soundId);
    if (!sound) return 0;
    return audio::AudioEngine::instance().playSound3DLooping(sound->data, position, sound->volume);
}

// One frame of every missile in flight (FUN_007015d0): home on the target,
// face along the way, and on arrival take the missile away and play the
// impact kit there (FUN_00703410, FUN_00700e20).
void SpellVisualSystem::updateMissiles(float deltaTime) {
    if (activeMissiles_.empty() || !m2Renderer_) return;
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    for (auto it = activeMissiles_.begin(); it != activeMissiles_.end(); ) {
        it->elapsed += deltaTime;
        const glm::vec3 target = missileTargetPoint(*it);
        const glm::vec3 travel = target - it->position;
        const spell_missile::Step step = spell_missile::advance(it->position, target, it->speed, deltaTime);
        if (!step.arrived && it->elapsed < it->maxLifetime) {
            it->position = step.position;
            m2Renderer_->setInstanceTransform(
                it->instanceId,
                placementModelMatrix(it->position, spell_missile::facingEuler(travel), it->scale));
            audio::AudioEngine::instance().setSoundPosition(it->soundHandle, it->position);
            ++it;
            continue;
        }
        m2Renderer_->removeInstance(it->instanceId);
        audio::AudioEngine::instance().stopSoundWithFade(it->soundHandle, spell_missile::kMissileSoundFadeSeconds);
        const uint32_t visualId = it->visualId;
        std::vector<MissileEnd> impacts = std::move(it->impacts);
        it = activeMissiles_.erase(it);
        // Impact kits are placed from a unit's feet like every other impact
        // here (the kit's own model carries its height), so a unit still
        // standing hands over its origin; one that has gone, where it was.
        for (const MissileEnd& impact : impacts) {
            glm::vec3 impactPos = impact.position;
            uint32_t impactInstance = 0;
            glm::mat4 unitFrame;
            if (impact.renderInstanceId != 0 && charRenderer &&
                charRenderer->getInstanceFrame(impact.renderInstanceId, unitFrame)) {
                impactPos = glm::vec3(unitFrame[3]);
                impactInstance = impact.renderInstanceId;
            }
            playSpellVisual(visualId, impactPos, /*useImpactKit=*/true, impactInstance);
        }
    }
}

void SpellVisualSystem::followUnitFromSpawn(const glm::vec3& spawnPos) {
    if (activeSpellVisuals_.empty() || !renderer_) return;
    auto& added = activeSpellVisuals_.back();
    if (added.attachmentId != 0 || added.attachInstanceId == 0) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    glm::mat4 unitFrame;
    if (!charRenderer || !charRenderer->getInstanceFrame(added.attachInstanceId, unitFrame)) return;
    added.followsUnit = true;
    added.followOffset = glm::vec3(glm::inverse(unitFrame) * glm::vec4(spawnPos, 1.0f));
}

void SpellVisualSystem::update(float deltaTime) {
    // First: an arrival plays its impact kit, which joins activeSpellVisuals_.
    updateMissiles(deltaTime);
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty()) return;

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // An effect rides on its unit's whole transform, turning with it:
            // the attachment point's for a hand, chest or head effect - the
            // CASTER's, not the local player's - and the unit's own otherwise.
            if (it->attachmentId != 0 && it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(it->attachInstanceId, it->attachmentId, attachMat)) {
                    m2Renderer_->setInstanceTransform(it->instanceId, attachMat);
                }
            } else if (it->followsUnit && charRenderer) {
                glm::mat4 unitFrame;
                if (charRenderer->getInstanceFrame(it->attachInstanceId, unitFrame)) {
                    m2Renderer_->setInstanceTransform(
                        it->instanceId, glm::translate(unitFrame, it->followOffset));
                }
            }
            ++it;
        }
    }


    if (charRenderer) {
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
            const float t = std::min(it->elapsed / it->duration, 1.0f);
            charRenderer->setInstancePosition(
                it->instanceId, glm::mix(it->start, it->end, t));
            if (it->spin) {
                glm::vec3 rotation = it->rotation;
                rotation.x += t * glm::two_pi<float>() * 2.0f;
                charRenderer->setInstanceRotation(it->instanceId, rotation);
            }
            if (t >= 1.0f) {
                charRenderer->removeInstance(it->instanceId);
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    for (const auto& missile : activeMissiles_) {
        if (m2Renderer_) m2Renderer_->removeInstance(missile.instanceId);
        audio::AudioEngine::instance().stopSound(missile.soundHandle);
    }
    activeMissiles_.clear();
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& projectile : physicalProjectiles_)
            renderer_->getCharacterRenderer()->removeInstance(projectile.instanceId);
    }
    physicalProjectiles_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee

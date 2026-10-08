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
#include "core/weapon_attachment.hpp"
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
    // MinAllowedScale and MaxAllowedScale (+0x14, +0x18), which 0x006f8c50
    // keeps an attached effect within.
    const uint32_t fxMinScaleField = fxLayout ? fxLayout->tryField("MinAllowedScale") : 0xFFFFFFFFu;
    const uint32_t fxMaxScaleField = fxLayout ? fxLayout->tryField("MaxAllowedScale") : 0xFFFFFFFFu;
    std::unordered_map<uint32_t, std::pair<float, float>> effectAllowedScales;
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
            if (fxMinScaleField < fxDbc->getFieldCount() && fxMaxScaleField < fxDbc->getFieldCount())
                effectAllowedScales[id] = {fxDbc->getFloat(i, fxMinScaleField), fxDbc->getFloat(i, fxMaxScaleField)};
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

    // A kit's LeftWeaponEffect and RightWeaponEffect (+0x24, +0x28), which
    // 0x0073a6c0 hangs in the hands.
    std::unordered_map<uint32_t, std::vector<KitWeaponEffect>> kitWeaponEffects;  // kitId → its
    if (kitDbc && kitDbc->isLoaded() && kitLayout) {
        const uint32_t leftField = kitLayout->tryField("LeftWeaponEffect");
        const uint32_t rightField = kitLayout->tryField("RightWeaponEffect");
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            for (const auto& [field, left] : {std::pair{leftField, true}, std::pair{rightField, false}}) {
                if (field >= kitDbc->getFieldCount()) continue;
                const uint32_t effect = kitDbc->getUInt32(i, field);
                auto pathIt = effect ? effectPaths.find(effect) : effectPaths.end();
                if (pathIt == effectPaths.end()) continue;
                KitWeaponEffect fx{.modelPath = pathIt->second, .left = left};
                if (auto it = effectScales.find(effect); it != effectScales.end()) fx.scale = it->second;
                if (auto it = effectAllowedScales.find(effect); it != effectAllowedScales.end()) {
                    fx.minScale = it->second.first;
                    fx.maxScale = it->second.second;
                }
                kitWeaponEffects[kitDbc->getUInt32(i, 0)].push_back(std::move(fx));
            }
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

    // Each kit's models as 0x00745230 hangs them on a unit: the model
    // columns at their attachments, the WorldEffect (+0x38) in the world,
    // and its SpellVisualKitModelAttach rows (0x007fa9f0, 0x007faa20).
    auto kitModelFor = [&](uint32_t effect, int32_t attachment) -> std::optional<KitModel> {
        auto pathIt = effect ? effectPaths.find(effect) : effectPaths.end();
        if (pathIt == effectPaths.end()) return std::nullopt;
        KitModel model{.path = pathIt->second, .attachment = attachment};
        if (auto it = effectScales.find(effect); it != effectScales.end()) model.scale = it->second;
        if (auto it = effectAllowedScales.find(effect); it != effectAllowedScales.end()) {
            model.minScale = it->second.first;
            model.maxScale = it->second.second;
        }
        return model;
    };
    if (kitDbc && kitDbc->isLoaded()) {
        const uint32_t fc = kitDbc->getFieldCount();
        auto kitColumn = [&](const char* name, uint32_t fallback) {
            const uint32_t f = kitLayout ? kitLayout->tryField(name) : fallback;
            return f < fc ? f : 0xFFFFFFFFu;
        };
        std::array<uint32_t, spell_kit::kKitSlots.size()> slotFields{};
        for (size_t k = 0; k < slotFields.size(); ++k)
            slotFields[k] = kitColumn(spell_kit::kKitSlots[k].column, spell_kit::kKitSlots[k].fallbackField);
        const uint32_t worldField = kitColumn("WorldEffect", 14);
        const uint32_t kitFlagsField = kitColumn("Flags", 37);
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            const uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            KitRecord kit;
            for (size_t k = 0; k < slotFields.size(); ++k) {
                if (slotFields[k] == 0xFFFFFFFFu) continue;
                if (auto model = kitModelFor(kitDbc->getUInt32(i, slotFields[k]),
                                             static_cast<int32_t>(spell_kit::kKitSlots[k].attachment)))
                    kit.models.push_back(std::move(*model));
            }
            if (worldField != 0xFFFFFFFFu) {
                if (auto model = kitModelFor(kitDbc->getUInt32(i, worldField), -1))
                    kit.models.push_back(std::move(*model));
            }
            if (kitFlagsField != 0xFFFFFFFFu) kit.flags = kitDbc->getUInt32(i, kitFlagsField);
            if (!kit.models.empty() || kit.flags != 0) kits_[kitId] = std::move(kit);
        }
    }
    const pipeline::DBCFieldMap* attachLayout = layout ? layout->getLayout("SpellVisualKitModelAttach") : nullptr;
    if (auto attachDbc = attachLayout ? cachedAssetManager_->loadDBCOptional("SpellVisualKitModelAttach.dbc") : nullptr;
        attachDbc && attachDbc->isLoaded()) {
        const uint32_t fc = attachDbc->getFieldCount();
        auto col = [&](const char* name) {
            const uint32_t f = attachLayout->tryField(name);
            return f < fc ? f : 0xFFFFFFFFu;
        };
        const uint32_t parentF = col("ParentSpellVisualKitID"), effectF = col("SpellVisualEffectNameID"),
                       attachF = col("AttachmentID"), xF = col("OffsetX"), yF = col("OffsetY"), zF = col("OffsetZ"),
                       yawF = col("Yaw"), pitchF = col("Pitch"), rollF = col("Roll");
        if (parentF != 0xFFFFFFFFu && effectF != 0xFFFFFFFFu && attachF != 0xFFFFFFFFu && rollF != 0xFFFFFFFFu &&
            xF != 0xFFFFFFFFu && yF != 0xFFFFFFFFu && zF != 0xFFFFFFFFu && yawF != 0xFFFFFFFFu &&
            pitchF != 0xFFFFFFFFu) {
            for (uint32_t i = 0; i < attachDbc->getRecordCount(); ++i) {
                auto model = kitModelFor(attachDbc->getUInt32(i, effectF), attachDbc->getInt32(i, attachF));
                if (!model) continue;
                model->local = spell_kit::modelAttachMatrix(
                    glm::vec3(attachDbc->getFloat(i, xF), attachDbc->getFloat(i, yF), attachDbc->getFloat(i, zF)),
                    attachDbc->getFloat(i, yawF), attachDbc->getFloat(i, pitchF), attachDbc->getFloat(i, rollF));
                kits_[attachDbc->getUInt32(i, parentF)].models.push_back(std::move(*model));
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
    const uint32_t svStateKitField     = svColumn("StateKit");
    const uint32_t svStateDoneKitField = svColumn("StateDoneKit");
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

        // The aura kits: StateKit and StateDoneKit, and the Flags that
        // keep a state kit to an unarmed, idle unit.
        {
            VisualAuraKits auraKits;
            auraKits.stateKit = static_cast<uint32_t>(std::max(svInt(i, svStateKitField, 0), 0));
            auraKits.stateDoneKit = static_cast<uint32_t>(std::max(svInt(i, svStateDoneKitField, 0), 0));
            auraKits.flags = static_cast<uint32_t>(svInt(i, svFlagsField, 0));
            if (kits_.count(auraKits.stateKit) == 0) auraKits.stateKit = 0;
            if (kits_.count(auraKits.stateDoneKit) == 0) auraKits.stateDoneKit = 0;
            if (auraKits.stateKit != 0 || auraKits.stateDoneKit != 0) visualAuraKits_[vid] = auraKits;
        }

        // The precast and cast kits' weapon effects.
        if (svPrecastKitField < svFc) {
            auto fxIt = kitWeaponEffects.find(svDbc->getUInt32(i, svPrecastKitField));
            if (fxIt != kitWeaponEffects.end()) precastWeaponEffects_[vid] = fxIt->second;
        }
        if (svCastKitField < svFc) {
            auto fxIt = kitWeaponEffects.find(svDbc->getUInt32(i, svCastKitField));
            if (fxIt != kitWeaponEffects.end()) castWeaponEffects_[vid] = fxIt->second;
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

    // The precast kit's weapon effects, for as long as it plays.
    if (auto fxIt = precastWeaponEffects_.find(visualId); fxIt != precastWeaponEffects_.end()) {
        playKitWeaponEffects(fxIt->second, attachInstanceId, true, castTimeMs);
    }

    // The precast kit's model. A visual without one shows nothing more at
    // the cast's start: the cast kit plays at SMSG_SPELL_GO and only then
    // (0x007fa2e0 plays SpellVisual +4 alone; 0x00809f80's 0x00800dd0 the
    // cast kit, +8).
    auto pathIt = spellVisualPrecastPath_.find(visualId);
    if (pathIt == spellVisualPrecastPath_.end()) return;

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

    // The cast kit's weapon effects.
    if (!useImpactKit) {
        if (auto fxIt = castWeaponEffects_.find(visualId); fxIt != castWeaponEffects_.end()) {
            playKitWeaponEffects(fxIt->second, attachInstanceId, false, 0);
        }
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

void SpellVisualSystem::playKitWeaponEffects(const std::vector<KitWeaponEffect>& effects,
                                             uint32_t attachInstanceId, bool isPrecast, uint32_t castTimeMs) {
    // 0x0073a6c0: only on a unit, and one whose model holds them
    // (CreatureModelData +4 without 0x10).
    if (effects.empty() || attachInstanceId == 0 || !renderer_ || !m2Renderer_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    const std::optional<float> attachedEffectScale =
        weaponEffectHolder_ ? weaponEffectHolder_(attachInstanceId) : std::optional<float>(1.0f);
    if (!attachedEffectScale) return;
    for (const KitWeaponEffect& fx : effects) {
        const uint32_t attachId = core::kitWeaponEffectAttachment(fx.left);
        glm::mat4 attachMat;
        if (!charRenderer->getAttachmentTransform(attachInstanceId, attachId, attachMat)) continue;
        const uint32_t modelId = acquireEffectModel(fx.modelPath);
        if (modelId == 0) continue;
        // 0x006f8c50: the model's scale, held within the effect's allowed
        // scales as the hand's scale makes it.
        const float scale = core::kitEffectScale(*attachedEffectScale, fx.scale, glm::length(glm::vec3(attachMat[0])),
                                                 fx.minScale, fx.maxScale);
        const uint32_t instanceId = m2Renderer_->createInstance(modelId, glm::vec3(attachMat[3]), glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) continue;
        m2Renderer_->restartInstanceAnimation(instanceId);
        m2Renderer_->setInstanceTransform(instanceId, attachMat * glm::scale(glm::mat4(1.0f), glm::vec3(scale)));
        // With its kit: a precast kit's for the cast, a cast kit's as its
        // model plays.
        float duration;
        const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        if (isPrecast && castTimeMs >= 500) {
            duration = std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f);
        } else {
            duration = animDurMs > 100.0f ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
                                          : SPELL_VISUAL_DEFAULT_DURATION;
        }
        activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration,
                                       .isPrecast = isPrecast, .attachmentId = attachId,
                                       .attachInstanceId = attachInstanceId, .scale = scale});
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
                                           const MissileEnd& to, std::vector<MissileEnd> impacts,
                                           const MissileTrajectory* trajectory) {
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
    float flight = spell_missile::flightTime(glm::length(target - missile.position), speed);
    // ADJUST_MISSILE (0x007022d0 hands it to 0x00700880): one flight to where
    // the target is now, timed by the server. Off the arc it flies straight,
    // homing, at the speed that lands it then.
    if (trajectory) {
        missile.adjusted = true;
        missile.arcStart = missile.position;
        missile.arc = spell_missile::planMissileArc(trajectory->elevation, 0.0f, target - missile.position,
                                                    spellMissileRow(trajectory->spellMissileId),
                                                    trajectory->flightSeconds);
        if (!missile.arc.onArc) missile.speed = missile.arc.straightSpeed;
        flight = missile.arc.flightSeconds;
    }
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

// SpellMissile.dbc, all fifteen columns 4 bytes (0x008b8310): the arc reads
// the two speeds at 4 and 5 and the gravity at 12 (0x00700880).
const spell_missile::SpellMissileRow* SpellVisualSystem::spellMissileRow(uint32_t id) {
    if (!spellMissileDbcLoaded_) {
        spellMissileDbcLoaded_ = true;
        if (!cachedAssetManager_)
            cachedAssetManager_ = core::Application::getInstance().getAssetManager();
        auto dbc = cachedAssetManager_ ? cachedAssetManager_->loadDBC("SpellMissile.dbc") : nullptr;
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 15) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                spell_missile::SpellMissileRow row;
                row.speeds[0] = dbc->getFloat(i, 4);
                row.speeds[1] = dbc->getFloat(i, 5);
                row.gravity = dbc->getFloat(i, 12);
                spellMissileRows_[dbc->getUInt32(i, 0)] = row;
            }
        }
    }
    if (id == 0) return nullptr;
    auto it = spellMissileRows_.find(id);
    return it != spellMissileRows_.end() ? &it->second : nullptr;
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
        spell_missile::Step step;
        if (it->adjusted && it->arc.onArc) {
            // On its arc it lands when its time is up, wherever the target
            // has gone (0x007015d0, flag 0x10000).
            step.arrived = it->elapsed >= it->arc.flightSeconds;
            step.position = spell_missile::arcPosition(it->arcStart, it->arc, it->elapsed);
        } else {
            step = spell_missile::advance(it->position, target, it->speed, deltaTime);
        }
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
        // 0x00700e20: a state kit held back for the missile plays as it lands.
        for (auto& [guid, unit] : unitAuraKits_) {
            const uint32_t unitInstance = unitInstanceResolver_ ? unitInstanceResolver_(guid) : 0;
            if (unitInstance == 0) continue;
            for (const MissileEnd& impact : impacts) {
                if (impact.renderInstanceId != unitInstance) continue;
                for (AuraKit& aura : unit.auras) {
                    if (aura.visualId == visualId) aura.awaitingMissile = false;
                }
            }
        }
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

bool SpellVisualSystem::kitModelTransform(uint32_t renderInstanceId, int32_t attachment, const glm::mat4& local,
                                          float effectScale, float minScale, float maxScale, glm::mat4& out,
                                          float& scale) {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || renderInstanceId == 0) return false;
    if (attachment < 0) {
        // In the world where the unit stands, turned as it faces.
        if (!charRenderer->getInstanceFrame(renderInstanceId, out)) return false;
        out = out * local;
        scale = 1.0f;
        return true;
    }
    glm::mat4 attachMat;
    if (!charRenderer->getAttachmentTransform(renderInstanceId, static_cast<uint32_t>(attachment), attachMat))
        return false;
    // 0x006f8c50: AttachedEffectScale times the effect's Scale, within its
    // allowed scales as the attachment's own scale shows it.
    const float attached = attachedEffectScale_ ? attachedEffectScale_(renderInstanceId) : 1.0f;
    scale = core::kitEffectScale(attached, effectScale, glm::length(glm::vec3(attachMat[0])), minScale, maxScale);
    out = attachMat * local * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    return true;
}

std::vector<SpellVisualSystem::KitModelInstance> SpellVisualSystem::playKitModels(const KitRecord& kit,
                                                                                 uint32_t renderInstanceId,
                                                                                 bool loops) {
    std::vector<KitModelInstance> shown;
    if (!m2Renderer_ || renderInstanceId == 0) return shown;
    for (const KitModel& model : kit.models) {
        glm::mat4 transform;
        float scale = 1.0f;
        if (!kitModelTransform(renderInstanceId, model.attachment, model.local, model.scale, model.minScale,
                               model.maxScale, transform, scale))
            continue;
        const uint32_t modelId = acquireEffectModel(model.path);
        if (modelId == 0) continue;
        const uint32_t instanceId = m2Renderer_->createInstance(modelId, glm::vec3(transform[3]), glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) continue;
        m2Renderer_->restartInstanceAnimation(instanceId);
        m2Renderer_->setInstanceTransform(instanceId, transform);
        if (loops) {
            // 0x007449c0: a state kit's model holds its Hold (158), or its
            // Stand where the kit has Flags 0x20 or the model no Hold.
            if ((kit.flags & spell_kit::kKitFlagStateStand) == 0 &&
                m2Renderer_->hasAnimation(instanceId, spell_kit::kAnimHold))
                m2Renderer_->setInstanceAnimation(instanceId, spell_kit::kAnimHold, true);
            shown.push_back({.instanceId = instanceId, .attachment = model.attachment, .local = model.local,
                             .scale = scale});
            continue;
        }
        // Once (0x00744870): its Decay where it has one, else as long as its
        // animation runs.
        if (m2Renderer_->hasAnimation(instanceId, spell_kit::kAnimDecay))
            m2Renderer_->setInstanceAnimation(instanceId, spell_kit::kAnimDecay, false);
        const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        const float duration = animDurMs > 100.0f ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
                                                  : SPELL_VISUAL_DEFAULT_DURATION;
        // A model in the world stays where it was put; one on an attachment
        // rides it.
        const bool attached = model.attachment >= 0;
        activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .duration = duration,
                                       .isPrecast = false,
                                       .attachmentId = attached ? static_cast<uint32_t>(model.attachment) : 0u,
                                       .attachInstanceId = attached ? renderInstanceId : 0u,
                                       .scale = scale, .local = model.local});
    }
    return shown;
}

void SpellVisualSystem::hideAuraKit(AuraKit& aura) {
    if (m2Renderer_) {
        for (const KitModelInstance& model : aura.models) m2Renderer_->removeInstance(model.instanceId);
    }
    aura.models.clear();
    aura.boundInstance = 0;
}

void SpellVisualSystem::setUnitAuraSlot(uint64_t unitGuid, uint32_t slot, uint32_t spellId) {
    if (unitGuid == 0) return;
    auto& slots = unitAuraKits_[unitGuid].slots;
    auto it = slots.find(slot);
    const uint32_t old = it != slots.end() ? it->second : 0;
    if (old == spellId) return;
    if (spellId != 0) slots[slot] = spellId;
    else slots.erase(slot);
    auto visualOf = [&](uint32_t spell) { return spellVisualResolver_ ? spellVisualResolver_(spell) : 0u; };
    if (old != 0) removeAuraStateKit(unitGuid, old, visualOf(old));
    if (spellId != 0) applyAuraStateKit(unitGuid, spellId, visualOf(spellId));
}

void SpellVisualSystem::setUnitAuraSlots(uint64_t unitGuid,
                                         const std::vector<std::pair<uint32_t, uint32_t>>& slotSpells) {
    if (unitGuid == 0) return;
    std::vector<uint32_t> emptied;
    if (auto it = unitAuraKits_.find(unitGuid); it != unitAuraKits_.end()) {
        for (const auto& [slot, spell] : it->second.slots) {
            const bool named = std::any_of(slotSpells.begin(), slotSpells.end(),
                                           [slot = slot](const auto& entry) { return entry.first == slot; });
            if (!named) emptied.push_back(slot);
        }
    }
    for (uint32_t slot : emptied) setUnitAuraSlot(unitGuid, slot, 0);
    for (const auto& [slot, spell] : slotSpells) setUnitAuraSlot(unitGuid, slot, spell);
}

void SpellVisualSystem::applyAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId) {
    if (unitGuid == 0 || spellId == 0 || visualId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto visualIt = visualAuraKits_.find(visualId);
    if (visualIt == visualAuraKits_.end() || visualIt->second.stateKit == 0) return;
    const VisualAuraKits& visual = visualIt->second;
    UnitAuraKits& unit = unitAuraKits_[unitGuid];
    AuraKit aura{.spellId = spellId, .visualId = visualId, .kitId = visual.stateKit,
                 .unarmedOnly = (visual.flags & spell_kit::kVisualFlagUnarmedStateKit) != 0};
    // 0x00724820: not while a missile carrying the spell is still on its way
    // to the unit - the missile plays the kit as it lands (0x00700e20).
    const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(unitGuid) : 0;
    if (instance != 0) {
        for (const ActiveMissile& missile : activeMissiles_) {
            if (missile.visualId != visualId) continue;
            for (const MissileEnd& end : missile.impacts) {
                if (end.renderInstanceId == instance) aura.awaitingMissile = true;
            }
        }
    }
    unit.auras.push_back(std::move(aura));
    // Then 0x00720400(1, 1) for a Flags 8 visual.
    if (unit.auras.back().unarmedOnly) stepUnarmedKits(unitGuid, unit, true);
}

void SpellVisualSystem::removeAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId) {
    if (unitGuid == 0 || spellId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    // 0x00743b40: every one of the spell's kits leaves the unit.
    if (auto it = unitAuraKits_.find(unitGuid); it != unitAuraKits_.end()) {
        auto& auras = it->second.auras;
        for (auto a = auras.begin(); a != auras.end();) {
            if (a->spellId == spellId) {
                hideAuraKit(*a);
                a = auras.erase(a);
            } else {
                ++a;
            }
        }
    }
    // Then the visual's StateDoneKit, once (kit type 8).
    auto visualIt = visualId ? visualAuraKits_.find(visualId) : visualAuraKits_.end();
    if (visualIt == visualAuraKits_.end() || visualIt->second.stateDoneKit == 0) return;
    auto kitIt = kits_.find(visualIt->second.stateDoneKit);
    const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(unitGuid) : 0;
    if (kitIt != kits_.end() && instance != 0) playKitModels(kitIt->second, instance, false);
}

void SpellVisualSystem::stepUnarmedKits(uint64_t unitGuid, UnitAuraKits& unit, bool force) {
    bool hasSuchAura = false;
    for (const AuraKit& aura : unit.auras) hasSuchAura = hasSuchAura || aura.unarmedOnly;
    const bool show = unarmedKitsQuery_ ? unarmedKitsQuery_(unitGuid) : true;
    switch (spell_kit::unarmedKitStep(unit.unarmedBits, show, force, hasSuchAura)) {
        case spell_kit::UnarmedKitStep::HideAll:
            for (AuraKit& aura : unit.auras) {
                if (!aura.unarmedOnly) continue;
                aura.playing = false;
                hideAuraKit(aura);
            }
            break;
        case spell_kit::UnarmedKitStep::ShowFirst:
            for (AuraKit& aura : unit.auras) {
                if (!aura.unarmedOnly) continue;
                aura.playing = true;
                break;
            }
            break;
        case spell_kit::UnarmedKitStep::None:
            break;
    }
}

void SpellVisualSystem::updateAuraKits() {
    if (unitAuraKits_.empty() || !m2Renderer_) return;
    for (auto unitIt = unitAuraKits_.begin(); unitIt != unitAuraKits_.end();) {
        UnitAuraKits& unit = unitIt->second;
        if (unit.auras.empty()) {
            if (unit.slots.empty()) unitIt = unitAuraKits_.erase(unitIt);
            else ++unitIt;
            continue;
        }
        stepUnarmedKits(unitIt->first, unit, false);
        const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(unitIt->first) : 0;
        for (AuraKit& aura : unit.auras) {
            if (!aura.playing || aura.awaitingMissile) {
                if (!aura.models.empty()) hideAuraKit(aura);
                continue;
            }
            // Onto the unit's model as it now is: the kit is the unit's,
            // whatever model draws it.
            if (aura.boundInstance != instance) {
                hideAuraKit(aura);
                auto kitIt = kits_.find(aura.kitId);
                if (instance != 0 && kitIt != kits_.end()) {
                    aura.models = playKitModels(kitIt->second, instance, true);
                    aura.boundInstance = instance;
                }
                continue;
            }
            CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
            if (!charRenderer) continue;
            for (const KitModelInstance& model : aura.models) {
                if (model.attachment < 0) continue;  // in the world, where it was put
                glm::mat4 attachMat;
                if (!charRenderer->getAttachmentTransform(instance, static_cast<uint32_t>(model.attachment), attachMat))
                    continue;
                // The scale it was hung with; the place it rides.
                m2Renderer_->setInstanceTransform(
                    model.instanceId, attachMat * model.local * glm::scale(glm::mat4(1.0f), glm::vec3(model.scale)));
            }
        }
        ++unitIt;
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
    updateAuraKits();
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
                    attachMat = attachMat * it->local;
                    if (it->scale != 1.0f) attachMat = attachMat * glm::scale(glm::mat4(1.0f), glm::vec3(it->scale));
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
    for (auto& [guid, unit] : unitAuraKits_) {
        for (AuraKit& aura : unit.auras) hideAuraKit(aura);
    }
    unitAuraKits_.clear();
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

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
// to build each visual's kits and missile.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first.
    if (!cachedAssetManager_ || !cachedAssetManager_->isInitialized()) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Load SpellVisualEffectName.dbc - ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    // Scale (+0x10), which sizes a missile (FUN_00732ff0) and every kit
    // model (0x006f8c50, 0x006f8ae0).
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

    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");

    // Each kit's models as 0x00745230 hangs them on a unit: the model
    // columns at their attachments, the WorldEffect (+0x38) in the world,
    // and its SpellVisualKitModelAttach rows (0x007fa9f0, 0x007faa20).
    auto kitModelFor = [&](uint32_t effect, int32_t attachment,
                           spell_kit::KitModelKind kind) -> std::optional<KitModel> {
        auto pathIt = effect ? effectPaths.find(effect) : effectPaths.end();
        if (pathIt == effectPaths.end()) return std::nullopt;
        KitModel model{.path = pathIt->second, .attachment = attachment, .kind = kind};
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
        const uint32_t kitShakeField = kitColumn("ShakeID", 16);
        // SoundID (+0x3c), which 0x00745230 plays with the kit.
        const uint32_t kitSoundField = kitColumn("SoundID", 15);
        // LeftWeaponEffect and RightWeaponEffect (+0x24, +0x28), which
        // 0x0073a6c0 hangs in the hands.
        const uint32_t leftWeaponField = kitColumn("LeftWeaponEffect", 9);
        const uint32_t rightWeaponField = kitColumn("RightWeaponEffect", 10);
        std::array<uint32_t, 4> procFields{};
        std::array<std::array<uint32_t, 4>, 4> paramFields{};  // [proc][param]
        static constexpr const char* kParamNames[4] = {"CharParamZero", "CharParamOne", "CharParamTwo",
                                                       "CharParamThree"};
        for (uint32_t k = 0; k < 4; ++k) {
            procFields[k] = kitColumn(("CharProc" + std::to_string(k)).c_str(), 17 + k);
            for (uint32_t p = 0; p < 4; ++p)
                paramFields[k][p] = kitColumn((kParamNames[p] + std::to_string(k)).c_str(), 21 + p * 4 + k);
        }
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            const uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            KitRecord kit;
            for (size_t k = 0; k < slotFields.size(); ++k) {
                if (slotFields[k] == 0xFFFFFFFFu) continue;
                const auto& slot = spell_kit::kKitSlots[k];
                const auto kind = slot.attachment == 20   ? spell_kit::KitModelKind::Head
                                  : slot.attachment == 19 ? spell_kit::KitModelKind::Base
                                                          : spell_kit::KitModelKind::Column;
                if (auto model = kitModelFor(kitDbc->getUInt32(i, slotFields[k]),
                                             static_cast<int32_t>(slot.attachment), kind))
                    kit.models.push_back(std::move(*model));
            }
            if (worldField != 0xFFFFFFFFu) {
                if (auto model = kitModelFor(kitDbc->getUInt32(i, worldField), -1, spell_kit::KitModelKind::World))
                    kit.models.push_back(std::move(*model));
            }
            for (const auto& [field, left] : {std::pair{leftWeaponField, true}, std::pair{rightWeaponField, false}}) {
                if (field == 0xFFFFFFFFu) continue;
                const uint32_t effect = kitDbc->getUInt32(i, field);
                auto pathIt = effect ? effectPaths.find(effect) : effectPaths.end();
                if (pathIt == effectPaths.end()) continue;
                KitWeaponEffect fx{.modelPath = pathIt->second, .left = left};
                if (auto it = effectScales.find(effect); it != effectScales.end()) fx.scale = it->second;
                if (auto it = effectAllowedScales.find(effect); it != effectAllowedScales.end()) {
                    fx.minScale = it->second.first;
                    fx.maxScale = it->second.second;
                }
                kit.weaponEffects.push_back(std::move(fx));
            }
            if (kitSoundField != 0xFFFFFFFFu)
                kit.soundId = static_cast<uint32_t>(std::max(kitDbc->getInt32(i, kitSoundField), 0));
            if (kitFlagsField != 0xFFFFFFFFu) kit.flags = kitDbc->getUInt32(i, kitFlagsField);
            if (kitShakeField != 0xFFFFFFFFu)
                kit.shakeId = static_cast<uint32_t>(std::max(kitDbc->getInt32(i, kitShakeField), 0));
            bool hasProc = false;
            for (uint32_t k = 0; k < 4; ++k) {
                if (procFields[k] == 0xFFFFFFFFu) continue;
                kit.charProc[k] = kitDbc->getUInt32(i, procFields[k]);
                for (uint32_t p = 0; p < 4; ++p) {
                    if (paramFields[k][p] != 0xFFFFFFFFu) kit.charParam[k][p] = kitDbc->getFloat(i, paramFields[k][p]);
                }
                hasProc = hasProc || kit.charProc[k] <= 0x11u;
            }
            if (!kit.models.empty() || !kit.weaponEffects.empty() || kit.soundId != 0 || kit.flags != 0 ||
                kit.shakeId != 0 || hasProc)
                kits_[kitId] = std::move(kit);
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
                auto model = kitModelFor(attachDbc->getUInt32(i, effectF), attachDbc->getInt32(i, attachF),
                                         spell_kit::KitModelKind::AttachRow);
                if (!model) continue;
                model->local = spell_kit::modelAttachMatrix(
                    glm::vec3(attachDbc->getFloat(i, xF), attachDbc->getFloat(i, yF), attachDbc->getFloat(i, zF)),
                    attachDbc->getFloat(i, yawF), attachDbc->getFloat(i, pitchF), attachDbc->getFloat(i, rollF));
                kits_[attachDbc->getUInt32(i, parentF)].models.push_back(std::move(*model));
            }
        }
    }

    // Load SpellVisual.dbc - each visual's missile and kits
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
    // The kits a cast plays (0x007fa2e0, 0x0080e1b0, 0x00800d00, 0x00700e20).
    const uint32_t svKitFields[7] = {svColumn("PrecastKit"),      svColumn("CastKit"),
                                     svColumn("ImpactKit"),       svColumn("CasterImpactKit"),
                                     svColumn("TargetImpactKit"), svColumn("InstantAreaKit"),
                                     svColumn("ImpactAreaKit")};
    auto svInt = [&](uint32_t row, uint32_t field, int32_t fallback) {
        return field != 0xFFFFFFFFu ? svDbc->getInt32(row, field) : fallback;
    };
    auto svVec = [&](uint32_t row, const uint32_t (&fields)[3]) {
        glm::vec3 v(0.0f);
        for (int axis = 0; axis < 3; ++axis)
            if (fields[axis] != 0xFFFFFFFFu) v[axis] = svDbc->getFloat(row, fields[axis]);
        return v;
    };
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

        // The cast's kits, those the kit table has.
        {
            std::array<uint32_t, 7> ids{};
            bool any = false;
            for (size_t k = 0; k < ids.size(); ++k) {
                const auto id = static_cast<uint32_t>(std::max(svInt(i, svKitFields[k], 0), 0));
                ids[k] = kits_.count(id) != 0 ? id : 0u;
                any = any || ids[k] != 0;
            }
            if (any)
                visualKits_[vid] = VisualKits{.precast = ids[0], .cast = ids[1], .impact = ids[2],
                                              .casterImpact = ids[3], .targetImpact = ids[4],
                                              .instantArea = ids[5], .impactArea = ids[6]};
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
    }
    LOG_INFO("SpellVisual: loaded ", kits_.size(), " kits, ", visualKits_.size(), " visuals' cast kits, ",
             missileVisuals_.size(), " missiles (of ", svDbc->getRecordCount(), " visuals)");
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
                                                uint32_t castTimeMs, uint32_t attachInstanceId, uint32_t spellId) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    // 0x007fa2e0: the visual's PrecastKit (+4), type 4. A visual without
    // one shows nothing more at the cast's start: its cast kit plays at
    // SMSG_SPELL_GO and only then.
    auto it = visualKits_.find(visualId);
    if (it == visualKits_.end() || it->second.precast == 0) return;
    playKitOnUnit(it->second.precast, spell_kit::KitType::Precast, attachInstanceId, worldPosition,
                  attachInstanceId == 0 ? &worldPosition : nullptr, castTimeMs, spellId);
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId, bool onCaster,
                                         uint32_t spellId) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = visualKits_.find(visualId);
    if (it == visualKits_.end()) return;
    const VisualKits& kits = it->second;
    // The cast kit (+8) on the caster (0x0080e1b0); a unit's impact kit
    // (0x00800d00) on it (0x00801f10, 0x00700e20) - both type 1.
    const uint32_t kitId = useImpactKit
                               ? spell_kit::impactKitFor(onCaster, kits.impact, kits.casterImpact, kits.targetImpact)
                               : kits.cast;
    if (kitId == 0) return;
    // The impact kit's sound plays with it, where it plays (0x00745230
    // flags the kit to play its SoundID); for a missile that is on arrival.
    if (useImpactKit) {
        auto kitIt = kits_.find(kitId);
        if (const LoadedSound* sound = kitIt != kits_.end() ? soundEntry(kitIt->second.soundId) : nullptr)
            audio::AudioEngine::instance().playSound3D(sound->data, worldPosition, sound->volume);
    }
    playKitOnUnit(kitId, spell_kit::KitType::Cast, attachInstanceId, worldPosition,
                  attachInstanceId == 0 ? &worldPosition : nullptr, 0, spellId);
}

void SpellVisualSystem::playKit(uint32_t kitId, spell_kit::KitType type, const glm::vec3& worldPosition,
                                uint32_t renderInstanceId) {
    if (!m2Renderer_ || kitId == 0) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    playKitOnUnit(kitId, type, renderInstanceId, worldPosition, renderInstanceId == 0 ? &worldPosition : nullptr);
}

void SpellVisualSystem::playSpellAreaKits(uint32_t visualId, const glm::vec3& place, bool missileCarriesImpact,
                                          uint32_t spellId) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = visualKits_.find(visualId);
    if (it == visualKits_.end()) return;
    // 0x0080e1b0: type 3 at the destination.
    if (it->second.instantArea != 0)
        playKitOnUnit(it->second.instantArea, spell_kit::KitType::Area, 0, place, &place, 0, spellId);
    if (it->second.impactArea != 0 && !missileCarriesImpact)
        playKitOnUnit(it->second.impactArea, spell_kit::KitType::Area, 0, place, &place, 0, spellId);
}

std::vector<SpellVisualSystem::KitModelInstance> SpellVisualSystem::playKitOnUnit(
    uint32_t kitId, spell_kit::KitType type, uint32_t renderInstanceId, const glm::vec3& position,
    const glm::vec3* place, uint32_t castTimeMs, uint32_t spellId) {
    auto it = kitId ? kits_.find(kitId) : kits_.end();
    if (it == kits_.end()) return {};
    const KitRecord& kit = it->second;
    // 0x0073b140: the unit's own part - the weapon effects of a precast or
    // cast kit (0x0073a6c0), the kit's colour and its camera shake.
    if (type == spell_kit::KitType::Precast || type == spell_kit::KitType::Cast)
        playKitWeaponEffects(kit.weaponEffects, renderInstanceId, type == spell_kit::KitType::Precast, castTimeMs);
    playKitColourFade(kitId, renderInstanceId, spellId);
    playKitShake(kitId, place ? *place : position);
    return playKitModels(kit, type, renderInstanceId, position, place, castTimeMs);
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
        // With its kit: a precast kit's for the cast, a cast kit's once.
        addKitModel(instanceId, isPrecast ? spell_kit::KitModelLife::Repeat : spell_kit::KitModelLife::Once,
                    isPrecast, castTimeMs, true, attachId, attachInstanceId, scale, glm::mat4(1.0f));
    }
}

void SpellVisualSystem::addKitModel(uint32_t instanceId, spell_kit::KitModelLife life, bool isPrecast,
                                    uint32_t castTimeMs, bool attached, uint32_t attachment,
                                    uint32_t attachInstanceId, float scale, const glm::mat4& local) {
    float standMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    if (!(standMs > 0.0f)) standMs = SPELL_VISUAL_DEFAULT_DURATION * 1000.0f;
    SpellVisualInstance shown{.instanceId = instanceId, .elapsed = 0.0f, .duration = standMs * 0.001f,
                              .isPrecast = isPrecast, .attachmentId = attachment,
                              .attachInstanceId = attachInstanceId, .attached = attached, .scale = scale,
                              .local = local};
    if (life == spell_kit::KitModelLife::Repeat) {
        // 0x007435a0: its animation again each time it ends, for the cast.
        if (castTimeMs > 0) shown.duration = static_cast<float>(castTimeMs) * 0.001f;
    } else {
        // 0x00744870: its Stand once, then its Decay where it has one.
        const auto timing =
            spell_kit::onceTiming(standMs, m2Renderer_->hasAnimation(instanceId, spell_kit::kAnimDecay));
        shown.switchAt = timing.switchAt;
        shown.decays = timing.decays;
        if (timing.decays) shown.duration = 1e30f;  // set as the Decay starts
    }
    activeSpellVisuals_.push_back(shown);
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
                                           const MissileTrajectory* trajectory, uint32_t spellId) {
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
    missile.casterInstanceId = from.renderInstanceId;
    missile.spellId = spellId;
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
        const uint32_t casterInstance = it->casterInstanceId;
        const uint32_t missileSpell = it->spellId;
        const glm::vec3 landedAt = it->position;
        std::vector<MissileEnd> impacts = std::move(it->impacts);
        it = activeMissiles_.erase(it);
        // 0x00700e20: the visual's ImpactAreaKit (+0x60) where it lands.
        if (auto kitsIt = visualKits_.find(visualId); kitsIt != visualKits_.end() && kitsIt->second.impactArea != 0)
            playKitOnUnit(kitsIt->second.impactArea, spell_kit::KitType::Area, 0, landedAt, &landedAt, 0,
                          missileSpell);
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
                    if (aura.visualId != visualId || !aura.awaitingMissile) continue;
                    aura.awaitingMissile = false;
                    glm::vec3 unitPos;
                    if (charRenderer && charRenderer->getInstancePosition(unitInstance, unitPos))
                        playKitShake(aura.kitId, unitPos);
                    playKitColourFade(aura.kitId, unitInstance, aura.spellId);
                }
            }
        }
        // 0x00700e20 plays a destination point's kit only while the caster
        // is still about.
        glm::mat4 casterFrame;
        const bool casterAbout = casterInstance != 0 && charRenderer &&
                                 charRenderer->getInstanceFrame(casterInstance, casterFrame);
        for (const MissileEnd& impact : impacts) {
            if (impact.renderInstanceId == 0 && !casterAbout) continue;
            glm::vec3 impactPos = impact.position;
            uint32_t impactInstance = 0;
            glm::mat4 unitFrame;
            if (impact.renderInstanceId != 0 && charRenderer &&
                charRenderer->getInstanceFrame(impact.renderInstanceId, unitFrame)) {
                impactPos = glm::vec3(unitFrame[3]);
                impactInstance = impact.renderInstanceId;
            }
            // The caster's own CasterImpactKit where it is one of them.
            playSpellVisual(visualId, impactPos, /*useImpactKit=*/true, impactInstance,
                            impactInstance != 0 && impactInstance == casterInstance, missileSpell);
        }
    }
}

bool SpellVisualSystem::kitModelTransform(uint32_t renderInstanceId, int32_t attachment, const glm::mat4& local,
                                          float effectScale, float minScale, float maxScale, glm::mat4& out,
                                          float& scale) {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || renderInstanceId == 0 || attachment < 0) return false;
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

glm::mat4 SpellVisualSystem::worldKitModelTransform(const KitModel& model, uint32_t renderInstanceId,
                                                     const glm::vec3& position, const glm::vec3* place) {
    // 0x006f8ae0: at a place, sized by the effect alone; where the unit
    // stands (flag 0x200), turned as it faces then and sized by it.
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    glm::vec3 at = place ? *place : position;
    float facing = 0.0f;
    float unitSize = 1.0f;
    glm::mat4 frame;
    if (!place && charRenderer && renderInstanceId != 0 && charRenderer->getInstanceFrame(renderInstanceId, frame)) {
        at = glm::vec3(frame[3]);
        facing = std::atan2(frame[0].y, frame[0].x);
        // 0x006f7950 times the unit's own scale.
        const pipeline::M2Model* unitModel = charRenderer->getInstanceModelData(renderInstanceId);
        const float worldScale = worldEffectScale_ ? worldEffectScale_(renderInstanceId) : 1.0f;
        unitSize = unitModel && unitModel->hasVertexBox
                       ? spell_kit::unitWorldEffectSize(worldScale, unitModel->vertexBoxMin, unitModel->vertexBoxMax)
                       : 1.0f;
        unitSize *= glm::length(glm::vec3(frame[0]));
    }
    const float scale = spell_kit::worldKitModelScale(unitSize, model.scale, model.minScale, model.maxScale);
    return spell_kit::worldKitModelMatrix(at, facing, model.local, scale);
}

std::vector<SpellVisualSystem::KitModelInstance> SpellVisualSystem::playKitModels(
    const KitRecord& kit, spell_kit::KitType type, uint32_t renderInstanceId, const glm::vec3& position,
    const glm::vec3* place, uint32_t castTimeMs) {
    std::vector<KitModelInstance> shown;
    if (!m2Renderer_) return shown;
    const spell_kit::KitModelLife life = spell_kit::kitModelLife(type);
    for (const KitModel& model : kit.models) {
        const auto where = spell_kit::kitModelPlace(type, model.kind, model.attachment, place != nullptr);
        glm::mat4 transform(1.0f);
        float scale = 1.0f;
        if (where == spell_kit::KitModelPlace::None) continue;
        if (where == spell_kit::KitModelPlace::Attachment) {
            // Dropped where the unit's model lacks the attachment (0x006f8c50).
            if (!kitModelTransform(renderInstanceId, model.attachment, model.local, model.scale, model.minScale,
                                   model.maxScale, transform, scale))
                continue;
        } else {
            transform = worldKitModelTransform(model, renderInstanceId, position,
                                               where == spell_kit::KitModelPlace::AtPlace ? place : nullptr);
        }
        const uint32_t modelId = acquireEffectModel(model.path);
        if (modelId == 0) continue;
        const uint32_t instanceId = m2Renderer_->createInstance(modelId, glm::vec3(transform[3]), glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) continue;
        m2Renderer_->restartInstanceAnimation(instanceId);
        m2Renderer_->setInstanceTransform(instanceId, transform);
        const bool attached = where == spell_kit::KitModelPlace::Attachment;
        if (life == spell_kit::KitModelLife::Hold) {
            // 0x007449c0: its Stand once, then its Hold (158) - its Stand
            // again where the kit has Flags 0x20 or the model no Hold.
            KitModelInstance held{.instanceId = instanceId, .attachment = attached ? model.attachment : -1,
                                  .local = model.local, .scale = scale};
            if ((kit.flags & spell_kit::kKitFlagStateStand) == 0 &&
                m2Renderer_->hasAnimation(instanceId, spell_kit::kAnimHold))
                held.holdAt = m2Renderer_->getInstanceAnimDuration(instanceId) * 0.001f;
            shown.push_back(held);
            continue;
        }
        // A model in the world stays where it was put; one on an attachment
        // rides it.
        addKitModel(instanceId, life, type == spell_kit::KitType::Precast, castTimeMs, attached,
                    attached ? static_cast<uint32_t>(model.attachment) : 0u, attached ? renderInstanceId : 0u, scale,
                    model.local);
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
    if (!aura.awaitingMissile && instance != 0) {
        glm::vec3 unitPos;
        if (renderer_ && renderer_->getCharacterRenderer() &&
            renderer_->getCharacterRenderer()->getInstancePosition(instance, unitPos))
            playKitShake(aura.kitId, unitPos);
        playKitColourFade(aura.kitId, instance, spellId);
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
    if (kitIt == kits_.end() || instance == 0) return;
    glm::vec3 unitPos(0.0f);
    if (renderer_ && renderer_->getCharacterRenderer())
        renderer_->getCharacterRenderer()->getInstancePosition(instance, unitPos);
    playKitOnUnit(kitIt->first, spell_kit::KitType::StateDone, instance, unitPos, nullptr, 0, spellId);
}

void SpellVisualSystem::playKitShake(uint32_t kitId, const glm::vec3& origin) {
    auto it = kitId ? kits_.find(kitId) : kits_.end();
    if (it != kits_.end() && it->second.shakeId > 0) playCameraShakes(it->second.shakeId, origin);
}

bool SpellVisualSystem::kitColoursUnit(uint32_t renderInstanceId, uint32_t spellId) const {
    const uint32_t typeFlags = unitTypeFlags_ ? unitTypeFlags_(renderInstanceId) : 0u;
    const uint32_t targetKind = kitSpellResolver_ && spellId ? kitSpellResolver_(spellId).targetKind : 0u;
    return spell_kit::kitColoursUnit((typeFlags & 0x40u) != 0, targetKind);
}

void SpellVisualSystem::playKitColourFade(uint32_t kitId, uint32_t renderInstanceId, uint32_t spellId) {
    auto it = kitId ? kits_.find(kitId) : kits_.end();
    if (it == kits_.end()) return;
    const KitRecord& kit = it->second;
    for (uint32_t k = 0; k < 4; ++k) {
        if (kit.charProc[k] == spell_kit::kCharProcLightTint) {
            // 0x007265c0 case 6 (0x007fa450): the light toward ParamZero
            // for the spell's cast time, reaching it at ParamOne of it.
            const uint32_t castMs = kitSpellResolver_ && spellId ? kitSpellResolver_(spellId).castTimeMs : 0u;
            lightTint_ = spell_kit::lightTint(static_cast<uint32_t>(std::lround(kit.charParam[k][0])),
                                              kit.charParam[k][1], castMs, colourClockMs_);
            continue;
        }
        if (kit.charProc[k] != spell_kit::kCharProcColourFade || renderInstanceId == 0) continue;
        if (!kitColoursUnit(renderInstanceId, spellId)) continue;
        // 0x007265c0 case 13: the colour, held ParamOne seconds and faded
        // over ParamTwo.
        colourFades_[renderInstanceId] = spell_kit::ColourFade{
            .startMs = colourClockMs_,
            .colour = static_cast<uint32_t>(std::lround(kit.charParam[k][0])) | 0xff000000u,
            .holdMs = static_cast<uint32_t>(std::lround(kit.charParam[k][1] * 1000.0f)),
            .fadeMs = static_cast<uint32_t>(std::lround(kit.charParam[k][2] * 1000.0f))};
    }
}

void SpellVisualSystem::updateLightTint() {
    float amount = 0.0f;
    if (lightTint_ && !spell_kit::lightTintAmount(*lightTint_, colourClockMs_, amount)) lightTint_.reset();
    if (!lightTintSink_) return;
    if (lightTint_) {
        // 0x007ee300: the amount as a byte.
        lightTintSink_(spell_kit::colourToRgb(lightTint_->colour),
                       static_cast<uint32_t>(std::clamp(std::lround(amount * 255.0f), 0L, 255L)));
        lightTinted_ = true;
    } else if (lightTinted_) {
        lightTintSink_(glm::vec3(1.0f), 0u);
        lightTinted_ = false;
    }
}

void SpellVisualSystem::updateUnitAlphas() {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;
    // 0x007265c0 case 14: the alpha of the latest aura's kit that has one
    // (the list's head, 0x0071abe0), faded to over its ParamTwo.
    std::unordered_map<uint32_t, std::pair<float, uint32_t>> wanted;  // instance → alpha, fade ms
    for (const auto& [guid, unit] : unitAuraKits_) {
        if (unit.auras.empty()) continue;
        const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(guid) : 0;
        if (instance == 0) continue;
        for (const AuraKit& aura : unit.auras) {
            auto kitIt = kits_.find(aura.kitId);
            if (kitIt == kits_.end() || aura.awaitingMissile) continue;
            for (uint32_t k = 0; k < 4; ++k) {
                const auto& param = kitIt->second.charParam[k];
                if (kitIt->second.charProc[k] == spell_kit::kCharProcAlpha && spell_kit::kitAlphaTaken(param[0]))
                    wanted[instance] = {param[0], spell_kit::kitAlphaFadeMs(param[2])};
            }
        }
    }
    for (auto it = unitKitAlphas_.begin(); it != unitKitAlphas_.end();) {
        if (wanted.count(it->first) == 0) {
            // Gone: back to the unit's own over a second.
            charRenderer->setInstanceKitAlpha(it->first, 1.0f, 1.0f);
            it = unitKitAlphas_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [instance, alphaFade] : wanted) {
        auto [it, added] = unitKitAlphas_.try_emplace(instance, alphaFade.first);
        if (!added && it->second == alphaFade.first) continue;
        it->second = alphaFade.first;
        charRenderer->setInstanceKitAlpha(instance, alphaFade.first, static_cast<float>(alphaFade.second) * 0.001f);
    }
}

void SpellVisualSystem::updateUnitColours() {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;
    std::unordered_map<uint32_t, uint32_t> colours;  // render instance → colour
    // The auras' colours (case 1), the latest on top as the client's list
    // has it (0x00720db0 reads its head).
    for (const auto& [guid, unit] : unitAuraKits_) {
        if (unit.auras.empty()) continue;
        const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(guid) : 0;
        if (instance == 0) continue;
        for (const AuraKit& aura : unit.auras) {
            auto kitIt = kits_.find(aura.kitId);
            if (kitIt == kits_.end() || !kitColoursUnit(instance, aura.spellId)) continue;
            for (uint32_t k = 0; k < 4; ++k) {
                if (kitIt->second.charProc[k] == spell_kit::kCharProcColour)
                    colours[instance] = static_cast<uint32_t>(std::lround(kitIt->second.charParam[k][0])) | 0xff000000u;
            }
        }
    }
    // A fade comes first (0x0071a9a0), and goes when it has run out.
    for (auto it = colourFades_.begin(); it != colourFades_.end();) {
        uint32_t colour = 0;
        if (spell_kit::fadeColour(it->second, colourClockMs_, colour)) {
            colours[it->first] = colour;
            ++it;
        } else {
            it = colourFades_.erase(it);
        }
    }
    for (uint32_t instance : colouredInstances_) {
        if (colours.count(instance) == 0) charRenderer->setInstanceDiffuseColour(instance, glm::vec3(1.0f));
    }
    colouredInstances_.clear();
    for (const auto& [instance, colour] : colours) {
        charRenderer->setInstanceDiffuseColour(instance, spell_kit::colourToRgb(colour));
        colouredInstances_.insert(instance);
    }
}

void SpellVisualSystem::playCameraShakes(uint32_t spellEffectCameraShakesId, const glm::vec3& origin) {
    if (!cameraShakeSink_ || spellEffectCameraShakesId == 0) return;
    if (!cameraShakesLoaded_) loadCameraShakes();
    auto it = spellEffectShakes_.find(spellEffectCameraShakesId);
    if (it == spellEffectShakes_.end()) return;
    for (const camera_shake::Shake& shake : it->second) cameraShakeSink_(shake, origin);
}

void SpellVisualSystem::loadCameraShakes() {
    auto* am = cachedAssetManager_ ? cachedAssetManager_ : core::Application::getInstance().getAssetManager();
    if (!am || !am->isInitialized()) return;
    cachedAssetManager_ = am;
    cameraShakesLoaded_ = true;  // a real attempt follows
    {
        auto* layouts = pipeline::getActiveDBCLayout();
        const auto* shakeLayout = layouts ? layouts->getLayout("CameraShakes") : nullptr;
        const auto* setLayout = layouts ? layouts->getLayout("SpellEffectCameraShakes") : nullptr;
        auto shakeDbc = cachedAssetManager_->loadDBCOptional("CameraShakes.dbc");
        auto setDbc = cachedAssetManager_->loadDBCOptional("SpellEffectCameraShakes.dbc");
        std::unordered_map<uint32_t, camera_shake::Shake> rows;
        if (shakeLayout && shakeDbc && shakeDbc->isLoaded()) {
            const uint32_t fc = shakeDbc->getFieldCount();
            const std::array<uint32_t, 7> f = {
                shakeLayout->tryField("ShakeType"), shakeLayout->tryField("Direction"),
                shakeLayout->tryField("Amplitude"), shakeLayout->tryField("Frequency"),
                shakeLayout->tryField("Duration"),  shakeLayout->tryField("Phase"),
                shakeLayout->tryField("Coefficient")};
            if (std::all_of(f.begin(), f.end(), [fc](uint32_t x) { return x < fc; })) {
                for (uint32_t i = 0; i < shakeDbc->getRecordCount(); ++i) {
                    rows[shakeDbc->getUInt32(i, 0)] = camera_shake::fromRow(
                        shakeDbc->getUInt32(i, f[0]), shakeDbc->getUInt32(i, f[1]), shakeDbc->getFloat(i, f[2]),
                        shakeDbc->getFloat(i, f[3]), shakeDbc->getFloat(i, f[4]), shakeDbc->getFloat(i, f[5]),
                        shakeDbc->getFloat(i, f[6]));
                }
            }
        }
        if (setLayout && setDbc && setDbc->isLoaded()) {
            const uint32_t fc = setDbc->getFieldCount();
            const std::array<uint32_t, 3> f = {setLayout->tryField("CameraShake0"), setLayout->tryField("CameraShake1"),
                                               setLayout->tryField("CameraShake2")};
            for (uint32_t i = 0; i < setDbc->getRecordCount(); ++i) {
                std::vector<camera_shake::Shake> shakes;
                for (uint32_t col : f) {
                    if (col >= fc) continue;
                    auto row = rows.find(setDbc->getUInt32(i, col));
                    if (row != rows.end()) shakes.push_back(row->second);
                }
                if (!shakes.empty()) spellEffectShakes_[setDbc->getUInt32(i, 0)] = std::move(shakes);
            }
        }
    }
}

void SpellVisualSystem::playSoundAt(uint32_t soundId, const glm::vec3& position) {
    if (const LoadedSound* sound = soundId ? soundEntry(soundId) : nullptr)
        audio::AudioEngine::instance().playSound3D(sound->data, position, sound->volume);
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

void SpellVisualSystem::updateAuraKits(float deltaTime) {
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
                    glm::vec3 unitPos(0.0f);
                    if (renderer_ && renderer_->getCharacterRenderer())
                        renderer_->getCharacterRenderer()->getInstancePosition(instance, unitPos);
                    aura.models = playKitModels(kitIt->second, spell_kit::KitType::State, instance, unitPos, nullptr);
                    aura.boundInstance = instance;
                }
                continue;
            }
            CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
            if (!charRenderer) continue;
            for (KitModelInstance& model : aura.models) {
                // 0x007449c0: its Hold once its Stand has run.
                model.elapsed += deltaTime;
                if (model.holdAt >= 0.0f && model.elapsed >= model.holdAt) {
                    m2Renderer_->setInstanceAnimation(model.instanceId, spell_kit::kAnimHold, true);
                    model.holdAt = -1.0f;
                }
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

void SpellVisualSystem::update(float deltaTime) {
    // First: an arrival plays its impact kit, which joins activeSpellVisuals_.
    colourClockMs_ += static_cast<uint32_t>(std::lround(deltaTime * 1000.0f));
    updateMissiles(deltaTime);
    updateAuraKits(deltaTime);
    updateUnitColours();
    updateUnitAlphas();
    updateLightTint();
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty()) return;

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        // 0x00744870: as its Stand ends, its Decay plays to its end.
        if (it->switchAt >= 0.0f && it->elapsed >= it->switchAt) {
            if (it->decays) {
                m2Renderer_->setInstanceAnimation(it->instanceId, spell_kit::kAnimDecay, false);
                it->duration = it->elapsed + m2Renderer_->getInstanceAnimDuration(it->instanceId) * 0.001f;
            } else {
                it->duration = it->elapsed;
            }
            it->switchAt = -1.0f;
        }
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // One on an attachment rides it (0x006f8c50) - the unit's that
            // played it; one in the world stays where it was put.
            if (it->attached && it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(it->attachInstanceId, it->attachmentId, attachMat)) {
                    attachMat = attachMat * it->local;
                    if (it->scale != 1.0f) attachMat = attachMat * glm::scale(glm::mat4(1.0f), glm::vec3(it->scale));
                    m2Renderer_->setInstanceTransform(it->instanceId, attachMat);
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
    colourFades_.clear();
    updateUnitColours();
    updateUnitAlphas();
    lightTint_.reset();
    updateLightTint();
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

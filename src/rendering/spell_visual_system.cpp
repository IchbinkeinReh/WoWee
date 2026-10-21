#include "rendering/spell_visual_system.hpp"
#include "rendering/animation/upper_body.hpp"
#include <cctype>
#include <future>
#include "rendering/dynamic_object_visual.hpp"
#include "rendering/blizzard_object.hpp"
#include "rendering/mount_seat.hpp"
#include "rendering/spell_missile.hpp"
#include "rendering/placement_transform.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/coordinates.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/weapon_attachment.hpp"
#include "audio/audio_engine.hpp"
#include "addons/lua_api_registrations.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
    // AreaEffectSize (+0xc), which sizes a dynamic object's model
    // (0x00704d90).
    const uint32_t fxAreaSizeField = fxLayout ? fxLayout->tryField("AreaEffectSize") : 0xFFFFFFFFu;
    std::unordered_map<uint32_t, float> effectAreaSizes;
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
            if (fxAreaSizeField < fxDbc->getFieldCount()) effectAreaSizes[id] = fxDbc->getFloat(i, fxAreaSizeField);
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
    // SpellVisualKitAreaModel.dbc's ModelName (+4) by row, as 0x007fbd70
    // counts them.
    std::vector<std::string> areaModelPaths;
    if (const auto* areaLayout = layout ? layout->getLayout("SpellVisualKitAreaModel") : nullptr) {
        auto areaDbc = cachedAssetManager_->loadDBCOptional("SpellVisualKitAreaModel.dbc");
        const uint32_t nameField = areaLayout->tryField("ModelName");
        if (areaDbc && areaDbc->isLoaded() && nameField < areaDbc->getFieldCount()) {
            for (uint32_t i = 0; i < areaDbc->getRecordCount(); ++i)
                areaModelPaths.push_back(pipeline::modelPathToM2(areaDbc->getString(i, nameField)));
        }
    }
    // Each kit's AnimID (+8), for a mount aura's rider pose (0x00724820).
    std::unordered_map<uint32_t, int32_t> kitAnimIds;
    // Each kit's WorldEffect (+0x38), BaseEffect (+0x14) and SoundID
    // (+0x3c), for a dynamic object's (0x00704b30, 0x00704f60).
    struct KitAreaColumns {
        uint32_t worldEffect = 0;
        uint32_t baseEffect = 0;
        uint32_t soundId = 0;
        /// The first CharProc 9 (0x00704f60): its CharParamZero and One.
        bool fallingModels = false;
        float fallingRow = 0.0f;
        float fallingPerSecond = 0.0f;
    };
    std::unordered_map<uint32_t, KitAreaColumns> kitAreaColumns;
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
        const uint32_t kitAnimField = kitColumn("AnimID", 2);
        const uint32_t kitStartAnimField = kitColumn("StartAnimID", 1);
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
            if (kitAnimField != 0xFFFFFFFFu && kitDbc->getInt32(i, kitAnimField) >= 0)
                kitAnimIds[kitId] = kitDbc->getInt32(i, kitAnimField);
            {
                KitAreaColumns& area = kitAreaColumns[kitId];
                const auto column = [&](uint32_t field) {
                    return field != 0xFFFFFFFFu ? static_cast<uint32_t>(std::max(kitDbc->getInt32(i, field), 0)) : 0u;
                };
                area.worldEffect = column(worldField);
                area.baseEffect = column(kitColumn("BaseEffect", 5));
                area.soundId = column(kitSoundField);
                for (uint32_t k = 0; k < 4 && !area.fallingModels; ++k) {
                    if (procFields[k] == 0xFFFFFFFFu ||
                        kitDbc->getUInt32(i, procFields[k]) != blizzard_object::kCharProc)
                        continue;
                    area.fallingModels = true;
                    if (paramFields[k][0] != 0xFFFFFFFFu) area.fallingRow = kitDbc->getFloat(i, paramFields[k][0]);
                    if (paramFields[k][1] != 0xFFFFFFFFu)
                        area.fallingPerSecond = kitDbc->getFloat(i, paramFields[k][1]);
                }
            }
            KitRecord kit;
            kit.id = kitId;
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
            if (kitAnimField != 0xFFFFFFFFu) kit.animId = kitDbc->getInt32(i, kitAnimField);
            if (kitStartAnimField != 0xFFFFFFFFu) kit.startAnimId = kitDbc->getInt32(i, kitStartAnimField);
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
                kit.shakeId != 0 || hasProc || kit.animId > 0 || kit.startAnimId >= 0)
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
    const uint32_t svMissileMotionField = svColumn("MissileMotion");
    const uint32_t svStateKitField     = svColumn("StateKit");
    const uint32_t svStateDoneKitField = svColumn("StateDoneKit");
    const uint32_t svChannelKitField   = svColumn("ChannelKit");
    const uint32_t svPersistentKitField = svColumn("PersistentAreaKit");
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
                missile.motionId = static_cast<uint32_t>(std::max(svInt(i, svMissileMotionField, 0), 0));
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
        // keep a state kit to an unarmed, idle unit; and the ChannelKit a
        // channel holds on its caster (0x0072bc70).
        {
            VisualAuraKits auraKits;
            auraKits.stateKit = static_cast<uint32_t>(std::max(svInt(i, svStateKitField, 0), 0));
            if (auto animIt = kitAnimIds.find(auraKits.stateKit); animIt != kitAnimIds.end())
                visualStateKitAnim_[vid] = animIt->second;
            auraKits.stateDoneKit = static_cast<uint32_t>(std::max(svInt(i, svStateDoneKitField, 0), 0));
            auraKits.channelKit = static_cast<uint32_t>(std::max(svInt(i, svChannelKitField, 0), 0));
            auraKits.flags = static_cast<uint32_t>(svInt(i, svFlagsField, 0));
            if (kits_.count(auraKits.stateKit) == 0) auraKits.stateKit = 0;
            if (kits_.count(auraKits.stateDoneKit) == 0) auraKits.stateDoneKit = 0;
            if (kits_.count(auraKits.channelKit) == 0) auraKits.channelKit = 0;
            if (auraKits.stateKit != 0 || auraKits.stateDoneKit != 0 || auraKits.channelKit != 0)
                visualAuraKits_[vid] = auraKits;
        }
        // The PersistentAreaKit a dynamic object of the visual's spell shows
        // (0x00704b30): the kit's WorldEffect, else its BaseEffect, and its
        // SoundID (0x00704f60). Nothing where the kit table lacks the row.
        if (const auto kitIt = kitAreaColumns.find(static_cast<uint32_t>(std::max(svInt(i, svPersistentKitField, 0), 0)));
            kitIt != kitAreaColumns.end()) {
            const KitAreaColumns& kit = kitIt->second;
            PersistentArea area;
            area.soundId = kit.soundId;
            const uint32_t effect = dynamic_object::effectRow(kit.worldEffect, effectPaths.count(kit.worldEffect) != 0,
                                                              kit.baseEffect, effectPaths.count(kit.baseEffect) != 0);
            if (effect != 0) {
                area.modelPath = effectPaths[effect];
                if (auto it = effectAreaSizes.find(effect); it != effectAreaSizes.end()) area.areaEffectSize = it->second;
                if (auto it = effectScales.find(effect); it != effectScales.end()) area.effectScale = it->second;
            }
            // 0x007fbd70: a CharProc 9's model, its SpellVisualKitAreaModel row.
            if (kit.fallingModels) {
                const auto row = blizzard_object::areaModelRow(kit.fallingRow, areaModelPaths.size());
                area.fallingModel = row ? areaModelPaths[*row] : pipeline::modelPathToM2(blizzard_object::kDefaultModel);
                area.fallingPerSecond = kit.fallingPerSecond;
            }
            if (!area.modelPath.empty() || area.soundId != 0 || !area.fallingModel.empty())
                persistentAreas_[vid] = std::move(area);
        }
        // Where a chain leaves and meets a unit (0x007fc5f0, 0x007fabf0).
        {
            const auto visualFlags = static_cast<uint32_t>(svInt(i, svFlagsField, 0));
            VisualEnds ends;
            ends.sourceAttachment = spell_missile::m2AttachmentFor(svInt(i, svMissileAttField, -1), visualFlags);
            ends.destinationAttachment = spell_missile::m2AttachmentFor(svInt(i, svMissileDstField, -1), visualFlags);
            ends.castOffset = spell_missile::attachmentOffset(svVec(i, svCastOffField));
            ends.impactOffset = spell_missile::attachmentOffset(svVec(i, svImpactOffField));
            ends.flags = visualFlags;
            visualEnds_[vid] = ends;
        }
    }
    LOG_INFO("SpellVisual: loaded ", kits_.size(), " kits, ", visualKits_.size(), " visuals' cast kits, ",
             missileVisuals_.size(), " missiles (of ", svDbc->getRecordCount(), " visuals)");
}

namespace {

/// An effect model made ready off the main thread: everything but the upload.
struct PreparedEffectModel {
    bool ok = false;
    std::string failure;
    pipeline::M2Model model;
    std::unordered_map<std::string, pipeline::BLPImage> textures;
};

PreparedEffectModel prepareEffectModel(pipeline::AssetManager* assets, const std::string& modelPath,
                                       bool decodeTextures) {
    PreparedEffectModel prepared;
    auto m2Data = assets->readFile(modelPath);
    if (m2Data.empty()) {
        prepared.failure = "could not read model";
        return prepared;
    }
    prepared.model = pipeline::M2Loader::load(m2Data);
    if (prepared.model.name.empty()) prepared.model.name = modelPath;
    if (prepared.model.vertices.empty() && prepared.model.particleEmitters.empty()) {
        prepared.failure = "empty model";
        return prepared;
    }
    // Load skin file for WotLK-format M2s
    if (prepared.model.version >= 264) {
        std::string skinPath = pipeline::skinPathForM2(modelPath);
        auto skinData = assets->readFile(skinPath);
        if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, prepared.model);
    }
    // The textures, decoded as the terrain worker decodes a doodad's and
    // under the key M2Renderer looks them up by.
    if (decodeTextures) {
        for (const auto& tex : prepared.model.textures) {
            if (tex.filename.empty()) continue;
            std::string texKey = tex.filename;
            std::replace(texKey.begin(), texKey.end(), '/', '\\');
            std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (prepared.textures.count(texKey)) continue;
            auto blp = assets->loadTexture(texKey, true);
            if (blp.isValid()) prepared.textures[texKey] = std::move(blp);
        }
    }
    prepared.ok = true;
    return prepared;
}

}  // namespace

/// Effect models are loaded on the spot inside it - the player's own spells.
struct SyncEffectLoadScope {
    bool& flag;
    bool previous;
    SyncEffectLoadScope(bool& f, bool on) : flag(f), previous(f) { flag = previous || on; }
    ~SyncEffectLoadScope() { flag = previous; }
    SyncEffectLoadScope(const SyncEffectLoadScope&) = delete;
    SyncEffectLoadScope& operator=(const SyncEffectLoadScope&) = delete;
};

struct SpellVisualSystem::PendingEffectLoad {
    std::string path;
    std::future<PreparedEffectModel> job;
};

bool SpellVisualSystem::involvesPlayer(uint32_t instanceA, uint32_t instanceB) const {
    const uint32_t player = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    return player != 0 && (instanceA == player || instanceB == player);
}

// A kit's model is an effect (0x006f94c0 gives it 0x006f7b00 for its
// events), on the unit the kit plays on - the active player's or another's.
void SpellVisualSystem::setEffectEventOwner(uint32_t instanceId, uint32_t unitInstanceId) {
    m2Renderer_->setInstanceEventOwner(instanceId, m2_event::Owner::Effect, involvesPlayer(unitInstanceId) ? 1u : 0u);
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

    auto pendingIt = pendingEffectLoads_.find(modelId);
    if (syncEffectLoads_) {
        // Wanted now. A worker already on it is waited for rather than raced.
        PreparedEffectModel prepared;
        if (pendingIt != pendingEffectLoads_.end()) {
            prepared = pendingIt->second->job.get();
            pendingEffectLoads_.erase(pendingIt);
        } else {
            prepared = prepareEffectModel(cachedAssetManager_, modelPath, false);
        }
        return uploadEffectModel(modelId, modelPath, &prepared) ? modelId : 0;
    }
    // A handful in flight at once: a crowd's first minute asks for dozens,
    // and a thread each would crowd out the terrain workers. One turned
    // away here is asked for again the next time the spell is cast.
    constexpr size_t kMaxLoadsInFlight = 4;
    if (pendingIt == pendingEffectLoads_.end() && pendingEffectLoads_.size() < kMaxLoadsInFlight) {
        auto pending = std::make_shared<PendingEffectLoad>();
        pending->path = modelPath;
        pending->job = std::async(std::launch::async, prepareEffectModel, cachedAssetManager_, modelPath, true);
        pendingEffectLoads_.emplace(modelId, std::move(pending));
    }
    return 0;
}

bool SpellVisualSystem::uploadEffectModel(uint32_t modelId, const std::string& modelPath, void* preparedModel) {
    auto& prepared = *static_cast<PreparedEffectModel*>(preparedModel);
    if (!prepared.ok) {
        LOG_WARNING("SpellVisual: ", prepared.failure, ": ", modelPath);
        spellVisualFailedModels_.insert(modelId);
        return false;
    }
    LOG_INFO("SpellVisual: M2 parsed: verts=", prepared.model.vertices.size(),
             " bones=", prepared.model.bones.size(), " particles=", prepared.model.particleEmitters.size(),
             " ribbons=", prepared.model.ribbonEmitters.size());
    if (!prepared.textures.empty()) m2Renderer_->setPredecodedBLPCache(&prepared.textures);
    const bool loaded = m2Renderer_->loadModel(prepared.model, modelId);
    m2Renderer_->setPredecodedBLPCache(nullptr);
    if (!loaded) {
        LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
        spellVisualFailedModels_.insert(modelId);
        return false;
    }
    m2Renderer_->markModelAsSpellEffect(modelId);
    LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    return true;
}

void SpellVisualSystem::finishEffectModelLoads() {
    if (pendingEffectLoads_.empty() || !m2Renderer_) return;
    // A few a frame: each upload is a GPU copy of the mesh and its textures.
    constexpr int kUploadsPerFrame = 2;
    int uploaded = 0;
    for (auto it = pendingEffectLoads_.begin();
         it != pendingEffectLoads_.end() && uploaded < kUploadsPerFrame;) {
        if (it->second->job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const uint32_t modelId = it->first;
        const std::string path = it->second->path;
        PreparedEffectModel prepared = it->second->job.get();
        it = pendingEffectLoads_.erase(it);
        ++uploaded;
        uploadEffectModel(modelId, path, &prepared);
    }
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs, uint32_t attachInstanceId, uint32_t spellId) {
    if (!m2Renderer_ || visualId == 0) return;
    const SyncEffectLoadScope syncLoads(syncEffectLoads_, involvesPlayer(attachInstanceId));
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
    const SyncEffectLoadScope syncLoads(syncEffectLoads_, involvesPlayer(attachInstanceId));
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
            audio::AudioEngine::instance().playSound3D(sound->data, worldPosition, sound->volume, 1.0f, 100.0f,
                                                       1.0f, audio::Channel::Effects);
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
                                          uint32_t spellId, uint64_t casterGuid, float speed,
                                          const std::optional<glm::vec3>& casterPosition) {
    // Mode 1: on the caster, at now plus the way at the spell's speed.
    const auto delayMs = casterGuid != 0 ? spell_kit::areaImpactDelayMs(speed, casterPosition, place) : std::nullopt;
    playAreaKits(visualId, place, missileCarriesImpact, spellId, casterGuid, delayMs);
}

void SpellVisualSystem::playDestinationAreaKits(uint32_t visualId, const glm::vec3& place,
                                                bool missileCarriesImpact, uint32_t spellId, uint64_t casterGuid,
                                                uint32_t impactDelayMs) {
    // 0x008022d0 hands the kit now plus the packet's delay.
    playAreaKits(visualId, place, missileCarriesImpact, spellId, casterGuid,
                 impactDelayMs > 0 ? std::optional<uint32_t>(impactDelayMs) : std::nullopt);
}

void SpellVisualSystem::playAreaKits(uint32_t visualId, const glm::vec3& place, bool missileCarriesImpact,
                                     uint32_t spellId, uint64_t casterGuid, std::optional<uint32_t> impactDelayMs) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = visualKits_.find(visualId);
    if (it == visualKits_.end()) return;
    // 0x0080e1b0: type 3 at the destination, played by the caster.
    const KitPlayExtra byCaster{.unitGuid = casterGuid};
    if (it->second.instantArea != 0)
        playKitOnUnit(it->second.instantArea, spell_kit::KitType::Area, 0, place, &place, 0, spellId, byCaster);
    if (it->second.impactArea == 0 || missileCarriesImpact) return;
    if (impactDelayMs) {
        // Timed (0x00728050 mode 1), played by the caster at its deadline.
        waitingKits_.push_back({.unitGuid = casterGuid,
                                .spellId = spellId,
                                .kitId = it->second.impactArea,
                                .type = spell_kit::KitType::Area,
                                .hasPlace = true,
                                .place = place,
                                .deadlineMs = colourClockMs_ + *impactDelayMs,
                                .mode = 1});
        return;
    }
    playKitOnUnit(it->second.impactArea, spell_kit::KitType::Area, 0, place, &place, 0, spellId, byCaster);
}

std::vector<SpellVisualSystem::KitModelInstance> SpellVisualSystem::playKitOnUnit(
    uint32_t kitId, spell_kit::KitType type, uint32_t renderInstanceId, const glm::vec3& position,
    const glm::vec3* place, uint32_t castTimeMs, uint32_t spellId, const KitPlayExtra& extra) {
    auto it = kitId ? kits_.find(kitId) : kits_.end();
    if (it == kits_.end()) return {};
    const KitRecord& kit = it->second;
    // 0x0072af60: a kit of a chain spell with Flags 0x1 waits on its unit
    // for the chain's pulse. A state kit waits in its aura instead
    // (holdStateKitForChain).
    const uint64_t unitGuid =
        renderInstanceId != 0 && instanceUnitResolver_ ? instanceUnitResolver_(renderInstanceId) : extra.unitGuid;
    if (!extra.replay && unitGuid != 0 && spellId != 0 && type != spell_kit::KitType::State &&
        (kit.flags & spell_kit::kKitFlagWaitsForChain) != 0 &&
        visualHasChainKit(spellVisualResolver_ ? spellVisualResolver_(spellId) : 0u)) {
        waitingKits_.push_back({.unitGuid = unitGuid,
                                .spellId = spellId,
                                .kitId = kitId,
                                .type = type,
                                .hasPlace = place != nullptr,
                                .place = place ? *place : position,
                                .castTimeMs = castTimeMs,
                                .counter = extra.counter,
                                .deadlineMs = colourClockMs_ + spell_kit::kWaitingKitMs});
        return {};
    }
    // 0x0073b140: the unit's own part - the kit's animation, the weapon
    // effects of a precast or cast kit (0x0073a6c0), the kit's colour and
    // its camera shake.
    if (renderInstanceId != 0 && type != spell_kit::KitType::State && kitAnimationSink_ &&
        (kit.startAnimId >= 0 || kit.animId >= 0)) {
        KitAnimationPlay play{.renderInstanceId = renderInstanceId,
                              .type = static_cast<uint32_t>(type),
                              .startAnim = kit.startAnimId,
                              .anim = kit.animId,
                              .kitFlags = kit.flags,
                              .castTime = kitSpellResolver_ && spellId && kitSpellResolver_(spellId).castTimeMs > 0};
        if (spellId != 0) play.castKitAnim = castAnimations(spellId).castAnim;
        kitAnimationSink_(play);
    }
    if (type == spell_kit::KitType::Precast || type == spell_kit::KitType::Cast)
        playKitWeaponEffects(kit.weaponEffects, renderInstanceId, type == spell_kit::KitType::Precast, castTimeMs);
    playKitColourFade(kitId, renderInstanceId, spellId);
    for (uint32_t k = 0; k < 4 && renderInstanceId != 0; ++k) {
        if (kit.charProc[k] == spell_kit::kCharProcMountTransition)
            startMountTransition(renderInstanceId, spellId, type, castTimeMs);
    }
    playKitShake(kitId, place ? *place : position);
    // CharProc 0 and 12 on the unit (0x007265c0); a state kit's start with
    // its aura or channel, which knows the chain's other end.
    if (renderInstanceId != 0 && type != spell_kit::KitType::State && instanceUnitResolver_)
        startKitChains(kit, instanceUnitResolver_(renderInstanceId), spellId, 0);
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
        setEffectEventOwner(instanceId, attachInstanceId);
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

// Where the caster lets the missile go (0x006fcd60: FUN_0071a720, then
// FUN_00720bf0). A visual naming MissileAttachment leaves from that
// attachment, carried by MissileCastOffset in its frame; one naming only an
// offset carries it from the directed spell hand (38, DAT_00ADAA50), or from
// the model's own frame where it has no such hand. Neither, and the client
// takes the caster's model's $CSL, $CSR or $CST event position, in that
// order, then the unit's own position (FUN_0071a720).
glm::vec3 SpellVisualSystem::missileSource(const MissileVisual& visual, const MissileEnd& from) const {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || from.renderInstanceId == 0) return from.position;
    glm::mat4 frame;
    if (visual.sourceAttachment >= 0) {
        if (charRenderer->getAttachmentTransform(from.renderInstanceId,
                                                 static_cast<uint32_t>(visual.sourceAttachment), frame))
            return glm::vec3(frame * glm::vec4(visual.castOffset, 1.0f));
    } else if (visual.castOffset != glm::vec3(0.0f)) {
        if (charRenderer->getAttachmentTransform(from.renderInstanceId, 38u, frame) ||
            charRenderer->getInstanceFrame(from.renderInstanceId, frame))
            return glm::vec3(frame * glm::vec4(visual.castOffset, 1.0f));
    }
    glm::vec3 event;
    for (uint32_t id : {0x4C534324u /* $CSL */, 0x52534324u /* $CSR */, 0x54534324u /* $CST */}) {
        if (charRenderer->getEventPosition(from.renderInstanceId, id, event)) return event;
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
                                           const MissileTrajectory* trajectory, uint32_t spellId,
                                           uint64_t casterGuid, uint8_t castCount) {
    if (!m2Renderer_ || visualId == 0 || !(speed > 0.0f)) return false;
    const SyncEffectLoadScope syncLoads(syncEffectLoads_,
                                        involvesPlayer(from.renderInstanceId, to.renderInstanceId));
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
    missile.casterGuid = casterGuid;
    missile.castCount = castCount;
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
    // A visual with a MissileMotion (0x00733xxx): flags 0x3800, its script
    // (0x006ff0a0) and the row's MissileCount of copies, each its index.
    int32_t copies = 1;
    if (visual->motionId != 0) {
        if (const MissileMotionRow* row = missileMotionRow(visual->motionId)) {
            missile.hasMotion = true;
            missile.facesDrawnMotion = true;
            if (row->missileCount > 0) copies = row->missileCount;
            if (!missileScripts_) missileScripts_ = std::make_unique<missile_motion::Scripts>();
            if (missileScripts_->has(visual->motionId) ||
                (failedMotionScripts_.count(visual->motionId) == 0 &&
                 missileScripts_->compile(visual->motionId, row->script))) {
                missile.motionId = visual->motionId;
            } else if (failedMotionScripts_.insert(visual->motionId).second) {
                LOG_WARNING("SpellVisual: SpellMissileMotion ", visual->motionId, " does not compile");
            }
        }
    }
    missile.missileCount = copies;
    // ADJUST_MISSILE (0x007022d0 hands it to 0x00700880): one flight to where
    // the target is now, timed by the server. Off the arc it flies straight,
    // homing, at the speed that lands it then.
    if (trajectory) {
        missile.adjusted = true;
        missile.facesDrawnMotion = true;
        missile.arcStart = missile.position;
        missile.arc = spell_missile::planMissileArc(trajectory->elevation, trajectory->castSpeed,
                                                    target - missile.position,
                                                    spellMissileRow(trajectory->spellMissileId),
                                                    trajectory->flightSeconds);
        if (!missile.arc.onArc) missile.speed = missile.arc.straightSpeed;
        flight = missile.arc.flightSeconds;
    }
    // A missile chasing a unit that runs from it takes longer than this, so
    // the cap is generous; it only stops one that can never arrive.
    missile.maxLifetime = flight * 3.0f + 5.0f;
    // 0x007022d0: the place it is let go, the distance from there and the
    // speed it starts at; a point as far behind it as the target is ahead
    // as where it was last drawn, so it first faces the target.
    missile.firePos = missile.position;
    missile.startDistance = glm::length(target - missile.position);
    missile.baseSpeed = missile.speed;
    missile.lastDrawn = missile.position - (target - missile.position);

    bool launched = false;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int32_t index = 0; index < copies; ++index) {
        ActiveMissile copy = missile;
        copy.missileIndex = index;
        // Rolled again for each copy as it is launched (0x007022d0).
        for (float& r : copy.rands) r = unit(missileRng_);
        copy.instanceId = m2Renderer_->createInstance(
            modelId, copy.position, spell_missile::facingEuler(target - copy.position), copy.scale);
        if (copy.instanceId == 0) break;
        m2Renderer_->restartInstanceAnimation(copy.instanceId);
        copy.soundHandle = startMissileSound(visual->soundId, copy.position);
        activeMissiles_.push_back(std::move(copy));
        launched = true;
    }
    LOG_DEBUG("SpellVisual: missile visualId=", visualId, " speed=", speed, " flight=", flight,
              "s model=", visual->modelPath, " target attach=", missile.targetAttachment, " copies=", copies);
    return launched;
}

// SpellMissileMotion.dbc (0x00ad4950): ID, Name, ScriptBody, Flags,
// MissileCount.
const SpellVisualSystem::MissileMotionRow* SpellVisualSystem::missileMotionRow(uint32_t id) {
    if (!missileMotionDbcLoaded_) {
        missileMotionDbcLoaded_ = true;
        auto* layouts = pipeline::getActiveDBCLayout();
        const pipeline::DBCFieldMap* l = layouts ? layouts->getLayout("SpellMissileMotion") : nullptr;
        auto dbc = cachedAssetManager_ && l ? cachedAssetManager_->loadDBC("SpellMissileMotion.dbc") : nullptr;
        const uint32_t scriptField = l ? l->tryField("ScriptBody") : 0xFFFFFFFFu;
        const uint32_t countField = l ? l->tryField("MissileCount") : 0xFFFFFFFFu;
        if (dbc && dbc->isLoaded() && scriptField < dbc->getFieldCount() && countField < dbc->getFieldCount()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                missileMotionRows_[dbc->getUInt32(i, 0)] =
                    MissileMotionRow{.script = dbc->getString(i, scriptField),
                                     .missileCount = dbc->getInt32(i, countField)};
            }
        }
    }
    auto it = missileMotionRows_.find(id);
    return it != missileMotionRows_.end() ? &it->second : nullptr;
}

glm::vec3 SpellVisualSystem::runMissileMotion(ActiveMissile& missile, const glm::vec3& target, glm::mat4& turn) {
    turn = glm::mat4(1.0f);
    if (missile.motionId == 0 || !missileScripts_) return missile.position;
    // The script works in the world's axes, this client's X and Y swapped.
    namespace cc = core::coords;
    const glm::vec3 pos = cc::renderToCanonical(missile.position);
    const glm::vec3 fire = cc::renderToCanonical(missile.firePos);
    const glm::vec3 aim = cc::renderToCanonical(target);
    missile_motion::Inputs in;
    in.progress = missile.progress;
    in.time = missile.elapsed;
    in.missileIndex = missile.missileIndex;
    in.missileCount = missile.missileCount;
    in.distanceToFirePos = glm::length(fire - pos);
    in.distanceToImpactPos = glm::length(aim - pos);
    in.startDistance = missile.startDistance;
    in.totalDistance = glm::length(aim - fire);
    in.rand1 = missile.rands[0];
    in.rand2 = missile.rands[1];
    in.rand3 = missile.rands[2];
    in.spellID = missile.spellId;
    const auto out = missileScripts_->run(missile.motionId, in);
    if (!out) {
        // 0x00701230: a script that raises is the end of the motion.
        missile.motionId = 0;
        return missile.position;
    }
    missile.speed = missile_motion::speedAfter(missile.speed, missile.baseSpeed, *out);
    turn = missile_motion::modelTurn(*out);
    return cc::canonicalToRender(missile_motion::drawnPosition(pos, fire, aim, *out));
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
            if (dbc->getFieldCount() > 27) {
                loaded.minDistance = dbc->getFloat(row, 26);
                loaded.cutoff = dbc->getFloat(row, 27);
            }
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
    return audio::AudioEngine::instance().playSound3DLooping(sound->data, position, sound->volume, 100.0f, 1.0f,
                                                             0.0f, audio::Channel::Effects);
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
        // Stopped by SMSG_SET_PROJECTILE_POSITION: once this step would leave
        // it further from the stop than it is, it lands where it is, its
        // target let go (0x007015d0, flag 0x400000; 0x00703410).
        if (it->hasStop && spell_missile::passedStop(it->position, step.position, it->stopPoint)) {
            step.arrived = true;
            step.position = it->position;
            it->targetInstanceId = 0;
            it->lastTarget = it->position;
        }
        if (!step.arrived && it->elapsed < it->maxLifetime) {
            it->position = step.position;
            // How far along it is (flag 0x1000, a motion's): the share of the
            // way from where it was let go that is behind it, or of its
            // time on an arc; never less than it was (0x006ffed0).
            if (it->hasMotion) {
                float along = 1.0f;
                if (it->adjusted && it->arc.onArc) {
                    along = it->arc.flightSeconds > 0.0f ? it->elapsed / it->arc.flightSeconds : 1.0f;
                } else if (const float total = glm::length(target - it->firePos); total > 1e-6f) {
                    along = 1.0f - glm::length(target - it->position) / total;
                }
                it->progress = std::max(it->progress, along);
            }
            glm::mat4 turn;
            const glm::vec3 drawn = runMissileMotion(*it, target, turn);
            // Flag 0x800: facing the way it was drawn moving, where it moved
            // at all (0x00701230). A move under 0.01 once it has faced some
            // way is that much of itself - its length times 100, at least
            // 0.05 - and the rest the way it faced last (flag 0x100000).
            glm::vec3 facing = travel;
            if (it->facesDrawnMotion) {
                glm::vec3 moved = drawn - it->lastDrawn;
                const float length = glm::length(moved);
                if (length > 2.3841858e-07f) {
                    if (length < 0.01f && it->hasDrawnDelta) {
                        const float share = std::max(length * 100.0f, 0.05f);
                        moved = moved * share + it->drawnDelta * (1.0f - share);
                    }
                    it->drawnDelta = moved;
                    it->hasDrawnDelta = true;
                    facing = moved;
                }
                it->lastDrawn = drawn;
            }
            m2Renderer_->setInstanceTransform(
                it->instanceId,
                placementModelMatrix(drawn, spell_missile::facingEuler(facing), it->scale) * turn);
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
                    holdStateKitForChain(aura);
                    if (!aura.awaitingChain) startStateKitOnUnit(guid, aura, aura.casterGuid);
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

void SpellVisualSystem::setProjectilePosition(uint64_t casterGuid, uint8_t castCount, const glm::vec3& point) {
    for (ActiveMissile& missile : activeMissiles_) {
        if (missile.casterGuid != casterGuid || missile.castCount != castCount) continue;
        missile.hasStop = true;
        missile.stopPoint = point;
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
        setEffectEventOwner(instanceId, renderInstanceId);
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

void SpellVisualSystem::setUnitAuraSlot(uint64_t unitGuid, uint32_t slot, uint32_t spellId, uint64_t casterGuid) {
    if (unitGuid == 0) return;
    auto& slots = unitAuraKits_[unitGuid].slots;
    auto it = slots.find(slot);
    const uint32_t old = it != slots.end() ? it->second : 0;
    if (old == spellId) return;
    if (spellId != 0) slots[slot] = spellId;
    else slots.erase(slot);
    auto visualOf = [&](uint32_t spell) { return spellVisualResolver_ ? spellVisualResolver_(spell) : 0u; };
    if (old != 0) {
        stepRiderPose(unitGuid, old, false);
        removeAuraStateKit(unitGuid, old, visualOf(old));
    }
    if (spellId != 0) {
        stepRiderPose(unitGuid, spellId, true);
        applyAuraStateKit(unitGuid, spellId, visualOf(spellId), casterGuid);
    }
}

void SpellVisualSystem::stepRiderPose(uint64_t unitGuid, uint32_t spellId, bool on) {
    if (!kitSpellResolver_ || !kitSpellResolver_(spellId).mounts) return;
    const uint32_t visualId = spellVisualResolver_ ? spellVisualResolver_(spellId) : 0u;
    if (visualId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto animIt = visualStateKitAnim_.find(visualId);
    const int32_t kitAnim = animIt != visualStateKitAnim_.end() ? animIt->second : -1;
    const uint32_t current = riderPose(unitGuid);
    const bool activePlayer = activePlayerResolver_ && activePlayerResolver_() == unitGuid;
    const uint32_t next = on ? mount_seat::riderPoseOnMountAura(current, kitAnim)
                             : mount_seat::riderPoseOnMountAuraGone(current, kitAnim, activePlayer);
    if (next == mount_seat::kRiderPoseMount) riderPoses_.erase(unitGuid);
    else riderPoses_[unitGuid] = next;
}

std::optional<SpellVisualSystem::KitAnimation> SpellVisualSystem::unitKitAnimation(uint64_t unitGuid) const {
    auto it = unitAuraKits_.find(unitGuid);
    if (it == unitAuraKits_.end()) return std::nullopt;
    // The unit's kits newest first (0x006f76c0 puts each at the head).
    const auto& auras = it->second.auras;
    for (auto a = auras.rbegin(); a != auras.rend(); ++a) {
        if (!a->playing || a->awaitingMissile || a->awaitingChain) continue;
        auto kitIt = kits_.find(a->kitId);
        if (kitIt == kits_.end() || kitIt->second.animId <= 0) continue;
        if (kitSpellResolver_ && kitSpellResolver_(a->spellId).mounts) continue;
        return KitAnimation{static_cast<uint32_t>(kitIt->second.animId), (kitIt->second.flags & 0xcu) == 0};
    }
    return std::nullopt;
}

std::optional<SpellVisualSystem::DeathKit> SpellVisualSystem::unitDeathKit(uint64_t unitGuid) const {
    auto it = unitAuraKits_.find(unitGuid);
    if (it == unitAuraKits_.end()) return std::nullopt;
    const auto& data = anim::animationData();
    const auto& auras = it->second.auras;
    for (auto a = auras.rbegin(); a != auras.rend(); ++a) {
        if (!a->playing || a->awaitingMissile || a->awaitingChain) continue;
        auto kitIt = kits_.find(a->kitId);
        if (kitIt == kits_.end() || kitIt->second.animId < 0) continue;
        const auto anim = static_cast<uint32_t>(kitIt->second.animId);
        // 0x0071dde0: dying, dead, drowning, the death's start and end.
        if (upper_body::isDeath(data.empty() ? anim : data.behavior(anim)))
            return DeathKit{anim, kitIt->second.flags};
    }
    return std::nullopt;
}

std::optional<SpellVisualSystem::CastLoop> SpellVisualSystem::castLoop(uint32_t castSpellId, uint32_t channelSpellId) {
    if (castSpellId == 0 && channelSpellId == 0) return std::nullopt;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    const auto visualOf = [this](uint32_t spell) { return spellVisualResolver_ ? spellVisualResolver_(spell) : 0u; };
    uint32_t kitId = 0;
    if (castSpellId != 0) {
        auto it = visualKits_.find(visualOf(castSpellId));
        kitId = it != visualKits_.end() ? it->second.precast : 0;
    } else {
        auto it = visualAuraKits_.find(visualOf(channelSpellId));
        kitId = it != visualAuraKits_.end() ? it->second.channelKit : 0;
    }
    auto kitIt = kitId ? kits_.find(kitId) : kits_.end();
    if (kitIt == kits_.end() || kitIt->second.animId <= 0) return std::nullopt;
    return CastLoop{static_cast<uint32_t>(kitIt->second.animId), kitIt->second.flags};
}

SpellVisualSystem::CastAnimations SpellVisualSystem::castAnimations(uint32_t spellId) {
    CastAnimations out;
    if (spellId == 0) return out;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = visualKits_.find(spellVisualResolver_ ? spellVisualResolver_(spellId) : 0u);
    if (it == visualKits_.end()) return out;
    if (auto kit = kits_.find(it->second.precast); kit != kits_.end()) {
        out.precastStart = kit->second.startAnimId;
        out.precastAnim = kit->second.animId;
        out.precastFlags = kit->second.flags;
    }
    if (auto kit = kits_.find(it->second.cast); kit != kits_.end()) out.castAnim = kit->second.animId;
    return out;
}

uint32_t SpellVisualSystem::riderPose(uint64_t unitGuid) const {
    auto it = riderPoses_.find(unitGuid);
    return it != riderPoses_.end() ? it->second : mount_seat::kRiderPoseMount;
}

void SpellVisualSystem::setUnitAuraSlots(uint64_t unitGuid, const std::vector<AuraSlotSpell>& slotSpells) {
    if (unitGuid == 0) return;
    std::vector<uint32_t> emptied;
    if (auto it = unitAuraKits_.find(unitGuid); it != unitAuraKits_.end()) {
        for (const auto& [slot, spell] : it->second.slots) {
            const bool named = std::any_of(slotSpells.begin(), slotSpells.end(),
                                           [slot = slot](const AuraSlotSpell& entry) { return entry.slot == slot; });
            if (!named) emptied.push_back(slot);
        }
    }
    for (uint32_t slot : emptied) setUnitAuraSlot(unitGuid, slot, 0);
    for (const AuraSlotSpell& entry : slotSpells) setUnitAuraSlot(unitGuid, entry.slot, entry.spellId, entry.casterGuid);
}

void SpellVisualSystem::applyAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId,
                                          uint64_t casterGuid) {
    if (unitGuid == 0 || spellId == 0 || visualId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto visualIt = visualAuraKits_.find(visualId);
    if (visualIt == visualAuraKits_.end() || visualIt->second.stateKit == 0) return;
    const VisualAuraKits& visual = visualIt->second;
    UnitAuraKits& unit = unitAuraKits_[unitGuid];
    AuraKit aura{.spellId = spellId, .visualId = visualId, .kitId = visual.stateKit,
                 .unarmedOnly = (visual.flags & spell_kit::kVisualFlagUnarmedStateKit) != 0,
                 .casterGuid = casterGuid};
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
    // Then 0x00745230: held for a chain's pulse (0x0072af60), or played -
    // its chains from the aura's caster where the kit names another unit
    // (flag 0x1000 with the slot's caster, 0x00724820).
    if (!aura.awaitingMissile) {
        holdStateKitForChain(aura);
        if (!aura.awaitingChain) startStateKitOnUnit(unitGuid, aura, casterGuid);
    }
    unit.auras.push_back(std::move(aura));
    // Then 0x00720400(1, 1) for a Flags 8 visual.
    if (unit.auras.back().unarmedOnly) stepUnarmedKits(unitGuid, unit, true);
}

void SpellVisualSystem::removeAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId) {
    if (unitGuid == 0 || spellId == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    removeUnitSpellEffects(unitGuid, spellId);
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
        if (kit.charProc[k] == spell_kit::kCharProcSwingTrail && renderInstanceId != 0) {
            // 0x007265c0 case 8, applied after the kit's procedures by
            // 0x0073b140 where its time is not 0 (0x00715ba0).
            const auto start = swing_trail::startFor(kit.charParam[k][0], kit.charParam[k][2], kit.charParam[k][3]);
            if (start.durationMs != 0) startSwingTrails(renderInstanceId, start);
            continue;
        }
        if (kit.charProc[k] == spell_kit::kCharProcWearItem && renderInstanceId != 0) {
            // 0x007265c0 case 17 (0x006f8650, 0x006f8600): Item.dbc's row
            // for ParamOne, its display in its inventory type's slot.
            const uint64_t unitGuid = instanceUnitResolver_ ? instanceUnitResolver_(renderInstanceId) : 0;
            if (!wornItemSink_ || unitGuid == 0 || !cachedAssetManager_) continue;
            auto itemDbc = cachedAssetManager_->loadDBCOptional("Item.dbc");
            // WotLK's eight columns: display at 5, inventory type at 6.
            const auto itemId = static_cast<uint32_t>(static_cast<int32_t>(kit.charParam[k][1]));
            const int32_t row = itemDbc && itemDbc->getFieldCount() >= 8 ? itemDbc->findRecordById(itemId) : -1;
            if (row < 0) continue;
            const uint32_t display = itemDbc->getUInt32(static_cast<uint32_t>(row), 5);
            const uint32_t inventoryType = itemDbc->getUInt32(static_cast<uint32_t>(row), 6);
            const int equipSlot =
                spell_kit::equipSlotForComponentSlot(spell_kit::componentSlotForInventoryType(inventoryType));
            if (equipSlot < 0 || display == 0) continue;
            wornItemSink_(unitGuid, equipSlot, display, static_cast<uint8_t>(inventoryType));
            wornItems_.push_back({.unitGuid = unitGuid, .spellId = spellId, .equipSlot = equipSlot});
            continue;
        }
        if (kit.charProc[k] == spell_kit::kCharProcTimedAlpha && renderInstanceId != 0) {
            // 0x007265c0 case 15 (0x0071a940): out over half a second, held,
            // then back.
            if (const auto fade = spell_kit::timedAlpha(kit.charParam[k], 1.0f, colourClockMs_)) {
                if (auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
                    charRenderer->setInstanceKitAlpha(renderInstanceId, fade->alpha,
                                                      static_cast<float>(fade->inMs) * 0.001f);
                    timedAlphas_[renderInstanceId] = *fade;
                }
            }
            continue;
        }
        if (kit.charProc[k] == spell_kit::kCharProcFreeze && renderInstanceId != 0) {
            // 0x007265c0 case 11 (0x006f80b0): the animation held while the
            // unit's effects of the spell last.
            auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
            const uint64_t unitGuid = instanceUnitResolver_ ? instanceUnitResolver_(renderInstanceId) : 0;
            if (charRenderer && unitGuid != 0) {
                // The mount first (+0x98c), then the unit (+0xb4), each at
                // ParamZero into its own sequence; +0xd8 and +0xd4 keep
                // whether each still has to be let go.
                const uint32_t mount = unitMountInstanceResolver_ ? unitMountInstanceResolver_(unitGuid) : 0u;
                const bool mountWas =
                    mount == 0 ||
                    charRenderer->setInstanceAnimationFrozen(mount, true, spell_kit::freezeAtMs(kit.charParam[k][0]));
                const bool was = charRenderer->setInstanceAnimationFrozen(
                    renderInstanceId, true, spell_kit::freezeAtMs(kit.charParam[k][0]));
                animationHolds_.push_back({.unitGuid = unitGuid,
                                           .spellId = spellId,
                                           .renderInstanceId = renderInstanceId,
                                           .wasHeld = was,
                                           .mountWasHeld = mount != 0 && mountWas});
            }
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
    // 0x0073dab0: a case 15 fade, its time up, back to the unit's own alpha.
    for (auto it = timedAlphas_.begin(); it != timedAlphas_.end();) {
        if (!spell_kit::timedAlphaOver(it->second, colourClockMs_)) {
            ++it;
            continue;
        }
        charRenderer->setInstanceKitAlpha(it->first, 1.0f, static_cast<float>(it->second.backMs) * 0.001f);
        it = timedAlphas_.erase(it);
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

const spell_chain::ChainEffect* SpellVisualSystem::chainEffect(uint32_t id) {
    if (!chainEffectsLoaded_) {
        auto* am = cachedAssetManager_ ? cachedAssetManager_ : core::Application::getInstance().getAssetManager();
        if (!am || !am->isInitialized()) return nullptr;
        cachedAssetManager_ = am;
        chainEffectsLoaded_ = true;  // a real attempt follows
        auto dbc = cachedAssetManager_->loadDBCOptional("SpellChainEffects.dbc");
        if (dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                auto row = spell_chain::parseChainEffect(dbc->getRecord(i), dbc->getRecordSize(),
                                                         [&](uint32_t offset) { return dbc->getStringByOffset(offset); });
                if (!row || row->id == 0) continue;
                // The Combo string's own bytes (0x007fc5f0 reads them as words).
                // parseChainEffect has checked the record holds the tail.
                const uint32_t comboOffset =
                    spell_chain::chainEffectTail(dbc->getRecord(i), dbc->getRecordSize()).comboString;
                if (comboOffset < dbc->getStringBlockSize() && dbc->getStringBlockData())
                    row->comboWords = spell_chain::comboWordsAt(dbc->getStringBlockData() + comboOffset,
                                                                dbc->getStringBlockSize() - comboOffset);
                chainEffects_[row->id] = std::move(*row);
            }
        }
        LOG_INFO("SpellVisual: loaded ", chainEffects_.size(), " chain effects");
    }
    auto it = id ? chainEffects_.find(id) : chainEffects_.end();
    return it != chainEffects_.end() ? &it->second : nullptr;
}

bool SpellVisualSystem::visualHasChainKit(uint32_t visualId) const {
    if (visualId == 0) return false;
    auto runsChain = [this](uint32_t kitId) {
        auto it = kitId ? kits_.find(kitId) : kits_.end();
        return it != kits_.end() && spell_kit::kitRunsChain(it->second.charProc);
    };
    if (auto it = visualKits_.find(visualId); it != visualKits_.end()) {
        const VisualKits& k = it->second;
        if (runsChain(k.cast) || runsChain(k.impact) || runsChain(k.casterImpact) || runsChain(k.targetImpact))
            return true;
    }
    if (auto it = visualAuraKits_.find(visualId); it != visualAuraKits_.end())
        return runsChain(it->second.stateKit) || runsChain(it->second.channelKit);
    return false;
}

void SpellVisualSystem::releaseWaitingKits(uint64_t unitGuid, uint32_t spellId, int32_t counter) {
    if (unitGuid == 0) return;
    // Taken out first: a kit played here may start chains of its own.
    std::vector<WaitingKit> due;
    for (auto it = waitingKits_.begin(); it != waitingKits_.end();) {
        if (it->mode == 0 && it->unitGuid == unitGuid && it->spellId == spellId && it->counter == counter) {
            due.push_back(*it);
            it = waitingKits_.erase(it);
        } else {
            ++it;
        }
    }
    // A state or channel kit waits with counter -1 (0x00724820, 0x0072bc70).
    if (counter == -1) {
        if (auto unitIt = unitAuraKits_.find(unitGuid); unitIt != unitAuraKits_.end()) {
            for (AuraKit& aura : unitIt->second.auras) {
                if (!aura.awaitingChain || aura.spellId != spellId) continue;
                aura.awaitingChain = false;
                startStateKitOnUnit(unitGuid, aura, aura.casterGuid);
            }
        }
    }
    for (const WaitingKit& w : due) {
        // Played as it was asked for, and not held again (+0x20 1).
        const uint32_t instance = !w.hasPlace && unitInstanceResolver_ ? unitInstanceResolver_(unitGuid) : 0u;
        if (!w.hasPlace && instance == 0) continue;
        glm::vec3 at = w.place;
        if (instance != 0 && renderer_ && renderer_->getCharacterRenderer())
            renderer_->getCharacterRenderer()->getInstancePosition(instance, at);
        playKitOnUnit(w.kitId, w.type, instance, at, w.hasPlace ? &w.place : nullptr, w.castTimeMs, w.spellId,
                      {.unitGuid = unitGuid, .counter = w.counter, .replay = true});
    }
}

void SpellVisualSystem::expireWaitingKits() {
    // Taken out first: a kit played here may queue more.
    std::vector<WaitingKit> due;
    for (auto it = waitingKits_.begin(); it != waitingKits_.end();) {
        if (static_cast<int32_t>(colourClockMs_ - it->deadlineMs) >= 0) {
            if (it->mode == 1) due.push_back(*it);
            it = waitingKits_.erase(it);
        } else {
            ++it;
        }
    }
    for (const WaitingKit& w : due) {
        // 0x00728140 mode 1: played by its unit, not held again (+0x20 1).
        playKitOnUnit(w.kitId, w.type, 0, w.place, &w.place, w.castTimeMs, w.spellId,
                      {.unitGuid = w.unitGuid, .counter = w.counter, .replay = true});
    }
}

const SpellVisualSystem::VisualEnds* SpellVisualSystem::visualEndsForSpell(uint32_t spellId) const {
    const uint32_t visualId = spellVisualResolver_ && spellId ? spellVisualResolver_(spellId) : 0u;
    auto it = visualId ? visualEnds_.find(visualId) : visualEnds_.end();
    return it != visualEnds_.end() ? &it->second : nullptr;
}

void SpellVisualSystem::startKitChains(const KitRecord& kit, uint64_t unitGuid, uint32_t spellId,
                                       uint64_t otherSource) {
    if (unitGuid == 0) return;
    for (uint32_t k = 0; k < 4; ++k) {
        if (!spell_kit::isChainProc(kit.charProc[k])) continue;
        const auto& param = kit.charParam[k];
        // 0x0088b9c0 chops ParamZero to the row's id.
        const spell_chain::ChainEffect* effect = chainEffect(static_cast<uint32_t>(static_cast<int32_t>(param[0])));
        if (!effect) continue;
        const auto castIt = castTargets_.find(unitGuid);
        const CastTargets* cast = castIt != castTargets_.end() ? &castIt->second : nullptr;
        const auto channelIt = unitChannels_.find(unitGuid);
        const bool channelMatches = channelIt != unitChannels_.end() && channelIt->second.spellId == spellId &&
                                    channelIt->second.object != 0;
        ChainObject chain;
        chain.effect = effect;
        chain.spellId = spellId;
        uint64_t source = unitGuid;
        std::vector<uint64_t> targets;
        bool toPlace = false;
        switch (spell_chain::chainTargets(otherSource != 0 && otherSource != unitGuid, cast && cast->place,
                                          channelMatches, cast ? cast->hits.size() : 0)) {
            case spell_chain::ChainTargets::FromOther:
                source = otherSource;
                targets.push_back(unitGuid);
                break;
            case spell_chain::ChainTargets::Place:
                chain.place = cast->place;
                toPlace = true;
                break;
            case spell_chain::ChainTargets::Channel:
                targets.push_back(channelIt->second.object);
                break;
            case spell_chain::ChainTargets::Hits:
                targets = cast->hits;
                break;
            case spell_chain::ChainTargets::None:
                continue;
        }
        // 0x007fc5f0: the spell's visual's ends.
        if (const VisualEnds* ends = visualEndsForSpell(spellId)) {
            chain.sourceAttachment = ends->sourceAttachment;
            chain.castOffset = ends->castOffset;
            chain.impactOffset = ends->impactOffset;
        }
        chain.nodes.push_back(source);
        if (chain.place) {
            // One node with no unit; the place carries the impact offset,
            // turned as the unit faces (flag 2).
            chain.nodes.push_back(0);
            CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
            const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(source) : 0;
            glm::mat4 frame(1.0f);
            if (charRenderer && instance) charRenderer->getInstanceFrame(instance, frame);
            chain.place = *chain.place + glm::mat3(frame) * chain.impactOffset;
        } else {
            chain.nodes.insert(chain.nodes.end(), targets.begin(), targets.end());
        }
        // The unit's chain counter (+0xf58) names this kit's bolts, two on
        // for the next.
        int32_t& unitCounter = unitChainCounters_[unitGuid];
        const int32_t counter = unitCounter;
        unitCounter += 2;
        // ParamThree: every bolt from the unit; ParamTwo: held by its effect.
        chain.held = param[2] != 0.0f;
        chain.ownerUnit = unitGuid;
        chain.ownerSpell = spellId;
        // 0x007fc5f0: a LightningObject for the row, then one for each row
        // its Combo names (the first row's words, twelve at most), while
        // the effect holds fewer than twelve (+0x4c).
        const spell_chain::ChainEffect* first = effect;
        uint32_t heldCount = 0;
        for (size_t word = 0;; ++word) {
            ChainObject object = chain;
            object.effect = effect;
            object.bolts = spell_chain::planBolts(*effect, object.nodes.size() - 1, param[3] != 0.0f,
                                                  colourClockMs_, object.endMs, counter);
            object.lightning.resize(object.bolts.size());
            chains_.push_back(std::move(object));
            if (chain.held) ++heldCount;
            if (heldCount >= 12 || word >= first->comboWords.size()) break;
            effect = chainEffect(spell_chain::comboRow(first->comboWords[word]));
            if (!effect) break;
        }
        // 0x007265c0 case 0 at a place: the visual's ImpactAreaKit (+0x60)
        // there, the unit's and named by the counter - unless it is this
        // kit - to wait for the bolt's pulse.
        const uint32_t visualId = spellVisualResolver_ ? spellVisualResolver_(spellId) : 0u;
        const auto kitsIt = visualId ? visualKits_.find(visualId) : visualKits_.end();
        if (toPlace && kitsIt != visualKits_.end() && kitsIt->second.impactArea != 0 &&
            kitsIt->second.impactArea != kit.id) {
            const glm::vec3 at = *cast->place;
            playKitOnUnit(kitsIt->second.impactArea, spell_kit::KitType::Area, 0, at, &at, 0, spellId,
                          {.unitGuid = unitGuid, .counter = counter});
        }
    }
}

void SpellVisualSystem::releaseChains(uint64_t unitGuid, uint32_t spellId) {
    // 0x007fc990: no longer held, its time up now.
    for (ChainObject& chain : chains_) {
        if (!chain.held || chain.ownerUnit != unitGuid || chain.ownerSpell != spellId) continue;
        chain.held = false;
        chain.endMs = colourClockMs_;
    }
}

void SpellVisualSystem::removeUnitSpellEffects(uint64_t unitGuid, uint32_t spellId) {
    releaseChains(unitGuid, spellId);
    // 0x006f87c0: a mount transition of the spell ends, the unit mounted
    // where its rider arrived.
    for (size_t i = 0; i < mountTransitions_.size();) {
        if (mountTransitions_[i].unitGuid == unitGuid && mountTransitions_[i].spellId == spellId)
            endMountTransition(i, false);
        else
            ++i;
    }
    // 0x006f8700: a worn item off again, the unit's own back (0x00723730).
    for (auto it = wornItems_.begin(); it != wornItems_.end();) {
        if (it->unitGuid != unitGuid || it->spellId != spellId) {
            ++it;
            continue;
        }
        if (wornItemSink_) wornItemSink_(unitGuid, it->equipSlot, 0, 0);
        it = wornItems_.erase(it);
    }
    // A held animation runs again, unless it was held before (0x006f80b0's
    // +0xd4).
    for (auto it = animationHolds_.begin(); it != animationHolds_.end();) {
        if (it->unitGuid != unitGuid || it->spellId != spellId) {
            ++it;
            continue;
        }
        if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
            // 0x006f87c0: the mount the unit has now, then the unit.
            const uint32_t mount = unitMountInstanceResolver_ ? unitMountInstanceResolver_(unitGuid) : 0u;
            if (!it->mountWasHeld && mount != 0) charRenderer->setInstanceAnimationFrozen(mount, false);
            if (!it->wasHeld) charRenderer->setInstanceAnimationFrozen(it->renderInstanceId, false);
        }
        it = animationHolds_.erase(it);
    }
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
}

void SpellVisualSystem::setUnitCastTargets(uint64_t unitGuid, uint32_t spellId, const std::vector<uint64_t>& hits,
                                           const std::optional<glm::vec3>& destination) {
    if (unitGuid == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    CastTargets& cast = castTargets_[unitGuid];
    // 0x00724f50: the hits less the unit itself, kept in the order they came.
    cast.hits.clear();
    for (uint64_t guid : hits) {
        if (guid != 0 && guid != unitGuid) cast.hits.push_back(guid);
    }
    // 0x00715400: the place, unless the visual's Flags 0x1 hands the chain
    // to the hits.
    const VisualEnds* ends = visualEndsForSpell(spellId);
    const bool hitsInstead = ends && (ends->flags & 0x1u) != 0 && !hits.empty();
    cast.place = hitsInstead ? std::nullopt : destination;
}

void SpellVisualSystem::setUnitChannel(uint64_t unitGuid, uint32_t spellId, uint64_t channelObject) {
    if (unitGuid == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    UnitChannel& channel = unitChannels_[unitGuid];
    channel.object = channelObject;
    if (channel.spellId == spellId) {
        if (spellId == 0) unitChannels_.erase(unitGuid);
        return;
    }
    const uint32_t old = channel.spellId;
    channel.spellId = spellId;
    // 0x0073eb50: the old channel's effects leave the unit.
    if (old != 0) removeUnitSpellEffects(unitGuid, old);
    if (spellId == 0) {
        unitChannels_.erase(unitGuid);
        return;
    }
    applyChannelKit(unitGuid, spellId);
}

void SpellVisualSystem::applyChannelKit(uint64_t unitGuid, uint32_t spellId) {
    // 0x0072bc70: the visual's ChannelKit, type 2.
    const uint32_t visualId = spellVisualResolver_ ? spellVisualResolver_(spellId) : 0;
    auto visualIt = visualId ? visualAuraKits_.find(visualId) : visualAuraKits_.end();
    if (visualIt == visualAuraKits_.end() || visualIt->second.channelKit == 0) return;
    auto kitIt = kits_.find(visualIt->second.channelKit);
    if (kitIt == kits_.end()) return;
    AuraKit aura{.spellId = spellId, .visualId = visualId, .kitId = kitIt->first};
    holdStateKitForChain(aura);
    if (!aura.awaitingChain) startStateKitOnUnit(unitGuid, aura, 0);
    unitAuraKits_[unitGuid].auras.push_back(std::move(aura));
}

void SpellVisualSystem::startStateKitOnUnit(uint64_t unitGuid, const AuraKit& aura, uint64_t otherSource) {
    auto kitIt = kits_.find(aura.kitId);
    if (kitIt == kits_.end()) return;
    const uint32_t instance = unitInstanceResolver_ ? unitInstanceResolver_(unitGuid) : 0;
    if (instance != 0) {
        glm::vec3 unitPos;
        if (renderer_ && renderer_->getCharacterRenderer() &&
            renderer_->getCharacterRenderer()->getInstancePosition(instance, unitPos))
            playKitShake(aura.kitId, unitPos);
        playKitColourFade(aura.kitId, instance, aura.spellId);
    }
    startKitChains(kitIt->second, unitGuid, aura.spellId, otherSource);
    // 0x007265c0 runs for every kit type: 16 starts the mount transition
    // from a state kit too.
    for (uint32_t k = 0; k < 4 && instance != 0; ++k) {
        if (kitIt->second.charProc[k] == spell_kit::kCharProcMountTransition)
            startMountTransition(instance, aura.spellId, spell_kit::KitType::State, 0);
    }
}

void SpellVisualSystem::holdStateKitForChain(AuraKit& aura) {
    // 0x00724820 and 0x0072bc70 ask 0x00745230 with counter -1, so the
    // kit waits for a pulse at either end of a bolt (0x0072af60).
    auto kitIt = kits_.find(aura.kitId);
    if (kitIt == kits_.end() || (kitIt->second.flags & spell_kit::kKitFlagWaitsForChain) == 0 ||
        !visualHasChainKit(aura.visualId))
        return;
    aura.awaitingChain = true;
    aura.chainDeadlineMs = colourClockMs_ + spell_kit::kWaitingKitMs;
}

void SpellVisualSystem::expireStateKitWaits() {
    for (auto& [guid, unit] : unitAuraKits_) {
        std::erase_if(unit.auras, [this](const AuraKit& aura) {
            return aura.awaitingChain && static_cast<int32_t>(colourClockMs_ - aura.chainDeadlineMs) >= 0;
        });
    }
}

void SpellVisualSystem::updateChainTargets(uint64_t unitGuid, uint32_t spellId, const std::vector<uint64_t>& targets,
                                           uint64_t channelObject) {
    if (unitGuid == 0) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    // 0x00724f50: the hits less the unit; the place stays as it was.
    CastTargets& cast = castTargets_[unitGuid];
    cast.hits.clear();
    for (uint64_t guid : targets) {
        if (guid != 0 && guid != unitGuid) cast.hits.push_back(guid);
    }
    auto channel = unitChannels_.find(unitGuid);
    if (channel != unitChannels_.end() && channelObject != 0) channel->second.object = channelObject;
    if (spellId == 0) return;
    // 0x0073eb50: the spell's effects leave the unit, its chains with them,
    // and while it channels its channel's kit goes on again, to the new
    // targets (0x0072bc70).
    removeUnitSpellEffects(unitGuid, spellId);
    if (channel != unitChannels_.end() && channel->second.spellId != 0)
        applyChannelKit(unitGuid, channel->second.spellId);
}

void SpellVisualSystem::setUnitChannels(const std::vector<ChannelState>& channels) {
    std::vector<uint64_t> ended;
    for (const auto& [guid, channel] : unitChannels_) {
        const bool named = std::any_of(channels.begin(), channels.end(),
                                       [guid = guid](const ChannelState& c) { return c.unitGuid == guid; });
        if (!named) ended.push_back(guid);
    }
    for (uint64_t guid : ended) setUnitChannel(guid, 0, 0);
    for (const ChannelState& c : channels) setUnitChannel(c.unitGuid, c.spellId, c.channelObject);
}

bool SpellVisualSystem::unitMiddle(uint32_t renderInstanceId, glm::vec3& middle, glm::mat4& frame) const {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || renderInstanceId == 0 || !charRenderer->getInstanceFrame(renderInstanceId, frame))
        return false;
    // 0x00717ad0: the GeoBox's height times the unit's scale; the model's
    // own box where the unit has no CreatureModelData row.
    float height = unitHeight_ ? unitHeight_(renderInstanceId) : 0.0f;
    if (!(height > 0.0f)) {
        const pipeline::M2Model* model = charRenderer->getInstanceModelData(renderInstanceId);
        height = model && model->hasVertexBox ? model->vertexBoxMax.z - model->vertexBoxMin.z : 0.0f;
    }
    middle = spell_chain::unitMiddle(glm::vec3(frame[3]), height * glm::length(glm::vec3(frame[0])));
    return true;
}

bool SpellVisualSystem::chainSourcePoint(uint32_t renderInstanceId, const ChainObject& chain, glm::vec3& out) const {
    glm::vec3 middle;
    glm::mat4 frame;
    if (!unitMiddle(renderInstanceId, middle, frame)) return false;
    CharacterRenderer* charRenderer = renderer_->getCharacterRenderer();
    // 0x007faa40: MissileAttachment with the cast offset, else the cast
    // model's $CSL event, else the middle carried by the offset.
    glm::mat4 attach;
    if (chain.sourceAttachment >= 0 &&
        charRenderer->getAttachmentTransform(renderInstanceId, static_cast<uint32_t>(chain.sourceAttachment), attach)) {
        out = glm::vec3(attach * glm::vec4(chain.castOffset, 1.0f));
        return true;
    }
    if (charRenderer->getEventPosition(renderInstanceId, 0x4C534324u /* $CSL */, out)) return true;
    out = middle + glm::mat3(frame) * chain.castOffset;
    return true;
}

bool SpellVisualSystem::chainUnitPoint(uint32_t renderInstanceId, uint32_t spellId, glm::vec3& out) const {
    glm::vec3 middle;
    glm::mat4 frame;
    if (!unitMiddle(renderInstanceId, middle, frame)) return false;
    CharacterRenderer* charRenderer = renderer_->getCharacterRenderer();
    glm::mat4 attach;
    const VisualEnds* ends = visualEndsForSpell(spellId);
    if (!ends) {
        // 0x007fabf0 with no spell: the chest (34).
        out = charRenderer->getAttachmentTransform(renderInstanceId, 34, attach) ? glm::vec3(attach[3]) : middle;
        return true;
    }
    // MissileDestinationAttachment with the impact offset, else the middle
    // carried by it.
    if (ends->destinationAttachment >= 0 &&
        charRenderer->getAttachmentTransform(renderInstanceId, static_cast<uint32_t>(ends->destinationAttachment),
                                             attach)) {
        out = glm::vec3(attach * glm::vec4(ends->impactOffset, 1.0f));
        return true;
    }
    out = middle + glm::mat3(frame) * ends->impactOffset;
    return true;
}

void SpellVisualSystem::updateChains(float deltaTime) {
    const uint32_t now = colourClockMs_;
    // Played after the walk: a kit played may start chains of its own.
    struct Release {
        uint64_t unit;
        uint32_t spellId;
        int32_t counter;
    };
    std::vector<Release> releases;
    for (auto it = chains_.begin(); it != chains_.end();) {
        ChainObject& chain = *it;
        for (size_t b = 0; b < chain.bolts.size(); ++b) {
            const spell_chain::Bolt& bolt = chain.bolts[b];
            auto& lightning = chain.lightning[b];
            // 0x007fae90: a bolt that shows has its ends found and its
            // lightning made; it shows only with a source and somewhere to go.
            if (spell_chain::boltShows(bolt, chain.held, now)) {
                const uint64_t from = bolt.from < chain.nodes.size() ? chain.nodes[bolt.from] : 0;
                const uint64_t to = bolt.to < chain.nodes.size() ? chain.nodes[bolt.to] : 0;
                glm::vec3 start(0.0f), end(0.0f);
                bool haveStart = false;
                bool haveEnd = false;
                glm::mat4 objectFrame;
                if (const uint32_t inst = from && unitInstanceResolver_ ? unitInstanceResolver_(from) : 0) {
                    haveStart = bolt.from == 0 ? chainSourcePoint(inst, chain, start)
                                               : chainUnitPoint(inst, chain.spellId, start);
                } else if (from && objectFrameResolver_ && objectFrameResolver_(from, objectFrame)) {
                    start = glm::vec3(objectFrame * glm::vec4(chain.castOffset, 1.0f));
                    haveStart = true;
                }
                if (const uint32_t inst = to && unitInstanceResolver_ ? unitInstanceResolver_(to) : 0) {
                    haveEnd = chainUnitPoint(inst, chain.spellId, end);
                } else if (to && objectFrameResolver_ && objectFrameResolver_(to, objectFrame)) {
                    end = glm::vec3(objectFrame * glm::vec4(chain.impactOffset, 1.0f));
                    haveEnd = true;
                } else if (chain.place) {
                    end = *chain.place;
                    haveEnd = true;
                }
                if (!lightning) {
                    lightning.emplace();
                    lightning->init(chain.effect, chainRng_);
                }
                lightning->setEnds(start, end);
                lightning->setVisible(haveStart && haveEnd);
                // As its pulse was when last laid out: the kits waiting at
                // either end play (the objects' virtual 0xc0).
                const auto release =
                    spell_chain::pulseRelease(lightning->pulseAtSource(), lightning->pulseAtEnd(), to != 0);
                if (from != 0) {
                    if (release.source) releases.push_back({from, chain.spellId, -1});
                    if (release.target) releases.push_back({to, chain.spellId, -1});
                    if (release.sourceCounter) releases.push_back({from, chain.spellId, bolt.counter});
                }
            }
            if (!chain.held && static_cast<int32_t>(now - bolt.endMs) >= 0) lightning.reset();
        }
        if (!chain.held && static_cast<int32_t>(now - chain.endMs) >= 0) {
            it = chains_.erase(it);
        } else {
            ++it;
        }
    }
    for (const Release& r : releases) releaseWaitingKits(r.unit, r.spellId, r.counter);
    // 0x009ab730: every lightning a frame on.
    for (ChainObject& chain : chains_) {
        for (auto& lightning : chain.lightning) {
            if (lightning) lightning->update(deltaTime, chainRng_);
        }
    }
}

void SpellVisualSystem::startMountTransition(uint32_t renderInstanceId, uint32_t spellId, spell_kit::KitType type,
                                             uint32_t castTimeMs) {
    const uint64_t unitGuid = instanceUnitResolver_ ? instanceUnitResolver_(renderInstanceId) : 0;
    if (unitGuid == 0 || spellId == 0 || !mountDisplayResolver_) return;
    // 0x007265c0 case 16: only a unit on no mount (0x0051a230, +0x98c).
    if (unitMountedQuery_ && unitMountedQuery_(unitGuid)) return;
    if (unitMountInstanceResolver_ && unitMountInstanceResolver_(unitGuid) != 0) return;
    // 0x006f9670: the spell's mount, or none to make.
    const std::optional<uint32_t> display = mountDisplayResolver_(spellId);
    if (!display) return;
    // The unit holds one (+0x9c4, 0x00715670): a new one takes the old's place.
    for (size_t i = 0; i < mountTransitions_.size();) {
        if (mountTransitions_[i].unitGuid != unitGuid) {
            ++i;
            continue;
        }
        if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
            if (mountTransitions_[i].modelInstance) charRenderer->removeInstance(mountTransitions_[i].modelInstance);
        }
        releaseMountRider(mountTransitions_[i]);
        mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    MountTransition t;
    t.unitGuid = unitGuid;
    t.spellId = spellId;
    t.precast = type == spell_kit::KitType::Precast;
    t.castEndMs = colourClockMs_ + castTimeMs;
    t.displayId = *display;
    t.riderInstance = renderInstanceId;
    // 0x007fbe00: where the unit stands, its ground normal, now.
    t.state = mount_transition::begin(colourClockMs_, glm::vec3(0.0f, 0.0f, 1.0f));
    mountTransitions_.push_back(t);
}

void SpellVisualSystem::onUnitMounted(uint64_t unitGuid) {
    // 0x0073d5d0: the unit's own mount now; its transition's effect ends
    // (0x006f87c0) - the hand over (0x007412b0) a mount the unit has.
    for (size_t i = 0; i < mountTransitions_.size();) {
        if (mountTransitions_[i].unitGuid != unitGuid) {
            ++i;
            continue;
        }
        if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
            if (mountTransitions_[i].modelInstance) charRenderer->removeInstance(mountTransitions_[i].modelInstance);
        }
        releaseMountRider(mountTransitions_[i]);
        mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void SpellVisualSystem::endMountTransition(size_t index, bool resync) {
    if (index >= mountTransitions_.size()) return;
    const MountTransition t = mountTransitions_[index];
    mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(index));
    if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        if (t.modelInstance) charRenderer->removeInstance(t.modelInstance);
    }
    releaseMountRider(t);
    if (!mountSink_ || t.displayId == 0 || t.modelInstance == 0) return;
    // 0x007412b0: arrived, the unit is mounted on the display at once -
    // and where the cast is over, 0x007fec00 sets it to the server's right
    // after, so the server's it is.
    if (!(t.state.flags & mount_transition::kArrived)) return;
    const uint32_t display = resync && unitMountFieldQuery_ ? unitMountFieldQuery_(t.unitGuid) : t.displayId;
    mountSink_(t.unitGuid, display);
}

void SpellVisualSystem::releaseMountRider(const MountTransition& t) {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || t.riderInstance == 0) return;
    charRenderer->setInstanceRenderOffset(t.riderInstance, glm::vec3(0.0f));
    charRenderer->setInstanceHoldsLastFrame(t.riderInstance, false);
    if (t.riderTilted) {
        if (const auto rotation = charRenderer->getInstanceRotation(t.riderInstance))
            charRenderer->setInstanceRotation(t.riderInstance, glm::vec3(0.0f, 0.0f, rotation->z));
        charRenderer->setInstanceTiltBlend(t.riderInstance, glm::vec3(0.0f), 0.0f);
    }
}

void SpellVisualSystem::updateMountTransitions() {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;
    using namespace mount_transition;
    const uint32_t now = colourClockMs_;
    for (size_t i = 0; i < mountTransitions_.size();) {
        MountTransition& t = mountTransitions_[i];
        const uint32_t rider = unitInstanceResolver_ ? unitInstanceResolver_(t.unitGuid) : 0u;
        // 0x007fb7f0: gone with its unit.
        if (rider == 0) {
            if (t.modelInstance) charRenderer->removeInstance(t.modelInstance);
            mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // A precast kit's effect goes as its cast runs out.
        if (t.precast && static_cast<int32_t>(now - t.castEndMs) >= 0) {
            endMountTransition(i, true);
            continue;
        }
        if (rider != t.riderInstance) {
            releaseMountRider(t);
            t.riderInstance = rider;
            t.riderTilted = false;
        }
        // 0x006f9610: the creature's answer gives the display, and then
        // the model (0x006f83d0) - Birth playing, unseen until it touches
        // the ground.
        if (t.displayId == 0) {
            const std::optional<uint32_t> display = mountDisplayResolver_ ? mountDisplayResolver_(t.spellId) : std::nullopt;
            if (!display) {
                mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            t.displayId = *display;
        }
        if (t.displayId != 0 && t.modelInstance == 0) {
            const uint32_t modelId = mountModelLoader_ ? mountModelLoader_(t.displayId) : 0u;
            glm::vec3 at(0.0f);
            charRenderer->getInstancePosition(rider, at);
            // 0x0071fbf0 scales the mount's matrix by the unit's model
            // scale (its vtable +0x7c, 0x0071c0e0). The unit rides nothing
            // yet (+0x98c is 0, 0x007265c0 case 16), so that is the unit's
            // own size, without the mount display's +0x990.
            const float scale = mountScale(unitScaleResolver_ ? unitScaleResolver_(t.unitGuid) : 1.0f);
            t.modelInstance = modelId ? charRenderer->createInstance(modelId, at, glm::vec3(0.0f), scale) : 0u;
            if (t.modelInstance == 0) {
                mountTransitions_.erase(mountTransitions_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            charRenderer->setInstanceOpacity(t.modelInstance, 0.0f);
            charRenderer->playAnimation(t.modelInstance, kAnimBirth, false);
        }
        if (t.modelInstance == 0) {
            ++i;
            continue;
        }
        glm::mat4 riderFrame(1.0f);
        glm::vec3 riderPos(0.0f);
        charRenderer->getInstanceFrame(rider, riderFrame);
        charRenderer->getInstancePosition(rider, riderPos);
        const float riderFacing = std::atan2(riderFrame[0][1], riderFrame[0][0]);
        // 0x007fa6a0, until it takes: the rider's sequence and its events,
        // the mount's Birth's, and the mount's $STB put by the rider.
        if (!(t.state.flags & kSetUp)) {
            uint32_t riderAnim = 0;
            float riderTime = 0.0f, riderLength = 0.0f;
            const pipeline::M2Model* mountModel = charRenderer->getInstanceModelData(t.modelInstance);
            if (!mountModel || !charRenderer->getAnimationState(rider, riderAnim, riderTime, riderLength)) {
                ++i;
                continue;
            }
            SetupInput in;
            in.riderSequenceMs = static_cast<uint32_t>(riderLength);
            in.riderStbMs = charRenderer->getAnimationEventTime(rider, riderAnim, kEventStartBegin).value_or(0u);
            in.riderSteMs = charRenderer->getAnimationEventTime(rider, riderAnim, kEventStartEnd).value_or(0u);
            in.birthStbMs =
                charRenderer->getAnimationEventTime(t.modelInstance, kAnimBirth, kEventStartBegin).value_or(0u);
            in.birthSteMs =
                charRenderer->getAnimationEventTime(t.modelInstance, kAnimBirth, kEventStartEnd).value_or(0u);
            const auto* stb = mountModel->findEvent(kEventStartBegin);
            in.mountStbWorld = glm::vec3(riderFrame * glm::vec4(stb ? stb->position : glm::vec3(0.0f), 1.0f));
            in.riderTranslation = riderPos;
            in.riderFacing = riderFacing;
            if (in.riderSequenceMs == 0 || !setup(t.state, in)) {
                ++i;
                continue;
            }
        }
        // 0x006f7480: the Birth run out sets it arrived (0x007f9f60).
        uint32_t mountAnim = 0;
        float mountTime = 0.0f, mountLength = 0.0f;
        if (charRenderer->getAnimationState(t.modelInstance, mountAnim, mountTime, mountLength)) {
            if (mountAnim == kAnimBirth) {
                t.birthSeen = true;
                if (mountTime >= mountLength) t.state.flags |= kArrived;
            } else if (t.birthSeen) {
                t.state.flags |= kArrived;
            }
        }
        step(t.state, now, riderFacing, riderPos, glm::vec3(0.0f, 0.0f, 1.0f),
             [this](const glm::vec3& top, const glm::vec3& bottom) -> std::optional<GroundHit> {
                 return groundQuery_ ? groundQuery_(top, bottom) : std::nullopt;
             });
        // 0x0071fbf0: the mount where the transition has it, turned, tilted
        // and faded in.
        charRenderer->setInstancePosition(t.modelInstance, t.state.position);
        charRenderer->setInstanceRotation(t.modelInstance, mountRotation(t.state));
        charRenderer->setInstanceOpacity(t.modelInstance, t.state.fade);
        // 0x007193f0: the rider lifted toward the seat by its progress.
        glm::vec3 lift(0.0f);
        glm::mat4 seat;
        if (t.state.riderProgress != 0.0f && charRenderer->getAttachmentTransform(t.modelInstance, 0, seat))
            lift = riderLift(t.state, glm::vec3(seat[3]), t.state.position);
        charRenderer->setInstanceRenderOffset(rider, lift);
        // 0x007193f0: the rider's ground normal eased toward the one it
        // stands on - wowee keeps no movement normal, so the ground found
        // under its feet stands in for 0x00987490's. A unit standing there
        // has long since eased to it (0x007197d0 every frame), so it starts
        // there.
        if (groundQuery_) {
            const glm::vec3 up(0.0f, 0.0f, 2.0f);
            if (const auto hit = groundQuery_(riderPos + up, riderPos - up)) {
                const float seconds = t.riderNormalMs ? static_cast<float>(now - t.riderNormalMs) * 0.001f : 1e6f;
                t.riderNormal = easeGroundNormal(t.riderNormal, hit->normal, seconds);
            }
        }
        t.riderNormalMs = now;
        // While it is carried it is drawn tilted on it as 0x0082dd80 tilts
        // its model (by its M2 flags & 3); otherwise plainly (0x00743330).
        if (const auto rotation = charRenderer->getInstanceRotation(rider)) {
            if (riderTilts(t.state)) {
                const pipeline::M2Model* riderModel = charRenderer->getInstanceModelData(rider);
                charRenderer->setInstanceRotation(
                    rider, tiltedRotation(t.riderNormal, rotation->z, riderModel ? riderModel->globalFlags : 0u));
                // And blended toward the full tilt by the sequence its body
                // plays (the end of 0x0082dd80).
                float weight = 0.0f;
                glm::mat4 pose(1.0f);
                const std::vector<glm::mat4>* bones = nullptr;
                int sequence = -1;
                float timeMs = 0.0f;
                float globalMs = 0.0f;
                if (riderModel && charRenderer->getInstancePose(rider, pose, bones, sequence, timeMs, globalMs) &&
                    sequence >= 0 && static_cast<size_t>(sequence) < riderModel->sequences.size()) {
                    const auto& seq = riderModel->sequences[static_cast<size_t>(sequence)];
                    weight = fullTiltWeight(seq.flags, timeMs, seq.duration,
                                            charRenderer->getInstancePlaybackRate(rider));
                }
                charRenderer->setInstanceTiltBlend(rider, tiltedRotation(t.riderNormal, rotation->z, 3u), weight);
                t.riderTilted = true;
            } else if (t.riderTilted) {
                charRenderer->setInstanceRotation(rider, glm::vec3(0.0f, 0.0f, rotation->z));
                charRenderer->setInstanceTiltBlend(rider, glm::vec3(0.0f), 0.0f);
                t.riderTilted = false;
            }
        }
        // Arrived, its animation ending holds its last frame (0x0073bbd0).
        charRenderer->setInstanceHoldsLastFrame(rider, riderHoldsLastFrame(t.state));
        ++i;
    }
}

void SpellVisualSystem::startSwingTrails(uint32_t renderInstanceId, const swing_trail::Start& start) {
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;
    // 0x00715ba0: only with the melee weapons drawn (+0xb5c 1), on the one
    // in each hand (+0xb50, +0xb54).
    const uint64_t unitGuid = instanceUnitResolver_ ? instanceUnitResolver_(renderInstanceId) : 0;
    if (!meleeDrawnQuery_ || unitGuid == 0 || !meleeDrawnQuery_(unitGuid)) return;
    for (uint32_t attachment : {1u /* HandRight */, 2u /* HandLeft */}) {
        if (!charRenderer->heldModelEvent(renderInstanceId, attachment, swing_trail::kEventBladeTop) ||
            !charRenderer->heldModelEvent(renderInstanceId, attachment, swing_trail::kEventBladeBottom))
            continue;
        auto it = std::find_if(swings_.begin(), swings_.end(), [&](const WeaponSwing& w) {
            return w.renderInstanceId == renderInstanceId && w.attachment == attachment;
        });
        if (it == swings_.end()) {
            swings_.push_back({.renderInstanceId = renderInstanceId, .attachment = attachment});
            it = std::prev(swings_.end());
        }
        // 0x007e4ff0: started again, emptied.
        it->trail.start(start, colourClockMs_);
    }
}

void SpellVisualSystem::publishClientStrips() {
    if (!m2Renderer_) return;
    std::vector<M2Renderer::ClientStrip> strips;
    // The swing trails, drawn with their weapons (0x007e4f50), each frame
    // laying the blade's bottom and top down until they have faded.
    if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        for (auto it = swings_.begin(); it != swings_.end();) {
            const auto top = charRenderer->heldModelEvent(it->renderInstanceId, it->attachment,
                                                          swing_trail::kEventBladeTop);
            const auto bottom = charRenderer->heldModelEvent(it->renderInstanceId, it->attachment,
                                                             swing_trail::kEventBladeBottom);
            M2Renderer::ClientStrip strip;
            if (!top || !bottom || !it->trail.step(colourClockMs_, top->position, bottom->position, strip.vertices)) {
                it = swings_.erase(it);
                continue;
            }
            if (!strip.vertices.empty()) {
                strip.material = swing_trail::material();
                strips.push_back(std::move(strip));
            }
            ++it;
        }
    } else {
        swings_.clear();
    }
    const glm::vec3 camera = renderer_ && renderer_->getCamera() ? renderer_->getCamera()->getPosition() : glm::vec3(0.0f);
    // 0x009ab070: render layers 0 to 3 in turn, each the lightning last
    // made first.
    for (int32_t layer = 0; layer < 4; ++layer) {
        for (auto chainIt = chains_.rbegin(); chainIt != chains_.rend(); ++chainIt) {
            if (chainIt->effect->renderLayer != layer) continue;
            for (auto lightIt = chainIt->lightning.rbegin(); lightIt != chainIt->lightning.rend(); ++lightIt) {
                if (!*lightIt) continue;
                M2Renderer::ClientStrip strip;
                if (!(*lightIt)->build(camera, strip.vertices)) continue;
                strip.texturePath = chainIt->effect->texture;
                strip.material = spell_chain::materialFor(*chainIt->effect);
                strips.push_back(std::move(strip));
            }
        }
    }
    if (strips.empty() && !publishedStrips_) return;
    publishedStrips_ = !strips.empty();
    m2Renderer_->setClientStrips(std::move(strips));
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
        audio::AudioEngine::instance().playSound3D(sound->data, position, sound->volume, 1.0f, 100.0f, 1.0f,
                                                   audio::Channel::Effects);
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
            if (!aura.playing || aura.awaitingMissile || aura.awaitingChain) {
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
    finishEffectModelLoads();
    // First: an arrival plays its impact kit, which joins activeSpellVisuals_.
    colourClockMs_ += static_cast<uint32_t>(std::lround(deltaTime * 1000.0f));
    updateMissiles(deltaTime);
    updateAuraKits(deltaTime);
    updateUnitColours();
    updateUnitAlphas();
    updateLightTint();
    updateChains(deltaTime);
    updateMountTransitions();
    expireWaitingKits();
    expireStateKitWaits();
    updateDynamicObjects(deltaTime);
    updateFallingModels(deltaTime);
    publishClientStrips();
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

void SpellVisualSystem::addDynamicObject(uint64_t guid, uint32_t visualId, const DynamicObjectCast& cast,
                                         uint8_t type, float radius, float objectScale, const glm::vec3& position,
                                         float yaw, uint64_t transportGuid) {
    loadSpellVisualDbc();
    removeDynamicObject(guid, false);
    const auto it = persistentAreas_.find(visualId);
    if (it == persistentAreas_.end()) return;
    DynamicObjectVisual object{.guid = guid, .area = it->second, .type = type, .radius = radius,
                               .objectScale = objectScale, .position = position, .yaw = yaw, .cast = cast};
    // A passenger whose transport is not there is made in the world
    // (0x00705230 -> 0x004f4230).
    if (transportGuid != 0 && transportFrameQuery_) {
        if (const auto frame = transportFrameQuery_(transportGuid)) {
            object.transportGuid = transportGuid;
            object.fromMadeFrame = glm::inverse(*frame);
            object.moved = glm::mat4(1.0f);
        }
    }
    // 0x00705230: held while a missile of its cast flies (0x00804cc0).
    object.held = missileInFlight(cast);
    startDynamicObjectKit(object);
    dynamicObjects_.push_back(std::move(object));
}

bool SpellVisualSystem::missileInFlight(const DynamicObjectCast& cast) const {
    return missileInFlightQuery_ && missileInFlightQuery_(cast.caster, cast.spellId, cast.castTime);
}

void SpellVisualSystem::startDynamicObjectKit(DynamicObjectVisual& object) {
    if (object.held) return;
    // 0x00704f60 -> 0x007fbd70: the kit's CharProc 9, models falling over
    // the object's radius, as many a second as spellEffectLevel (default 9)
    // lets them (0x007fb5b0).
    if (!object.area.fallingModel.empty()) {
        const int level = std::atoi(addons::storedCVarValue("spellEffectLevel", "9").c_str());
        fallingModels_.push_back(FallingModels{
            .ownerGuid = object.guid, .modelPath = object.area.fallingModel, .centre = object.position,
            .radius = object.radius, .perSecond = blizzard_object::rate(object.area.fallingPerSecond, level),
            .frame = object.moved});
    }
    // 0x00704f60: the kit's sound on the object, looping, rising over two
    // seconds.
    if (const LoadedSound* sound = soundEntry(object.area.soundId)) {
        const float maxDistance = sound->cutoff > 0.0f ? sound->cutoff : 100.0f;
        const float minDistance = sound->minDistance > 0.0f ? std::min(sound->minDistance, maxDistance) : 1.0f;
        object.soundHandle = audio::AudioEngine::instance().playSound3DLooping(
            sound->data, object.position, sound->volume, maxDistance, minDistance,
            dynamic_object::kSoundFadeInSeconds, audio::Channel::Effects);
    }
}

void SpellVisualSystem::stopFallingModels(uint64_t ownerGuid) {
    // 0x007f9ee0: no more fall; those falling play out.
    for (FallingModels& falling : fallingModels_) {
        if (falling.ownerGuid != ownerGuid) continue;
        falling.ownerGuid = 0;
        falling.perSecond = 0.0f;
    }
}

void SpellVisualSystem::updateFallingModels(float deltaTime) {
    if (!m2Renderer_) return;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (auto it = fallingModels_.begin(); it != fallingModels_.end();) {
        FallingModels& falling = *it;
        // 0x007fc220: those due this frame, each put on what is below its
        // point, under any ceiling on the way up.
        const auto segment = [this](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
            return segmentQuery_ ? segmentQuery_(from, to) : std::nullopt;
        };
        const glm::mat4* frame = falling.frame ? &*falling.frame : nullptr;
        for (uint32_t n = blizzard_object::advance(falling.carried, deltaTime, falling.perSecond); n > 0; --n) {
            const float fraction = unit(fallingRng_);
            FallingModels::Fall fall{.position = blizzard_object::drop(falling.centre, frame, falling.radius, fraction,
                                                                       unit(fallingRng_), segment)};
            fall.startsIn = blizzard_object::startDelaySeconds(unit(fallingRng_));
            falling.falls.push_back(fall);
        }
        for (auto fallIt = falling.falls.begin(); fallIt != falling.falls.end();) {
            FallingModels::Fall& fall = *fallIt;
            if (fall.instanceId == 0) {
                // 0x007fb2d0: once its time has come and its model is there,
                // drawn where it fell, playing its Stand once.
                fall.startsIn -= deltaTime;
                if (fall.startsIn > 0.0f) {
                    ++fallIt;
                    continue;
                }
                const uint32_t modelId = acquireEffectModel(falling.modelPath);
                if (modelId == 0) {
                    if (const auto known = spellVisualModelIds_.find(falling.modelPath);
                        known != spellVisualModelIds_.end() && spellVisualFailedModels_.count(known->second) != 0) {
                        fallIt = falling.falls.erase(fallIt);
                        continue;
                    }
                    ++fallIt;
                    continue;
                }
                fall.instanceId = m2Renderer_->createInstance(modelId, blizzard_object::drawnAt(fall.position, frame),
                                                              glm::vec3(0.0f), 1.0f, false);
                if (fall.instanceId == 0) {
                    fallIt = falling.falls.erase(fallIt);
                    continue;
                }
                // Its events to 0x007f9d80; its end to 0x007fc200.
                m2Renderer_->setInstanceEventOwner(fall.instanceId, m2_event::Owner::FallingModel, 0);
                m2Renderer_->setInstanceAnimation(fall.instanceId, dynamic_object::kAnimStand, false);
                fall.endsIn = m2Renderer_->getInstanceAnimDuration(fall.instanceId) * 0.001f;
                ++fallIt;
                continue;
            }
            fall.endsIn -= deltaTime;
            if (fall.endsIn <= 0.0f) {
                // 0x007fc200 -> 0x007fc010: its Stand over, it goes.
                m2Renderer_->removeInstance(fall.instanceId);
                fallIt = falling.falls.erase(fallIt);
                continue;
            }
            // 0x007fb2d0: drawn each frame where the frame puts it.
            if (frame) m2Renderer_->setInstancePosition(fall.instanceId, blizzard_object::drawnAt(fall.position, frame));
            ++fallIt;
        }
        // 0x007fc220: stopped and with none left, it goes.
        if (falling.perSecond == 0.0f && falling.falls.empty()) {
            it = fallingModels_.erase(it);
            continue;
        }
        ++it;
    }
}

void SpellVisualSystem::releaseDynamicObject(DynamicObjectVisual& object) {
    stopFallingModels(object.guid);
    // 0x00704940 / 0x007054e0: the sound falls over three seconds.
    if (object.soundHandle != 0)
        audio::AudioEngine::instance().stopSoundWithFade(object.soundHandle, dynamic_object::kSoundFadeOutSeconds);
    object.soundHandle = 0;
    if (object.instanceId != 0 && m2Renderer_) m2Renderer_->removeInstance(object.instanceId);
    object.instanceId = 0;
}

void SpellVisualSystem::removeDynamicObject(uint64_t guid, bool destroyed) {
    for (auto it = dynamicObjects_.begin(); it != dynamicObjects_.end(); ++it) {
        if (it->guid != guid || it->decaying) continue;
        // 0x00704a70: a destroyed object's model plays its Decay and the
        // object goes as it ends (0x00704a40); otherwise it goes now.
        if (it->instanceId != 0 && m2Renderer_ &&
            dynamic_object::decaysOnRemoval(destroyed,
                                            m2Renderer_->hasAnimation(it->instanceId, dynamic_object::kAnimDecay))) {
            m2Renderer_->setInstanceAnimation(it->instanceId, dynamic_object::kAnimDecay, false);
            it->decaying = true;
            it->sequenceEndsIn = m2Renderer_->getInstanceAnimDuration(it->instanceId) * 0.001f;
            return;
        }
        releaseDynamicObject(*it);
        dynamicObjects_.erase(it);
        return;
    }
}

void SpellVisualSystem::updateDynamicObjects(float deltaTime) {
    if (!m2Renderer_) return;
    for (auto it = dynamicObjects_.begin(); it != dynamicObjects_.end();) {
        DynamicObjectVisual& object = *it;
        // 0x007132e0 -> 0x00704910: on a transport, its matrix each frame,
        // for the model (0x00744380) and the drops (0x007fa1b0).
        if (object.transportGuid != 0 && transportFrameQuery_) {
            if (const auto frame = transportFrameQuery_(object.transportGuid)) {
                object.moved = *frame * object.fromMadeFrame;
                if (object.instanceId != 0)
                    m2Renderer_->setInstanceTransform(object.instanceId, *object.moved * object.madeModel);
                for (FallingModels& falling : fallingModels_) {
                    if (falling.ownerGuid == object.guid) falling.frame = object.moved;
                }
            }
        }
        // 0x007051b0: once its cast's missile has arrived, the model shows -
        // from its Birth - and its kit starts (0x00704f60).
        if (object.held && !object.decaying && !missileInFlight(object.cast)) {
            object.held = false;
            startDynamicObjectKit(object);
        }
        if (object.instanceId == 0 && !object.decaying && !object.area.modelPath.empty()) {
            // A model that will not load is not asked for again.
            if (const auto known = spellVisualModelIds_.find(object.area.modelPath);
                known != spellVisualModelIds_.end() && spellVisualFailedModels_.count(known->second) != 0) {
                object.area.modelPath.clear();
            }
        }
        if (object.instanceId == 0 && !object.decaying && !object.held && !object.area.modelPath.empty()) {
            // 0x00704d90, as the model has loaded: its scale, and its Birth.
            const uint32_t modelId = acquireEffectModel(object.area.modelPath);
            if (modelId != 0) {
                const float scale =
                    dynamic_object::modelScale(object.type, object.radius, m2Renderer_->getModelVertexRadius(modelId),
                                               true, object.area.areaEffectSize, object.area.effectScale) *
                    object.objectScale;
                object.instanceId = m2Renderer_->createInstance(modelId, object.position,
                                                                glm::vec3(0.0f, 0.0f, object.yaw), scale);
                if (object.instanceId != 0 && object.moved) {
                    object.madeModel = m2Renderer_->getInstanceModelMatrix(object.instanceId).value_or(glm::mat4(1.0f));
                    m2Renderer_->setInstanceTransform(object.instanceId, *object.moved * object.madeModel);
                }
                if (object.instanceId != 0) {
                    // Its events go to the object (0x00704f20 -> 0x00704c30).
                    m2Renderer_->setInstanceEventOwner(object.instanceId, m2_event::Owner::DynamicObject, 0);
                    object.hasHold = m2Renderer_->hasAnimation(object.instanceId, dynamic_object::kAnimHold);
                    const uint32_t first = dynamic_object::firstSequence(
                        m2Renderer_->hasAnimation(object.instanceId, dynamic_object::kAnimBirth));
                    m2Renderer_->setInstanceAnimation(object.instanceId, first, first == dynamic_object::kAnimStand);
                    object.sequenceEndsIn = m2Renderer_->getInstanceAnimDuration(object.instanceId) * 0.001f;
                }
            }
        }
        if (object.instanceId != 0 && object.sequenceEndsIn >= 0.0f) {
            object.sequenceEndsIn -= deltaTime;
            if (object.sequenceEndsIn <= 0.0f) {
                object.sequenceEndsIn = -1.0f;
                if (object.decaying) {
                    // 0x00704a40: the Decay over, the object goes.
                    releaseDynamicObject(object);
                    it = dynamicObjects_.erase(it);
                    continue;
                }
                // 0x00704af0 -> 0x00704a00: its Hold, else its Stand.
                m2Renderer_->setInstanceAnimation(object.instanceId, dynamic_object::idleSequence(object.hasHold),
                                                  true);
            }
        }
        if (object.decaying && object.instanceId == 0) {
            releaseDynamicObject(object);
            it = dynamicObjects_.erase(it);
            continue;
        }
        ++it;
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    // 0x007fec00: the cast's precast kits' effects go (0x00744bd0), a mount
    // transition's handing its unit over, which is then set to the
    // server's mount.
    for (size_t i = 0; i < mountTransitions_.size();) {
        if (mountTransitions_[i].precast)
            endMountTransition(i, true);
        else
            ++i;
    }
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
    for (DynamicObjectVisual& object : dynamicObjects_) releaseDynamicObject(object);
    dynamicObjects_.clear();
    for (const FallingModels& falling : fallingModels_) {
        for (const FallingModels::Fall& fall : falling.falls) {
            if (fall.instanceId != 0 && m2Renderer_) m2Renderer_->removeInstance(fall.instanceId);
        }
    }
    fallingModels_.clear();
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
    riderPoses_.clear();
    chains_.clear();
    if (CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        for (const MountTransition& t : mountTransitions_) {
            if (t.modelInstance) charRenderer->removeInstance(t.modelInstance);
            if (t.riderInstance) charRenderer->setInstanceRenderOffset(t.riderInstance, glm::vec3(0.0f));
        }
    }
    mountTransitions_.clear();
    waitingKits_.clear();
    unitChainCounters_.clear();
    swings_.clear();
    castTargets_.clear();
    unitChannels_.clear();
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& [instance, fade] : timedAlphas_)
            renderer_->getCharacterRenderer()->setInstanceKitAlpha(instance, 1.0f, 0.0f);
        for (const AnimationHold& hold : animationHolds_) {
            const uint32_t mount = unitMountInstanceResolver_ ? unitMountInstanceResolver_(hold.unitGuid) : 0u;
            if (!hold.mountWasHeld && mount != 0)
                renderer_->getCharacterRenderer()->setInstanceAnimationFrozen(mount, false);
            if (!hold.wasHeld)
                renderer_->getCharacterRenderer()->setInstanceAnimationFrozen(hold.renderInstanceId, false);
        }
    }
    animationHolds_.clear();
    timedAlphas_.clear();
    for (const WornItem& worn : wornItems_) {
        if (wornItemSink_) wornItemSink_(worn.unitGuid, worn.equipSlot, 0, 0);
    }
    wornItems_.clear();
    publishClientStrips();
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

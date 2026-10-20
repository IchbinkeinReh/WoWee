// The sounds a unit's model events play through the unit (0x00732650): its
// CreatureSoundData's fidgets, wings and the rest (unit_model_events.hpp).
#include "core/entity_spawner.hpp"

#include "addons/lua_api_registrations.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_event_sounds.hpp"
#include "rendering/renderer.hpp"
#include "rendering/unit_model_events.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace wowee::core {

namespace {

namespace ue = rendering::unit_event;

/// CreatureDisplayInfo (3.3.5a): 1 ModelID, 2 SoundID; CreatureModelData:
/// 13 SoundID (0x0072d940 reads +0x8, then the model's +0x34).
constexpr uint32_t kDisplayModelColumn = 1;
constexpr uint32_t kDisplaySoundColumn = 2;
constexpr uint32_t kModelSoundColumn = 13;

/// xorshift32: the draws a slot's chance takes (the client's own generator
/// is 0x00464580; only the chance it gives matters).
uint32_t nextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/// GroundEffectTexture's TerrainType (+0x28), which 0x007a0530 reads.
constexpr uint32_t kGroundEffectTerrainTypeColumn = 10;

/// The footfall sounds from TerrainType and FootstepTerrainLookup
/// (0x004cf990); null when either is missing.
std::shared_ptr<const ue::FootstepTerrainSounds> loadFootstepSounds(pipeline::AssetManager& assets) {
    auto terrainTypes = assets.loadDBCOptional("TerrainType.dbc");
    auto lookup = assets.loadDBCOptional("FootstepTerrainLookup.dbc");
    if (!terrainTypes || !lookup || terrainTypes->getFieldCount() < 5 || lookup->getFieldCount() < 5) return nullptr;
    auto table = std::make_shared<ue::FootstepTerrainSounds>();
    for (uint32_t i = 0; i < terrainTypes->getRecordCount(); ++i) {
        // ID, TerrainDesc, FootstepSprayRun, FootstepSprayWalk, SoundID.
        table->addTerrainType(static_cast<int32_t>(terrainTypes->getUInt32(i, 0)), terrainTypes->getUInt32(i, 4));
    }
    for (uint32_t i = 0; i < lookup->getRecordCount(); ++i) {
        // ID, CreatureFootstepID, TerrainSoundID, SoundID, SoundIDSplash.
        table->addLookup(lookup->getUInt32(i, 1), static_cast<int32_t>(lookup->getUInt32(i, 2)),
                         lookup->getUInt32(i, 3), lookup->getUInt32(i, 4));
    }
    return table;
}

}  // namespace

void EntitySpawner::playUnitModelEvents(uint32_t localPlayerInstance) {
    rendering::CharacterRenderer* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) return;
    std::vector<rendering::CharacterRenderer::FiredEvent> fired = cr->takeFiredEvents();
    if (fired.empty() || !gameHandler_ || !assetManager_) return;

    // Whose model each event came from: a unit's own, or the mount under it.
    const uint64_t self = gameHandler_->getPlayerGuid();
    const auto ownerOf = [&](uint32_t instanceId) -> uint64_t {
        if (instanceId == 0) return 0;
        if (instanceId == localPlayerInstance || instanceId == mountInstanceId_) return self;
        for (const auto* instances : {&creatureInstances_, &playerInstances_}) {
            for (const auto& [guid, id] : *instances) {
                if (id == instanceId) return guid;
            }
        }
        for (const auto& [guid, mount] : remotePlayerMounts_) {
            if (mount.instanceId == instanceId) return guid;
        }
        return 0;
    };

    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!displays || !models || !sounds || sounds->getFieldCount() < ue::kColumnCount) return;

    // The unit's display's CreatureSoundData row (+0x974), and - for the
    // fidgets and the slots - its pet row in its place while the unit has a
    // pet number (0x0071a3f0).
    const auto soundRow = [&](const game::Unit& unit, bool petRow) -> int32_t {
        const int32_t display = displays->findRecordById(unit.getDisplayId());
        if (display < 0 || displays->getFieldCount() <= kDisplaySoundColumn) return -1;
        uint32_t soundId = displays->getUInt32(static_cast<uint32_t>(display), kDisplaySoundColumn);
        if (soundId == 0 && models->getFieldCount() > kModelSoundColumn) {
            const int32_t model =
                models->findRecordById(displays->getUInt32(static_cast<uint32_t>(display), kDisplayModelColumn));
            if (model >= 0) soundId = models->getUInt32(static_cast<uint32_t>(model), kModelSoundColumn);
        }
        int32_t row = soundId != 0 ? sounds->findRecordById(soundId) : -1;
        if (row < 0 || !petRow) return row;
        const uint16_t petNumber = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
        if (petNumber == 0xFFFF || unit.getField(petNumber) == 0) return row;
        const auto pet = static_cast<int32_t>(sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnPetRow));
        if (pet > 0) row = sounds->findRecordById(static_cast<uint32_t>(pet));
        return row;
    };

    // The unit the client moves - the player, or the vehicle it drives
    // (DAT_00ca1238): its creature sounds, as the player's own, are heard
    // at the listener (0x00747310).
    const uint64_t mover = gameHandler_->activeMover();
    const bool listenerAtCharacter = addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";

    // The ground a unit stands on (0x007c2440 from z + 0.1 down): a
    // building's floor material's, else the terrain's quad's, else none.
    const auto groundTerrainType = [&](const glm::vec3& at) -> int32_t {
        const float probeZ = at.z + 0.1f;
        std::optional<float> buildingFloor;
        int32_t buildingType = -1;
        if (auto* wmo = renderer_->getWMORenderer()) {
            buildingFloor = wmo->getFloorHeight(at.x, at.y, probeZ, nullptr,
                                                std::numeric_limits<float>::quiet_NaN(), &buildingType);
        }
        std::optional<float> terrainFloor;
        std::optional<int32_t> terrainType;
        if (auto* terrain = renderer_->getTerrainManager()) {
            terrainFloor = terrain->getHeightAt(at.x, at.y);
            if (terrainFloor && *terrainFloor > probeZ) terrainFloor.reset();
            if (terrainFloor) {
                if (const auto effect = terrain->getGroundEffectIdAt(at.x, at.y)) {
                    auto effects = assetManager_->loadDBCOptional("GroundEffectTexture.dbc");
                    const int32_t row = effects && effects->getFieldCount() > kGroundEffectTerrainTypeColumn
                                            ? effects->findRecordById(*effect)
                                            : -1;
                    if (row >= 0) {
                        terrainType = static_cast<int32_t>(
                            effects->getUInt32(static_cast<uint32_t>(row), kGroundEffectTerrainTypeColumn));
                    }
                }
            }
        }
        return ue::groundTerrainType(buildingFloor, buildingType, terrainFloor, terrainType);
    };
    // The footstep slider over the Sound Effects volume the play is heard at.
    const auto footstepVolume = [&]() -> float {
        audio::AudioCoordinator* ac = renderer_->getAudioCoordinator();
        audio::FootstepManager* steps = ac ? ac->getFootstepManager() : nullptr;
        const float effects = audio::AudioEngine::instance().getEffectsVolume();
        if (!steps || !(effects > 0.0f)) return 1.0f;
        return std::clamp(steps->getVolumeScale() / effects, 0.0f, 1.0f);
    };

    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    const auto nowMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
    for (const auto& event : fired) {
        const ue::Action action = ue::classify(event.id);
        if (action.kind == ue::Kind::None) continue;
        const uint64_t guid = ownerOf(event.instanceId);
        auto entity = guid ? gameHandler_->getEntityManager().getEntity(guid) : nullptr;
        if (!entity || !entity->isUnit()) continue;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        // Where the unit is: the instance the event came from - the mount's
        // under a rider - stands where the unit does.
        glm::vec3 unitPosition;
        if (!cr->getInstancePosition(event.instanceId, unitPosition)) continue;

        switch (action.kind) {
        case ue::Kind::Fidget: {
            const int32_t row = soundRow(unit, true);
            if (row < 0) break;
            const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnFidget0 + action.index);
            const glm::vec3 at = unitPosition + glm::vec3(0.0f, 0.0f, ue::kFidgetHeight);
            if (sound != 0) player.playOnce(sound, &at, 1.0f, assetManager_);
            break;
        }
        case ue::Kind::Slot: {
            // $FDX's Stand sound is not the active player's (0x007465e0).
            const bool activePlayer = guid == self;
            if (action.index == ue::kSlotStand && activePlayer) break;
            if (!ue::slotPlays(action.index, nextRandom(unitEventRandom_))) break;
            if (action.index == ue::kSlotStand) {
                if (!ue::standSoundDue(nowMs, lastStandSoundMs_)) break;
                lastStandSoundMs_ = nowMs;
            }
            const int32_t row = soundRow(unit, true);
            if (row < 0) break;
            const uint32_t sound =
                sounds->getUInt32(static_cast<uint32_t>(row), ue::slotColumn(action.index));
            if (sound == 0) break;
            // The active player's own, and the vehicle it drives, with the
            // listener at the character: where the listener is and fainter
            // (0x00747310).
            if ((activePlayer || guid == mover) && listenerAtCharacter) {
                player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_);
            } else {
                player.playOnce(sound, &unitPosition, 1.0f, assetManager_);
            }
            break;
        }
        case ue::Kind::AtEvent: {
            const int32_t row = soundRow(unit, false);
            if (row < 0) break;
            const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), action.index);
            if (sound != 0) player.playOnce(sound, &event.position, 1.0f, assetManager_);
            break;
        }
        case ue::Kind::Footstep: {
            // 0x00747240: not a hovering unit's; with FootstepSounds on.
            if (addons::storedCVarValue("FootstepSounds", "1") == "0") break;
            const uint16_t bytes1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
            if (bytes1 != 0xFFFF && ue::hovers(unit.getField(bytes1))) break;
            const int32_t row = soundRow(unit, true);
            if (row < 0) break;
            const uint32_t footstepId = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnFootstep);
            if (footstepId == 0) break;
            if (!footstepSounds_) footstepSounds_ = loadFootstepSounds(*assetManager_);
            if (!footstepSounds_) break;
            // The foot in water, where the unit is and not swimming
            // (0x0071a030).
            const bool activePlayer = guid == self;
            const uint32_t moveFlags =
                activePlayer ? gameHandler_->getMovementInfo().flags : unit.getReportedMoveFlags();
            std::optional<float> liquid;
            if (auto* water = renderer_->getWaterRenderer()) liquid = water->getWaterHeightAt(unitPosition.x, unitPosition.y);
            const bool inWater = ue::footInWater(liquid, event.position.z, moveFlags);
            const uint32_t sound = footstepSounds_->sound(footstepId, groundTerrainType(unitPosition), inWater);
            if (sound == 0) break;
            // 0x004cf170: the active player's own, a player's, with the
            // listener at the character - where the listener is, at 0.65;
            // every other where the foot is.
            const float volume = footstepVolume();
            if (activePlayer && entity->getType() == game::ObjectType::PLAYER && listenerAtCharacter) {
                player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume * volume, assetManager_);
            } else {
                player.playOnce(sound, &event.position, volume, assetManager_);
            }
            break;
        }
        case ue::Kind::None:
            break;
        }
    }
}

}  // namespace wowee::core

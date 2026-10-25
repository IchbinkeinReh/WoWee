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
#include "rendering/unit_splash.hpp"
#include "rendering/melee_sound.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"

#include <algorithm>
#include <bit>
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

int32_t EntitySpawner::unitSoundRow(const game::Unit& unit, bool petRow) const {
    // The unit's display's CreatureSoundData row (+0x974), and - for the
    // fidgets and the slots - its pet row in its place while the unit has a
    // pet number (0x0071a3f0).
    if (!assetManager_) return -1;
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!displays || !models || !sounds || sounds->getFieldCount() < ue::kColumnCount) return -1;
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
}

void EntitySpawner::playUnitSlot(const game::Unit& unit, uint32_t slot, bool always, const glm::vec3* at) {
    // 0x00747310, a unit's sound slot: by its chance unless asked for
    // outright; the Stand sound at most every ten seconds of all units.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    if (!always && !ue::slotPlays(slot, nextRandom(unitEventRandom_))) return;
    if (slot == ue::kSlotStand) {
        const auto nowMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now().time_since_epoch())
                                                     .count());
        if (!ue::standSoundDue(nowMs, lastStandSoundMs_)) return;
        lastStandSoundMs_ = nowMs;
    }
    const int32_t row = unitSoundRow(unit, true);
    if (row < 0) return;
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), ue::slotColumn(slot));
    if (sound == 0) return;
    // Counted by its kind (0x00746b30), more of a creature heard further.
    bool moreAudible = false;
    if (unit.getType() == game::ObjectType::UNIT) {
        const auto& info = gameHandler_->getCreatureInfoCache();
        const auto it = info.find(unit.getEntry());
        moreAudible = it != info.end() && (it->second.typeFlags & ue::kCreatureTypeFlagMoreAudible) != 0;
    }
    const audio::PlayLimit type = ue::slotPlayType(slot, moreAudible);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    // The active player's own, and the vehicle it drives, with the listener
    // at the character: where the listener is and fainter.
    const uint64_t guid = unit.getGuid();
    const bool own = guid == gameHandler_->getPlayerGuid() || guid == gameHandler_->activeMover();
    if (own && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
        player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_, type);
        return;
    }
    std::optional<glm::vec3> where;
    if (at) where = *at;
    else where = unitSoundPosition(guid);
    if (where) player.playOnce(sound, &*where, 1.0f, assetManager_, type);
}

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

    const auto soundRow = [&](const game::Unit& unit, bool petRow) { return unitSoundRow(unit, petRow); };

    const bool listenerAtCharacter = addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";

    // The ground a unit stands on (0x007c2a70 from z + 0.1 down): a
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
            // The terrain met on the way down, to weigh a building's floor
            // against: none where it is over the probe (0x007c28f0).
            terrainFloor = terrain->getHeightAt(at.x, at.y);
            if (terrainFloor && *terrainFloor > probeZ) terrainFloor.reset();
            // The quad's type, read by where the unit is and not how high
            // (0x007a0530 takes x and y alone): under the terrain's surface
            // with no building's floor beneath, still the quad's.
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
    for (const auto& event : fired) {
        // A melee swing's whoosh and landing (0x00756240).
        if (event.id == rendering::melee_sound::kEventSwing || rendering::melee_sound::isLandEvent(event.id)) {
            if (const uint64_t attacker = ownerOf(event.instanceId)) meleeModelEvent(attacker, event.id, event.position);
            continue;
        }
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
            if (action.index == ue::kSlotStand && guid == self) break;
            playUnitSlot(unit, action.index, false, &unitPosition);
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
            // 0x00747240: not a hovering unit's, one using a vehicle or a
            // player's ghost; with FootstepSounds on.
            if (addons::storedCVarValue("FootstepSounds", "1") == "0") break;
            const bool activePlayer = guid == self;
            const uint32_t moveFlags =
                activePlayer ? gameHandler_->getMovementInfo().flags : unit.getReportedMoveFlags();
            const uint16_t bytes1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
            const uint16_t playerFlags = game::fieldIndex(game::UF::PLAYER_FLAGS);
            const bool isPlayer = entity->getType() == game::ObjectType::PLAYER;
            if (ue::footfallSilent(bytes1 != 0xFFFF ? unit.getField(bytes1) : 0u, moveFlags,
                                   gameHandler_->vehicleRoster().seatState(guid), isPlayer,
                                   isPlayer && playerFlags != 0xFFFF ? unit.getField(playerFlags) : 0u)) {
                break;
            }
            const int32_t row = soundRow(unit, true);
            if (row < 0) break;
            const uint32_t footstepId = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnFootstep);
            if (footstepId == 0) break;
            if (!footstepSounds_) footstepSounds_ = loadFootstepSounds(*assetManager_);
            if (!footstepSounds_) break;
            // The foot in water, where the unit is and not swimming
            // (0x0071a030).
            std::optional<float> liquid;
            if (auto* water = renderer_->getWaterRenderer()) liquid = water->getWaterHeightAt(unitPosition.x, unitPosition.y);
            const bool inWater = ue::footInWater(liquid, event.position.z, moveFlags);
            const uint32_t sound = footstepSounds_->sound(footstepId, groundTerrainType(unitPosition), inWater);
            if (sound == 0) break;
            // 0x004cf170: the active player's own, a player's, with the
            // listener at the character - where the listener is, at 0.65;
            // every other where the foot is. The player's own as type 17,
            // four at once; every other unit's as type 13, six at once
            // (0x009f2268) - past them a footfall is not heard.
            const float volume = footstepVolume();
            const bool own = activePlayer && isPlayer;
            const auto limit = own ? audio::PlayLimit::OwnFootstep : audio::PlayLimit::UnitFootstep;
            if (own && listenerAtCharacter) {
                player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume * volume, assetManager_, limit);
            } else {
                player.playOnce(sound, &event.position, volume, assetManager_, limit);
            }
            break;
        }
        case ue::Kind::None:
            break;
        }
    }
}


void EntitySpawner::playUnitSplashes(uint32_t localPlayerInstance) {
    namespace us = rendering::unit_splash;
    rendering::CharacterRenderer* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_ || !assetManager_) return;
    auto races = assetManager_->loadDBCOptional("ChrRaces.dbc");
    if (!races || races->getFieldCount() <= us::kChrRacesSplashSoundColumn) return;
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    rendering::WaterRenderer* water = renderer_->getWaterRenderer();
    const auto& entities = gameHandler_->getEntityManager();
    const auto& creatureInfo = gameHandler_->getCreatureInfoCache();
    const uint64_t self = gameHandler_->getPlayerGuid();
    const uint64_t mover = gameHandler_->activeMover();
    const bool listenerAtCharacter = addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";
    const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
    const uint16_t scaleField = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();

    // The display's CreatureModelData CollisionHeight at its scale.
    const auto modelHeight = [&](const game::Unit& unit) -> float {
        const int32_t display = displays ? displays->findRecordById(unit.getDisplayId()) : -1;
        if (display < 0 || !models || models->getFieldCount() <= us::kModelCollisionHeightColumn)
            return us::kDefaultCollisionHeight;
        const auto drow = static_cast<uint32_t>(display);
        const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
        if (model < 0) return us::kDefaultCollisionHeight;
        const auto mrow = static_cast<uint32_t>(model);
        const float objectScale =
            scaleField != 0xFFFF ? std::bit_cast<float>(unit.getField(scaleField)) : 1.0f;
        return us::collisionHeight(models->getFloat(mrow, us::kModelCollisionHeightColumn), objectScale,
                                   displays->getFloat(drow, us::kDisplayScaleColumn),
                                   models->getFloat(mrow, us::kModelScaleColumn));
    };

    std::unordered_map<uint64_t, float> kept;
    const auto check = [&](uint64_t guid, uint32_t instanceId) {
        auto entity = entities.getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        // Only a unit with a race has a splash to play (+0x8f0).
        const uint32_t race = bytes0 != 0xFFFF ? (unit.getField(bytes0) & 0xFFu) : 0u;
        const int32_t raceRow = race != 0 ? races->findRecordById(race) : -1;
        const uint32_t sound =
            raceRow >= 0 ? races->getUInt32(static_cast<uint32_t>(raceRow), us::kChrRacesSplashSoundColumn) : 0u;
        if (sound == 0) return;
        // Where the unit stands: the renderer's own character, else the
        // instance it is drawn by - its mount's while it rides.
        glm::vec3 at;
        if (guid == self) {
            at = renderer_->getCharacterPosition();
        } else {
            uint32_t drawn = instanceId;
            if (const RemotePlayerMount* mount = getRemotePlayerMount(guid); mount && mount->instanceId != 0)
                drawn = mount->instanceId;
            if (!cr->getInstancePosition(drawn, at)) return;
        }
        float depth = 0.0f;
        if (water) {
            if (const auto level = water->getWaterHeightAt(at.x, at.y)) depth = *level - at.z;
        }
        const auto last = splashDepth_.find(guid);
        const float lastDepth = last != splashDepth_.end() ? last->second : 0.0f;
        const uint32_t moveFlags = guid == self ? gameHandler_->getMovementInfo().flags : unit.getReportedMoveFlags();
        if ((moveFlags & ue::kMoveFlagSwimming) != 0) {
            // Kept as it was while swimming.
            kept[guid] = lastDepth;
            return;
        }
        kept[guid] = depth;
        const bool isPlayer = entity->getType() == game::ObjectType::PLAYER;
        bool byModel = isPlayer;
        if (!isPlayer) {
            if (auto it = creatureInfo.find(unit.getEntry()); it != creatureInfo.end())
                byModel = (it->second.typeFlags & us::kTypeFlagModelCollision) != 0;
        }
        const float height = byModel ? modelHeight(unit) : us::kDefaultCollisionHeight;
        if (!us::crossed(height, depth, lastDepth)) return;
        if (guid == mover && listenerAtCharacter) {
            player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_,
                            audio::PlayLimit::RaceSplash);
        } else {
            player.playOnce(sound, &at, 1.0f, assetManager_, audio::PlayLimit::RaceSplash);
        }
    };

    check(self, localPlayerInstance);
    for (const auto& [guid, instanceId] : playerInstances_) check(guid, instanceId);
    for (const auto& [guid, instanceId] : creatureInstances_) check(guid, instanceId);
    splashDepth_ = std::move(kept);
}

}  // namespace wowee::core

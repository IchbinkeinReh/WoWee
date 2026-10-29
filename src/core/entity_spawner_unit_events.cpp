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

/// The gate every pet's own line goes through (0x007474b0's first test): a
/// line is said while Sound_EnablePetSounds is on, or by a unit that is nobody's
/// pet - the client asks its sound component's owner link (+0x114) whether it is
/// empty, and who summoned the unit answers the same question of the fields.
bool petVoiceAllowed(const game::Unit& unit) {
    return addons::storedCVarValue("Sound_EnablePetSounds", "1") != "0" ||
           game::unitSummonedByGuid(unit) == 0;
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
    // The active player's own, and the vehicle it drives: kept over a
    // crowd's (0x6e) and, with the listener at the character, where the
    // listener is and fainter.
    const uint64_t guid = unit.getGuid();
    const bool own = guid == gameHandler_->getPlayerGuid() || guid == gameHandler_->activeMover();
    const int priority = own ? audio::voice_priority::kActiveMover : audio::voice_priority::kDefault;
    if (own && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
        player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_, type, false, priority);
        return;
    }
    std::optional<glm::vec3> where;
    if (at) where = *at;
    else where = unitSoundPosition(guid);
    if (where) player.playOnce(sound, &*where, 1.0f, assetManager_, type, false, priority);
}

void EntitySpawner::playUnitJumpSound(const game::Unit& unit, uint32_t slot) {
    // A jump's slots are asked outright, not by their chance: the client's
    // four callers all pass its "no roll" parameter (0x0073ed10 cases 0xbb
    // and 0x346, FUN_0073d2b0), and where the unit stands is where it jumps.
    playUnitSlot(unit, slot, true, nullptr);
}

void EntitySpawner::playUnitReactionSound(const game::Unit& unit, uint32_t reaction) {
    // 0x00716b10, SMSG_AI_REACTION's handler: it asks one thing per reaction and
    // nothing for the others. A reaction of HATE asks the unit its slot 8,
    // rolled like any other though its chance is 100 of 100; an UNFRIENDLY one
    // asks its Aggro voice line instead - a pet's kind, so the pet gate stands
    // between it and the ear. Neither ask is given a position: the line is said
    // where the unit stands, and the packet carries none.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    if (reaction == 0) {
        playUnitSlot(unit, ue::kSlotAlert, false, nullptr);
        return;
    }
    if (reaction == 2 && petVoiceAllowed(unit))
        playUnitVoiceKind(unit, ue::kVoiceKindAggro);
}

void EntitySpawner::playUnitVoiceLine(const game::Unit& unit, uint32_t sound, uint32_t* outHandle) {
    // How every line of 0x007474b0 reaches the ear: heard as that unit's own
    // voice, the active player's own - and the mover it drives - kept over a
    // crowd's (0x6e) and, with the listener at the character, at the listener
    // and fainter. None of these lines is counted by a kind, so nothing caps
    // how many of them are going at once.
    const uint64_t guid = unit.getGuid();
    const bool own = guid == gameHandler_->getPlayerGuid() || guid == gameHandler_->activeMover();
    const int priority = own ? audio::voice_priority::kActiveMover : audio::voice_priority::kDefault;
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    if (own && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
        player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_,
                        audio::PlayLimit::None, false, priority, outHandle);
        return;
    }
    if (const auto where = unitSoundPosition(guid))
        player.playOnce(sound, &*where, 1.0f, assetManager_, audio::PlayLimit::None, false, priority, outHandle);
}

void EntitySpawner::playUnitVoiceKind(const game::Unit& unit, uint32_t kind) {
    // The row the unit's display gives it (0x0072d940's +0x974), asked plain:
    // the pet row that stands in for a sound slot does not stand in for a
    // voice line, and no chance is rolled for one either.
    const uint32_t column = ue::voiceColumn(kind);
    if (column == ue::kVoiceColumnNone) return;
    const int32_t row = unitSoundRow(unit, false);
    if (row < 0) return;
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!sounds || sounds->getFieldCount() <= column) return;
    const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), column);
    if (sound == 0) return;
    // A unit says one line at a time (0x007474b0): the play it is saying is
    // held in its tracker slot, and while that runs a kind only gets through
    // when it outranks the kind already playing. Death outranks a Kill, an
    // Order outranks an Aggro, and an Aggro outranks nothing at all.
    const uint64_t guid = unit.getGuid();
    if (auto playing = unitVoiceLines_.find(guid); playing != unitVoiceLines_.end()) {
        if (!audio::AudioEngine::instance().isSoundPlaying(playing->second.first))
            unitVoiceLines_.erase(playing);
        else if (static_cast<int32_t>(kind) <= playing->second.second)
            return;
    }
    uint32_t handle = 0;
    playUnitVoiceLine(unit, sound, &handle);
    if (handle == 0) return;  // nothing started: no line to be in the way
    unitVoiceLines_[guid] = {handle, static_cast<int32_t>(kind)};
}

void EntitySpawner::playUnitPetActionSound(const game::Unit& unit, uint32_t action) {
    // 0x00716b90, SMSG_PET_ACTION_SOUND: a value of 0 asks the pet to say what
    // it waits on, 1 says it has been set on something. Nothing else the value
    // can hold says anything. Both are a pet's lines, so the client asks them
    // while Sound_EnablePetSounds is on - or of a unit that is nobody's pet,
    // which the guid in the packet can still name.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    if (action > 1) return;
    if (!petVoiceAllowed(unit)) return;
    playUnitVoiceKind(unit, action == 0 ? ue::kVoiceKindOrder : ue::kVoiceKindKill);
}

void EntitySpawner::playPetDismissSound(uint32_t modelId, const glm::vec3& at) {
    // 0x00716c00, SMSG_PET_DISMISS_SOUND: the packet names no unit but a model,
    // and the line asked for is that model's own - the sound row of the model's
    // CreatureModelData row (its +0x34, the fallback the display's own SoundID
    // leaves to it), dismiss column. So a pet whose sound row only its display
    // names says nothing here, and no unit need be known to hear it. None of the
    // voice kinds' gates are on this ask: no pet gate, no chance, no cooldown,
    // and the play is handed no tracker slot - so it neither waits on a line the
    // pet is still saying nor keeps one out of its way.
    if (!renderer_ || !assetManager_) return;
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!models || !sounds || sounds->getFieldCount() <= ue::kColumnPetDismiss) return;
    const int32_t model = models->findRecordById(modelId);
    if (model < 0 || models->getFieldCount() <= kModelSoundColumn) return;
    const uint32_t soundId = models->getUInt32(static_cast<uint32_t>(model), kModelSoundColumn);
    if (soundId == 0) return;
    const int32_t row = sounds->findRecordById(soundId);
    if (row < 0) return;
    const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnPetDismiss);
    if (sound == 0) return;
    // The position the packet gives, raised by 1.0 (0x009e1130) off the ground
    // it was read at.
    const glm::vec3 where{at.x, at.y, at.z + 1.0f};
    renderer_->getM2EventSounds().playOnce(sound, &where, 1.0f, assetManager_);
}

void EntitySpawner::playUnitDeathSound(uint64_t guid) {
    // 0x007474b0 kind 4, asked as the unit's dead state goes up (0x0073c5d0)
    // and by the combat death handler (0x007561e0): the Death column of the
    // CreatureSoundData row its display gives it. Not a slot - so no chance is
    // rolled, and not the pet row either, which only the slot asks substitute
    // (0x0071a3f0); and of the four voice kinds this is the one a pet is asked
    // for regardless of Sound_EnablePetSounds. The client refuses one display
    // outright - the record it keeps for a player only (0x0072d940 gives a
    // +0x964 to type 9 alone) when that record's own flags have 0x1000000 -
    // and a creature never has such a record to refuse.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    if (deadCreatureGuids_.count(guid)) return;  // the same death, noticed twice
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return;
    playUnitVoiceKind(static_cast<const game::Unit&>(*entity), ue::kVoiceKindDeath);
}

const std::vector<ue::EmoteSoundRow>& EntitySpawner::emoteSoundRows() {
    if (emoteSoundRowsLoaded_ || !assetManager_) return emoteSoundRows_;
    emoteSoundRowsLoaded_ = true;
    auto table = assetManager_->loadDBCOptional("EmotesTextSound.dbc");
    if (!table || table->getRecordCount() == 0) {
        LOG_WARNING("Emotes: EmotesTextSound.dbc not available - an emote says nothing");
        return emoteSoundRows_;
    }
    // ID, EmotesText id, race, sex, SoundEntries id: what the client insists
    // the table is when it loads it (0x0063e4f0 refuses five columns or a
    // twenty-byte row differently, and reads no layout for either).
    if (table->getFieldCount() < ue::kEmoteSoundColumnCount) {
        LOG_WARNING("EmotesTextSound.dbc has ", table->getFieldCount(), " fields, expected ",
                    ue::kEmoteSoundColumnCount);
        return emoteSoundRows_;
    }
    const uint32_t records = table->getRecordCount();
    emoteSoundRows_.reserve(records);
    for (uint32_t i = 0; i < records; ++i) {
        emoteSoundRows_.push_back({
            .emotesTextId = table->getUInt32(i, ue::kEmoteSoundColumnEmotesText),
            .race = table->getUInt32(i, ue::kEmoteSoundColumnRace),
            .sex = table->getUInt32(i, ue::kEmoteSoundColumnSex),
            .soundId = table->getUInt32(i, ue::kEmoteSoundColumnSound),
        });
    }
    LOG_INFO("EmotesTextSound: ", emoteSoundRows_.size(), " voice lines read");
    return emoteSoundRows_;
}

void EntitySpawner::playUnitEmoteSound(const game::Unit& unit, uint32_t emotesTextId) {
    // An emote's voice line, asked of the unit the emote shows on. Both gates
    // are the client's, and they sit inside the play itself - 0x00746d60's
    // branch for an emote-flagged ask: nothing at all while
    // Sound_EnableEmoteSounds is off, and while Sound_EnablePetSounds is off a
    // pet stays quiet, where its own unit still emotes. The client's pet test
    // is its sound component's owner link (+0x114); who summoned the unit is
    // the same question asked of the fields.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    if (addons::storedCVarValue("Sound_EnableEmoteSounds", "1") == "0") return;
    const uint64_t guid = unit.getGuid();
    if (!petVoiceAllowed(unit)) return;
    // The line is the speaker's race's and sex's, which only a player has -
    // and only once its name has been answered, so a first emote from someone
    // out of sight stays unheard rather than guessed at.
    const uint32_t sound = ue::emoteSoundId(emoteSoundRows(), emotesTextId,
                                            gameHandler_->lookupPlayerRace(guid),
                                            gameHandler_->lookupPlayerGender(guid));
    if (sound == 0) return;
    // Heard by the same rule as every other line a unit says.
    playUnitVoiceLine(unit, sound);
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

// The sounds a unit's model events play through the unit (0x00732650): its
// CreatureSoundData's fidgets, wings and the rest (unit_model_events.hpp).
#include "core/entity_spawner.hpp"
#include "game/mount_special.hpp"

#include "addons/lua_api_registrations.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/camera_pivot.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/collision_fit.hpp"
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
#include <cmath>
#include <optional>
#include <chrono>
#include <limits>
#include <unordered_set>

namespace wowee::core {

namespace {

namespace ue = rendering::unit_event;

/// CreatureDisplayInfo (3.3.5a): 1 ModelID, 2 SoundID; CreatureModelData:
/// 13 SoundID (0x0072d940 reads +0x8, then the model's +0x34).
constexpr uint32_t kDisplayModelColumn = 1;
constexpr uint32_t kDisplaySoundColumn = 2;
constexpr uint32_t kModelSoundColumn = 13;
/// CreatureModelData's FoleyMaterialID (+0x28) and Material's FoleySoundID
/// (+0x8), which 0x007464d0 plays.
constexpr uint32_t kModelFoleyMaterialColumn = 10;
constexpr uint32_t kMaterialFoleySoundColumn = 2;

/// The SoundEntries id of a unit's armour foley: its display's model's
/// FoleyMaterialID, then that Material's FoleySoundID (0x007464d0 ->
/// 0x004cfc10); 0 for none.
uint32_t foleySoundFor(pipeline::AssetManager& assets, uint32_t displayId) {
    auto displays = assets.loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assets.loadDBCOptional("CreatureModelData.dbc");
    auto materials = assets.loadDBCOptional("Material.dbc");
    if (!displays || !models || !materials || models->getFieldCount() <= kModelFoleyMaterialColumn ||
        materials->getFieldCount() <= kMaterialFoleySoundColumn)
        return 0;
    const int32_t display = displays->findRecordById(displayId);
    if (display < 0) return 0;
    const int32_t model =
        models->findRecordById(displays->getUInt32(static_cast<uint32_t>(display), kDisplayModelColumn));
    if (model < 0) return 0;
    const int32_t material =
        materials->findRecordById(models->getUInt32(static_cast<uint32_t>(model), kModelFoleyMaterialColumn));
    return material < 0 ? 0 : materials->getUInt32(static_cast<uint32_t>(material), kMaterialFoleySoundColumn);
}

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
    // fidgets, the slots, the footfalls and the loop - its mount's or its pet
    // row in its place (0x0071a3f0).
    if (!assetManager_) return -1;
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!displays || !models || !sounds || sounds->getFieldCount() < ue::kColumnCount) return -1;
    // A display's: the row it names, else its model's (0x007195d0).
    const auto rowOfDisplay = [&](uint32_t displayId) -> int32_t {
        const int32_t display = displays->findRecordById(displayId);
        if (display < 0 || displays->getFieldCount() <= kDisplaySoundColumn) return -1;
        const uint32_t named = displays->getUInt32(static_cast<uint32_t>(display), kDisplaySoundColumn);
        if (const int32_t row = named != 0 ? sounds->findRecordById(named) : -1; row >= 0) return row;
        if (models->getFieldCount() <= kModelSoundColumn) return -1;
        const int32_t model =
            models->findRecordById(displays->getUInt32(static_cast<uint32_t>(display), kDisplayModelColumn));
        const uint32_t modelsRow = model >= 0 ? models->getUInt32(static_cast<uint32_t>(model), kModelSoundColumn) : 0u;
        return modelsRow != 0 ? sounds->findRecordById(modelsRow) : -1;
    };
    const int32_t own = rowOfDisplay(unit.getDisplayId());
    if (!petRow) return own;
    const int32_t mount = unit.getMountDisplayId() != 0 ? rowOfDisplay(unit.getMountDisplayId()) : -1;
    const uint16_t petField = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
    const uint32_t petNumber = petField != 0xFFFF ? unit.getField(petField) : 0u;
    const auto pet =
        own >= 0 ? static_cast<int32_t>(sounds->getUInt32(static_cast<uint32_t>(own), ue::kColumnPetRow)) : 0;
    switch (ue::soundRowOf(mount >= 0, petNumber, pet)) {
    case ue::SoundRow::Mount: return mount;
    case ue::SoundRow::Pet: return sounds->findRecordById(static_cast<uint32_t>(pet));
    case ue::SoundRow::Own: break;
    }
    return own;
}

void EntitySpawner::playUnitSlot(const game::Unit& unit, uint32_t slot, bool always, const glm::vec3* at) {
    // 0x00747310, a unit's sound slot: by its chance unless asked for
    // outright; the Stand sound at most every ten seconds of all units.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    // A player's by 0x007631a0: the same row, its own chances and place.
    const bool player = unit.getType() == game::ObjectType::PLAYER;
    if (!always && !ue::slotPlays(player ? ue::playerSlotChance(slot) : ue::slotChance(slot),
                                  nextRandom(unitEventRandom_)))
        return;
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
    rendering::M2EventSounds& sound3d = renderer_->getM2EventSounds();
    // The active player's own, and the vehicle it drives: kept over a
    // crowd's (0x6e) and, with the listener at the character, where the
    // listener is and fainter.
    const uint64_t guid = unit.getGuid();
    const bool self = guid == gameHandler_->getPlayerGuid();
    const bool own = self || (!player && guid == gameHandler_->activeMover());
    const audio::PlayLimit type =
        player ? ue::playerSlotPlayType(slot, self) : ue::slotPlayType(slot, moreAudible);
    const int priority = own ? audio::voice_priority::kActiveMover : audio::voice_priority::kDefault;
    if (own && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
        sound3d.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_, type, false, priority);
        return;
    }
    std::optional<glm::vec3> where;
    if (at && !player) where = *at;
    else where = unitSoundPosition(guid);
    if (where && player) where->z += ue::kPlayerSlotHeight;
    if (where) sound3d.playOnce(sound, &*where, 1.0f, assetManager_, type, false, priority);
}

void EntitySpawner::updateUnitLoopSounds() {
    // 0x007467f0 for each unit: alive and not feigning death, with a loop in
    // its sound row, the loop plays on it - unless it plays already, or the
    // same sound plays within sqrt(6) yards (0x004cfe00); otherwise it fades
    // out over half a second (0x004c6390). A unit gone takes its loop with
    // it the same way (0x008799e0).
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    auto& audio = audio::AudioEngine::instance();
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    const bool table = sounds && sounds->getFieldCount() > ue::kColumnLoop;
    const uint16_t flags2Field = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
    std::vector<uint64_t> units;
    units.reserve(creatureInstances_.size() + playerInstances_.size() + 1);
    for (const auto& [guid, instance] : creatureInstances_) units.push_back(guid);
    for (const auto& [guid, instance] : playerInstances_) units.push_back(guid);
    const uint64_t self = gameHandler_->getPlayerGuid();
    if (self != 0 && !playerInstances_.count(self)) units.push_back(self);
    std::unordered_set<uint64_t> present;
    for (const uint64_t guid : units) {
        present.insert(guid);
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        const auto* unit = entity && entity->isUnit() ? static_cast<const game::Unit*>(entity.get()) : nullptr;
        uint32_t loop = 0;
        if (unit && table) {
            if (const int32_t row = unitSoundRow(*unit, true); row >= 0)
                loop = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnLoop);
        }
        const uint32_t flags2 = unit && flags2Field != 0xFFFF ? unit->getField(flags2Field) : 0u;
        const bool wanted = unit && ue::loopSoundWanted(unit->getHealth(), flags2, loop);
        auto it = unitLoopSounds_.find(guid);
        if (!wanted) {
            if (it != unitLoopSounds_.end()) {
                audio.stopSoundWithFade(it->second.handle, ue::kLoopFadeOutSeconds);
                unitLoopSounds_.erase(it);
            }
            continue;
        }
        const auto at = unitSoundPosition(guid);
        if (!at) continue;
        if (it != unitLoopSounds_.end() && audio.isSoundPlaying(it->second.handle)) {
            audio.setSoundPosition(it->second.handle, *at);
            it->second.at = *at;
            continue;
        }
        bool sameNear = false;
        for (const auto& [other, playing] : unitLoopSounds_) {
            if (other == guid || playing.sound != loop) continue;
            const glm::vec3 d = playing.at - *at;
            if (glm::dot(d, d) < ue::kSameSoundNearSq) { sameNear = true; break; }
        }
        if (sameNear) continue;
        const uint32_t handle = renderer_->getM2EventSounds().playLoop(loop, *at, 0.0f, assetManager_);
        if (handle != 0) unitLoopSounds_[guid] = UnitLoopSound{handle, loop, *at};
    }
    for (auto it = unitLoopSounds_.begin(); it != unitLoopSounds_.end();) {
        if (present.count(it->first)) { ++it; continue; }
        audio.stopSoundWithFade(it->second.handle, rendering::m2_event::kOwnerGoneFadeSeconds);
        it = unitLoopSounds_.erase(it);
    }
}

void EntitySpawner::playUnitJumpSound(const game::Unit& unit, uint32_t slot) {
    // A jump's slots are asked outright, not by their chance: the client's
    // four callers all pass its "no roll" parameter (0x0073ed10 cases 0xbb
    // and 0x346, FUN_0073d2b0), and where the unit stands is where it jumps.
    playUnitSlot(unit, slot, true, nullptr);
}

void EntitySpawner::playFlightCeilingSound(const game::Unit& unit) {
    // 0x0073a890 reads the row itself (0x0071a3f0, pet row and all) and plays
    // its Exertion column straight (0x004c6a40), not through the slots: no
    // chance and no play count. With the listener at the character it is
    // heard where the listener is, at 0.65 and kept over a crowd (0x6e);
    // otherwise where the unit is, at full volume.
    if (!renderer_ || !gameHandler_ || !assetManager_) return;
    const int32_t row = unitSoundRow(unit, true);
    if (row < 0) return;
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), ue::slotColumn(ue::kSlotExertion));
    if (sound == 0) return;
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    if (addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
        player.playOnce(sound, nullptr, ue::kListenerAtCharacterVolume, assetManager_, audio::PlayLimit::None, false,
                        audio::voice_priority::kActiveMover);
        return;
    }
    if (const auto where = unitSoundPosition(unit.getGuid()))
        player.playOnce(sound, &*where, 1.0f, assetManager_, audio::PlayLimit::None, false,
                        audio::voice_priority::kDefault);
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
    const auto armourFoleySound = [&](const game::Unit& unit) { return foleySoundFor(*assetManager_, unit.getDisplayId()); };

    const bool listenerAtCharacter = addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";

    // The ground a unit stands on (0x007c2a70 from z + 0.1 down): a
    // building's floor material's, else the terrain's quad's, else none.
    //
    // Nothing found is an answer here, not a fault: a unit whose position is
    // a little under a building's floor - which the client draws where it is
    // - has no floor below a tenth of a yard over its feet, in the client as
    // here. Not reported as a floor missing, then;
    // it was most of what the report said in a city (27 of 32 places in
    // Orgrimmar).
    const auto groundTerrainType = [&](const glm::vec3& at) -> int32_t {
        const float probeZ = at.z + 0.1f;
        std::optional<float> buildingFloor;
        int32_t buildingType = -1;
        if (auto* wmo = renderer_->getWMORenderer()) {
            buildingFloor = wmo->getFloorHeight(at.x, at.y, probeZ, nullptr,
                                                std::numeric_limits<float>::quiet_NaN(), &buildingType,
                                                /*reportMiss=*/false);
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
        // A melee swing's whoosh, its victim's reaction and its landing (0x00756240).
        if (rendering::melee_sound::isMeleeEvent(event.id)) {
            if (const uint64_t attacker = ownerOf(event.instanceId)) meleeModelEvent(attacker, event.id, event.position);
            continue;
        }
        // A cast's hand or target point, or a bow's release: the unit's
        // queued missiles are let go (0x00732650 -> 0x00715a50).
        if (ue::releasesQueuedMissiles(event.id)) {
            if (const uint64_t unit = ownerOf(event.instanceId))
                if (game::SpellHandler* spells = gameHandler_->getSpellHandler()) spells->modelReleaseEvent(unit);
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
            // player's ghost; the armour's rustle first (CGUnit_C vtable
            // +0x11c), then the step itself with FootstepSounds on.
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
            // 0x007464d0: the model's foley material's sound, two yards above
            // the unit - the player's own while
            // Sound_EnableArmorFoleySoundForSelf is on, everyone else's while
            // ...ForOthers is (0x007633f0).
            if (addons::storedCVarValue(activePlayer ? "Sound_EnableArmorFoleySoundForSelf"
                                                     : "Sound_EnableArmorFoleySoundForOthers", "1") != "0") {
                if (const uint32_t foley = armourFoleySound(unit)) {
                    const glm::vec3 at = unitPosition + glm::vec3(0.0f, 0.0f, 2.0f);
                    player.playOnce(foley, &at, 1.0f, assetManager_);
                }
            }
            if (addons::storedCVarValue("FootstepSounds", "1") == "0") break;
            const int32_t row = soundRow(unit, true);
            if (row < 0) break;
            const uint32_t footstepId = sounds->getUInt32(static_cast<uint32_t>(row), ue::kColumnFootstep);
            if (footstepId == 0) break;
            if (!footstepSounds_) footstepSounds_ = loadFootstepSounds(*assetManager_);
            if (!footstepSounds_) break;
            // The foot in water, where the unit is and not swimming
            // (0x0071a030).
            std::optional<float> liquid;
            if (const auto in = rendering::liquid_query::atUnit(renderer_->liquidWorld(), unitPosition, 0.01f))
                liquid = in->level;
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
        case ue::Kind::DeathThud: {
            // The unit's size class: its display's, else its model's.
            const int32_t drow = displays->findRecordById(unit.getDisplayId());
            if (drow < 0) break;
            int32_t size = displays->getFieldCount() > ue::kDisplaySizeClassColumn
                               ? displays->getInt32(static_cast<uint32_t>(drow), ue::kDisplaySizeClassColumn)
                               : -1;
            if (size == -1) {
                const int32_t mrow = models->findRecordById(displays->getUInt32(static_cast<uint32_t>(drow), 1));
                if (mrow >= 0 && models->getFieldCount() > ue::kModelSizeClassColumn)
                    size = models->getInt32(static_cast<uint32_t>(mrow), ue::kModelSizeClassColumn);
            }
            if (!deathThudSounds_) {
                deathThudSounds_ = std::make_shared<ue::DeathThudSounds>();
                uint32_t maxId = 0;
                if (auto ts = assetManager_->loadDBCOptional("TerrainTypeSounds.dbc"))
                    for (uint32_t i = 0; i < ts->getRecordCount(); ++i) maxId = std::max(maxId, ts->getUInt32(i, 0));
                deathThudSounds_->setTerrainSoundCount(maxId + 1);
                // ID, SizeClass, TerrainTypeSoundID, SoundEntryID, SoundEntryIDWater.
                if (auto dbc = assetManager_->loadDBCOptional("DeathThudLookups.dbc");
                    dbc && dbc->getFieldCount() >= 5) {
                    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                        deathThudSounds_->add(dbc->getUInt32(i, 1), dbc->getUInt32(i, 2), dbc->getUInt32(i, 3),
                                              dbc->getUInt32(i, 4));
                }
            }
            // The ground's TerrainType and its sound column.
            uint32_t terrainSound = 0;
            const int32_t terrainType = groundTerrainType(unitPosition);
            if (auto types = assetManager_->loadDBCOptional("TerrainType.dbc");
                types && terrainType >= 0 && types->getFieldCount() > ue::kTerrainTypeSoundColumn) {
                const int32_t trow = types->findRecordById(static_cast<uint32_t>(terrainType));
                if (trow >= 0) terrainSound = types->getUInt32(static_cast<uint32_t>(trow), ue::kTerrainTypeSoundColumn);
            }
            // In liquid: the water sound, and none at all deeper than two yards.
            std::optional<float> liquid;
            if (const auto in = rendering::liquid_query::atUnit(renderer_->liquidWorld(), unitPosition))
                liquid = in->level;
            const bool inWater = liquid && *liquid > unitPosition.z;
            if (inWater && *liquid - unitPosition.z > ue::kDeathThudMaxDepth) break;
            const uint32_t sound = deathThudSounds_->sound(size, terrainSound, inWater);
            if (sound != 0) player.playOnce(sound, &unitPosition, 1.0f, assetManager_);
            break;
        }
        case ue::Kind::None:
            break;
        }
    }
}


float EntitySpawner::playerCollisionHeight() {
    namespace us = rendering::unit_splash;
    if (!gameHandler_ || !assetManager_) return collisionHeight_;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity || !entity->isUnit()) return collisionHeight_;
    const auto& unit = static_cast<const game::Unit&>(*entity);
    const uint16_t scaleField = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    const uint32_t scaleBits = scaleField != 0xFFFF ? unit.getField(scaleField) : 0u;
    // The native display's, whatever form it is drawn in (0x006d78c0 reads
    // +0xf8).
    const uint16_t nativeField = game::fieldIndex(game::UF::UNIT_FIELD_NATIVEDISPLAYID);
    const uint32_t native = nativeField != 0xFFFF ? unit.getField(nativeField) : 0u;
    const uint32_t displayId = native != 0 ? native : unit.getDisplayId();
    if (displayId == collisionHeightDisplay_ && scaleBits == collisionHeightScaleBits_) {
        return collisionHeight_;
    }
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    const int32_t display = displays ? displays->findRecordById(displayId) : -1;
    if (display < 0 || !models || models->getFieldCount() <= us::kModelCollisionHeightColumn) {
        return collisionHeight_;
    }
    const auto drow = static_cast<uint32_t>(display);
    const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
    if (model < 0) return collisionHeight_;
    const auto mrow = static_cast<uint32_t>(model);
    const float objectScale = scaleField != 0xFFFF ? std::bit_cast<float>(scaleBits) : 1.0f;
    collisionHeight_ = us::collisionHeight(models->getFloat(mrow, us::kModelCollisionHeightColumn), objectScale,
                                           displays->getFloat(drow, us::kDisplayScaleColumn),
                                           models->getFloat(mrow, us::kModelScaleColumn));
    if (models->getFieldCount() > us::kModelCollisionWidthColumn) {
        const float width = us::collisionHeight(models->getFloat(mrow, us::kModelCollisionWidthColumn), objectScale,
                                                displays->getFloat(drow, us::kDisplayScaleColumn),
                                                models->getFloat(mrow, us::kModelScaleColumn));
        if (width > 0.01f && width < 50.0f) collisionHalfWidth_ = width * 0.5f;
    }
    collisionHeightDisplay_ = displayId;
    collisionHeightScaleBits_ = scaleBits;
    return collisionHeight_;
}

float EntitySpawner::playerMountedCollisionHeight(uint32_t mountDisplayId) {
    namespace us = rendering::unit_splash;
    constexpr uint32_t kModelMountHeightColumn = 16;  // CreatureModelData +0x40
    if (!gameHandler_ || !assetManager_) return 0.0f;
    auto entity = gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid());
    if (!entity || !entity->isUnit()) return 0.0f;
    const auto& unit = static_cast<const game::Unit&>(*entity);
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    if (!displays || !models || models->getFieldCount() <= kModelMountHeightColumn) return 0.0f;
    const uint16_t nativeField = game::fieldIndex(game::UF::UNIT_FIELD_NATIVEDISPLAYID);
    const uint32_t native = nativeField != 0xFFFF ? unit.getField(nativeField) : 0u;
    const int32_t display = displays->findRecordById(native != 0 ? native : unit.getDisplayId());
    const int32_t mountDisplay = displays->findRecordById(mountDisplayId);
    if (display < 0 || mountDisplay < 0) return 0.0f;
    const auto drow = static_cast<uint32_t>(display);
    const auto mdrow = static_cast<uint32_t>(mountDisplay);
    const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
    const int32_t mountModel = models->findRecordById(displays->getUInt32(mdrow, 1));
    if (model < 0 || mountModel < 0) return 0.0f;
    const auto mrow = static_cast<uint32_t>(model);
    const uint16_t scaleField = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    const uint32_t scaleBits = scaleField != 0xFFFF ? unit.getField(scaleField) : 0u;
    const float objectScale = scaleBits != 0 ? std::bit_cast<float>(scaleBits) : 1.0f;
    // The rider's scale as its standing height has it (us::collisionHeight).
    const float scale = us::collisionHeight(1.0f, objectScale, displays->getFloat(drow, us::kDisplayScaleColumn),
                                            models->getFloat(mrow, us::kModelScaleColumn));
    float mountScale = displays->getFloat(mdrow, us::kDisplayScaleColumn);
    if (!(mountScale > 0.0f)) mountScale = 1.0f;
    return rendering::camera_pivot::mountedCollisionHeight(
        models->getFloat(mrow, us::kModelCollisionHeightColumn),
        models->getFloat(static_cast<uint32_t>(mountModel), kModelMountHeightColumn), mountScale, scale);
}

bool EntitySpawner::noRoomToMount(uint32_t mountDisplay) const {
    namespace us = rendering::unit_splash;
    namespace col = rendering::collision;
    constexpr uint32_t kModelMountHeightColumn = 16;  // CreatureModelData MountHeight (+0x40)
    auto* cc = renderer_ ? renderer_->getCameraController() : nullptr;
    if (!gameHandler_ || !assetManager_ || !cc) return false;
    const auto unit =
        std::dynamic_pointer_cast<game::Unit>(gameHandler_->getEntityManager().getEntity(gameHandler_->getPlayerGuid()));
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    if (!unit || !displays || !models || models->getFieldCount() <= kModelMountHeightColumn) return false;
    const auto field = [&](game::UF f) -> uint32_t {
        const uint16_t i = game::fieldIndex(f);
        return i != 0xFFFF ? unit->getField(i) : 0u;
    };
    // The native display's body (+0xf8), and the mount's.
    const int32_t display = displays->findRecordById(field(game::UF::UNIT_FIELD_NATIVEDISPLAYID));
    const int32_t mount = displays->findRecordById(mountDisplay);
    if (display < 0 || mount < 0) return false;
    const auto drow = static_cast<uint32_t>(display);
    const auto mdrow = static_cast<uint32_t>(mount);
    const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
    const int32_t mountModel = models->findRecordById(displays->getUInt32(mdrow, 1));
    if (model < 0 || mountModel < 0) return false;
    const auto mrow = static_cast<uint32_t>(model);
    // 0x006cf350.
    const float scale = us::collisionHeight(1.0f, std::bit_cast<float>(field(game::UF::OBJECT_FIELD_SCALE_X)),
                                            displays->getFloat(drow, us::kDisplayScaleColumn),
                                            models->getFloat(mrow, us::kModelScaleColumn));
    const auto box = col::mountingBox(renderer_->getCharacterPosition(),
                                      models->getFloat(mrow, us::kModelCollisionWidthColumn) * scale * 0.5f,
                                      models->getFloat(mrow, us::kModelCollisionHeightColumn),
                                      models->getFloat(static_cast<uint32_t>(mountModel), kModelMountHeightColumn),
                                      displays->getFloat(mdrow, us::kDisplayScaleColumn), scale);
    if (!box) return false;
    // 0x0075e480: a gather that fails is no room.
    const auto facets = cc->gatherWorldFacets(box->lo, box->hi);
    return !facets || col::boxOverlapsFacets(box->lo, box->hi, *facets);
}

bool EntitySpawner::growthNoLongerFits(uint64_t guid, float oldScale) {
    namespace us = rendering::unit_splash;
    namespace col = rendering::collision;
    // CreatureModelData MountHeight (+0x40).
    constexpr uint32_t kModelMountHeightColumn = 16;
    if (!gameHandler_ || !assetManager_ || !renderer_) return false;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    // 0x00725f50: a unit that is not a player always fits.
    if (!entity || entity->getType() != game::ObjectType::PLAYER) return false;
    const auto& unit = static_cast<const game::Unit&>(*entity);
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    if (!displays || !models || models->getFieldCount() <= kModelMountHeightColumn) return false;
    const auto field = [&](game::UF f) -> uint32_t {
        const uint16_t i = game::fieldIndex(f);
        return i != 0xFFFF ? unit.getField(i) : 0u;
    };
    const float newScale = std::bit_cast<float>(field(game::UF::OBJECT_FIELD_SCALE_X));
    // 0x0072a560: the fit is tested only while the unit grows, and only for
    // the active player (0x004cee50).
    const bool tested = col::scaleGrew(newScale, oldScale) && guid == gameHandler_->getPlayerGuid();
    // A display's record and its model's, or nothing.
    struct Rows {
        uint32_t display;
        uint32_t model;
    };
    const auto rowsOf = [&](uint32_t displayId) -> std::optional<Rows> {
        const int32_t d = displays->findRecordById(displayId);
        if (d < 0) return std::nullopt;
        const int32_t m = models->findRecordById(displays->getUInt32(static_cast<uint32_t>(d), 1));
        if (m < 0) return std::nullopt;
        return Rows{static_cast<uint32_t>(d), static_cast<uint32_t>(m)};
    };
    // 0x006cf350: the collision scale, the object scale at least 1 times the
    // display's and the model's.
    const auto collisionScale = [&](const Rows& r, float objectScale) {
        return us::collisionHeight(1.0f, objectScale, displays->getFloat(r.display, us::kDisplayScaleColumn),
                                   models->getFloat(r.model, us::kModelScaleColumn));
    };
    // The collision box is the native display's (unit +0xf8), whatever form
    // the unit is drawn in.
    const auto native = rowsOf(field(game::UF::UNIT_FIELD_NATIVEDISPLAYID));
    auto* cc = renderer_->getCameraController();
    const glm::vec3 feet = renderer_->getCharacterPosition();
    const auto mountDisplay = static_cast<int32_t>(field(game::UF::UNIT_FIELD_MOUNTDISPLAYID));
    if (mountDisplay > 0) {
        // 0x0072a5a2: mounted on a display the client has no model for, it
        // asks for the growth to go without testing anything.
        const auto mount = rowsOf(static_cast<uint32_t>(mountDisplay));
        if (!mount) return true;
        // 0x006d7720; a rider with no model is a fatal error there.
        if (!native) return false;
        const float riderHeight = models->getFloat(native->model, us::kModelCollisionHeightColumn);
        const float riderWidth = models->getFloat(native->model, us::kModelCollisionWidthColumn);
        const float mountHeight = models->getFloat(mount->model, kModelMountHeightColumn);
        // +0x990: the mount display's scale.
        const float mountScale = displays->getFloat(mount->display, us::kDisplayScaleColumn);
        if (!tested || !cc) return false;
        const float oldCollisionScale = collisionScale(*native, oldScale);
        // The body as it is (+0x850, +0x854): the rider's half-width and its
        // height on the mount at the old scale.
        const float halfWidth = riderWidth * oldCollisionScale * 0.5f;
        const float currentHeight =
            col::mountedRiderHeight(riderHeight, mountHeight, mountScale) * oldCollisionScale;
        const auto box = col::mountedGrowthBox(feet, halfWidth, currentHeight, riderHeight, mountHeight,
                                               mountScale, collisionScale(*native, newScale));
        if (!box) return false;
        // 0x0075e480: a gather that fails is no room.
        const auto facets = cc->gatherWorldFacets(box->lo, box->hi);
        return !facets || col::boxOverlapsFacets(box->lo, box->hi, *facets);
    }
    // 0x006d78c0; a unit with no model is a fatal error there.
    if (!native) return false;
    const float width = models->getFloat(native->model, us::kModelCollisionWidthColumn);
    const float height = models->getFloat(native->model, us::kModelCollisionHeightColumn);
    // NOCOLLISIONBOX, ZEROSCALEUNIT: logged, and it fits.
    constexpr float kZero = 9.536743e-07f;
    if (std::abs(width) < kZero || std::abs(height) < kZero) return false;
    const float scale = collisionScale(*native, newScale);
    if (std::abs(scale) < kZero) return false;
    if (!tested || !cc) return false;
    // 0x006e8ff0 -> 0x0075e500: the movement body at the grown size, and
    // the faces round it.
    const col::FitExtents ext = col::growthFitExtents(width, height, scale);
    const col::Body body(feet, ext.halfWidth, ext.height);
    const auto facets = cc->gatherWorldFacets(body.boxMin(), body.boxMax());
    return !facets || col::bodyOverlapsFacets(body, *facets);
}

std::pair<float, float> EntitySpawner::unitRippleSize(const game::Unit& unit) {
    namespace us = rendering::unit_splash;
    const uint16_t scaleField = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    const uint32_t scaleBits = scaleField != 0xFFFF ? unit.getField(scaleField) : 0u;
    float scale = scaleBits != 0 ? std::bit_cast<float>(scaleBits) : 1.0f;
    if (!(scale > 0.01f && scale <= 100.0f)) scale = 1.0f;
    const uint64_t key = (static_cast<uint64_t>(unit.getDisplayId()) << 32) | scaleBits;
    if (const auto it = rippleHeights_.find(key); it != rippleHeights_.end()) return {scale, it->second};
    float height = us::kDefaultCollisionHeight * scale;
    if (assetManager_) {
        auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
        auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
        const int32_t display = displays ? displays->findRecordById(unit.getDisplayId()) : -1;
        if (display >= 0 && models && models->getFieldCount() > us::kModelCollisionHeightColumn) {
            const auto drow = static_cast<uint32_t>(display);
            const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
            if (model >= 0) {
                const auto mrow = static_cast<uint32_t>(model);
                height = us::collisionHeight(models->getFloat(mrow, us::kModelCollisionHeightColumn), scale,
                                             displays->getFloat(drow, us::kDisplayScaleColumn),
                                             models->getFloat(mrow, us::kModelScaleColumn));
            }
        }
    }
    rippleHeights_[key] = height;
    return {scale, height};
}

float EntitySpawner::modelCollisionHeight(const game::Unit& unit) const {
    namespace us = rendering::unit_splash;
    auto displays = assetManager_ ? assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc") : nullptr;
    auto models = assetManager_ ? assetManager_->loadDBCOptional("CreatureModelData.dbc") : nullptr;
    const int32_t display = displays ? displays->findRecordById(unit.getDisplayId()) : -1;
    if (display < 0 || !models || models->getFieldCount() <= us::kModelCollisionHeightColumn)
        return us::kDefaultCollisionHeight;
    const auto drow = static_cast<uint32_t>(display);
    const int32_t model = models->findRecordById(displays->getUInt32(drow, 1));
    if (model < 0) return us::kDefaultCollisionHeight;
    const auto mrow = static_cast<uint32_t>(model);
    const uint16_t scaleField = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    const float objectScale = scaleField != 0xFFFF ? std::bit_cast<float>(unit.getField(scaleField)) : 1.0f;
    return us::collisionHeight(models->getFloat(mrow, us::kModelCollisionHeightColumn), objectScale,
                               displays->getFloat(drow, us::kDisplayScaleColumn),
                               models->getFloat(mrow, us::kModelScaleColumn));
}

bool EntitySpawner::deepInLiquid(const game::Unit& unit) const {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) return false;
    const uint64_t guid = unit.getGuid();
    uint32_t drawn = getPlayerInstanceId(guid);
    if (drawn == 0) drawn = getCreatureInstanceId(guid);
    if (const RemotePlayerMount* mount = getRemotePlayerMount(guid); mount && mount->instanceId != 0)
        drawn = mount->instanceId;
    glm::vec3 at;
    if (drawn == 0 || !cr->getInstancePosition(drawn, at)) return false;
    const auto liquid = rendering::liquid_query::atUnit(renderer_->liquidWorld(), at);
    return liquid && game::deepInLiquid(liquid->level - at.z, modelCollisionHeight(unit));
}

void EntitySpawner::playUnitSplashes(uint32_t localPlayerInstance) {
    namespace us = rendering::unit_splash;
    rendering::CharacterRenderer* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_ || !assetManager_) return;
    auto races = assetManager_->loadDBCOptional("ChrRaces.dbc");
    if (!races || races->getFieldCount() <= us::kChrRacesSplashSoundColumn) return;
    const auto& entities = gameHandler_->getEntityManager();
    const auto& creatureInfo = gameHandler_->getCreatureInfoCache();
    const uint64_t self = gameHandler_->getPlayerGuid();
    const uint64_t mover = gameHandler_->activeMover();
    const bool listenerAtCharacter = addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";
    const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();

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
        if (const auto liquid = rendering::liquid_query::atUnit(renderer_->liquidWorld(), at))
            depth = liquid->level - at.z;
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
        float height = byModel ? modelCollisionHeight(unit) : us::kDefaultCollisionHeight;
        if (guid == self && renderer_->getCameraController()) height = renderer_->getCameraController()->collisionHeight();
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

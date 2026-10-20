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

#include <chrono>

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
            // The active player's own, with the listener at the character,
            // where the listener is and fainter (0x00747310).
            if (activePlayer && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0") {
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
        case ue::Kind::None:
            break;
        }
    }
}

}  // namespace wowee::core

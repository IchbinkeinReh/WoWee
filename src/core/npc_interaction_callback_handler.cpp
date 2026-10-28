#include "core/npc_interaction_callback_handler.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "game/game_handler.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/npc_voice_manager.hpp"

namespace wowee { namespace core {

NPCInteractionCallbackHandler::NPCInteractionCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer* renderer,
    game::GameHandler& gameHandler,
    audio::AudioCoordinator* audioCoordinator)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
    , audioCoordinator_(audioCoordinator)
{
}

std::pair<uint32_t, audio::VoiceType> NPCInteractionCallbackHandler::resolveNpcVoice(uint64_t guid) const {
    auto entity = gameHandler_.getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::UNIT)
        return {0, audio::VoiceType::GENERIC};
    auto unit = std::static_pointer_cast<game::Unit>(entity);
    const uint32_t displayId = unit->getDisplayId();
    // The display is asked for its own sake now - its row names the kit the line
    // comes from - and the race guess it feeds is only what the race library is
    // asked for when the display names no kit.
    return {displayId, entitySpawner_.detectVoiceTypeFromDisplayId(displayId)};
}

void NPCInteractionCallbackHandler::setupCallbacks() {
    // NPC greeting callback - play voice line
    gameHandler_.setNpcGreetingCallback([this](uint64_t guid, const glm::vec3& position) {
        // Play NPC_WELCOME animation on the NPC
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            if (cr) {
                uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
                if (instanceId != 0) cr->playAnimation(instanceId, rendering::anim::NPC_WELCOME, false);
            }
        }
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            // Convert canonical to render coords for 3D audio
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            const auto [displayId, voiceType] = resolveNpcVoice(guid);
            audioCoordinator_->getNpcVoiceManager()->playGreeting(guid, displayId, voiceType, renderPos);
        }
    });

    // NPC farewell callback - play farewell voice line
    gameHandler_.setNpcFarewellCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            const auto [displayId, voiceType] = resolveNpcVoice(guid);
            audioCoordinator_->getNpcVoiceManager()->playFarewell(guid, displayId, voiceType, renderPos);
        }
    });

    // NPC vendor callback - play vendor voice line
    gameHandler_.setNpcVendorCallback([this](uint64_t guid, const glm::vec3& position) {
        if (audioCoordinator_ && audioCoordinator_->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);
            const auto [displayId, voiceType] = resolveNpcVoice(guid);
            audioCoordinator_->getNpcVoiceManager()->playVendor(guid, displayId, voiceType, renderPos);
        }
    });
    // An angry unit's line is not asked here: the client asks it of the unit
    // itself as its reaction changes (SMSG_AI_REACTION, handled through
    // AnimationCallbackHandler), out of its own sound row rather than out of a
    // race library.
}

}} // namespace wowee::core

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
    game::GameHandler& gameHandler,
    audio::AudioCoordinator* audioCoordinator)
    : gameHandler_(gameHandler)
    , audioCoordinator_(audioCoordinator)
{
}

void NPCInteractionCallbackHandler::setupCallbacks() {
    gameHandler_.setNpcLineCallback([this](uint64_t guid, game::GameHandler::NpcLine line, const glm::vec3& position) {
        auto* voices = audioCoordinator_ ? audioCoordinator_->getNpcVoiceManager() : nullptr;
        const auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (!voices || !entity || entity->getType() != game::ObjectType::UNIT) return;
        const uint32_t displayId = static_cast<const game::Unit&>(*entity).getDisplayId();
        const glm::vec3 at = core::coords::canonicalToRender(position);
        switch (line) {
            case game::GameHandler::NpcLine::Click: voices->playClick(guid, displayId, at); break;
            case game::GameHandler::NpcLine::Hello: voices->playHello(guid, displayId, at); break;
            case game::GameHandler::NpcLine::Farewell: voices->playFarewell(guid, displayId, at); break;
        }
    });
}

}} // namespace wowee::core

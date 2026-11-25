#pragma once

#include <cstdint>
#include <utility>
#include <glm/glm.hpp>
#include "audio/npc_voice_manager.hpp"

namespace wowee {

namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; }

namespace core {

/// What a unit spoken to says, and its animation as it is.
class NPCInteractionCallbackHandler {
public:
    NPCInteractionCallbackHandler(game::GameHandler& gameHandler, audio::AudioCoordinator* audioCoordinator);

    void setupCallbacks();

private:
    game::GameHandler& gameHandler_;
    audio::AudioCoordinator* audioCoordinator_;
};

} // namespace core
} // namespace wowee

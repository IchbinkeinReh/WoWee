#pragma once

#include <cstdint>
#include <utility>
#include <glm/glm.hpp>
#include "audio/npc_voice_manager.hpp"

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace core { class EntitySpawner; }

namespace core {

/// Handles NPC interaction callbacks: greeting, farewell, vendor, aggro voice lines.
class NPCInteractionCallbackHandler {
public:
    NPCInteractionCallbackHandler(EntitySpawner& entitySpawner,
                                  rendering::Renderer* renderer,
                                  game::GameHandler& gameHandler,
                                  audio::AudioCoordinator* audioCoordinator);

    void setupCallbacks();

private:
    /// The unit's display id and the voice type guessed from it, in that order:
    /// one lookup of the unit instead of four copies of it. The display id is what
    /// names the voice kit the line is taken from; the guessed type is only what
    /// the race library is asked for when the display names no kit.
    [[nodiscard]] std::pair<uint32_t, audio::VoiceType> resolveNpcVoice(uint64_t guid) const;

    EntitySpawner& entitySpawner_;
    rendering::Renderer* renderer_;
    game::GameHandler& gameHandler_;
    audio::AudioCoordinator* audioCoordinator_;
};

} // namespace core
} // namespace wowee

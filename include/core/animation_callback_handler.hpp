#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace core { class AppearanceComposer; class EntitySpawner; }

namespace core {

/// Handles animation callbacks: death, respawn, swing, hit reaction, spell cast, emote,
/// stun, stealth, health, ghost, stand state, loot, sprint, vehicle.
class AnimationCallbackHandler {
public:
    AnimationCallbackHandler(EntitySpawner& entitySpawner,
                             rendering::Renderer& renderer,
                             game::GameHandler& gameHandler);

    void setupCallbacks();

private:
    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    game::GameHandler& gameHandler_;
};

} // namespace core
} // namespace wowee

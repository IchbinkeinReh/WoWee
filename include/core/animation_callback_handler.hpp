#pragma once

#include <unordered_map>

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
    /// The spell each other unit last began casting, for its end
    /// (0x007fec00 asks the base animation again where its PrecastKit
    /// animates the unit).
    std::unordered_map<uint64_t, uint32_t> castSpells_;
};

} // namespace core
} // namespace wowee

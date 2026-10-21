#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <unordered_map>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace core { class EntitySpawner; }

namespace core {

/// Handles entity spawn/despawn callbacks: creatures, players, game objects.
class EntitySpawnCallbackHandler {
public:
    /// @param isLocalPlayerGuid Returns true if the given GUID is the local player (to skip self-spawn)
    EntitySpawnCallbackHandler(EntitySpawner& entitySpawner,
                               rendering::Renderer& renderer,
                               game::GameHandler& gameHandler,
                               std::function<bool(uint64_t)> isLocalPlayerGuid);

    void setupCallbacks();

private:
    EntitySpawner& entitySpawner_;
    rendering::Renderer& renderer_;
    game::GameHandler& gameHandler_;
    std::function<bool(uint64_t)> isLocalPlayerGuid_;

    // Where each creature was last asked to be drawn, with the server's own
    // display: a mirror look arrives after the unit and draws it again there.
    struct LastCreatureSpawn {
        uint32_t displayId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f, scale = 1.0f;
    };
    std::unordered_map<uint64_t, LastCreatureSpawn> lastCreatureSpawns_;
};

} // namespace core
} // namespace wowee

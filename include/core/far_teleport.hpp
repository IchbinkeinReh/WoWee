#pragma once

#include <glm/vec3.hpp>
#include <glm/geometric.hpp>

namespace wowee {
namespace core {

/// A teleport of the player's own on the same map that the client takes
/// behind a loading screen: more than 150 yards (0x006ecf80 -> 0x0074b620 ->
/// 0x00781500). The screen's end unloads the world's sound (0x00409550).
inline bool teleportShowsLoadingScreen(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = from - to;
    return glm::dot(d, d) > 150.0f * 150.0f;
}

}  // namespace core
}  // namespace wowee

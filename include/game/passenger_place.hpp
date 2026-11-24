#pragma once

#include "core/coordinates.hpp"

#include <cmath>
#include <glm/glm.hpp>

namespace wowee::game {

/// Where a vehicle's passenger is (0x004f4460): its vehicle's place and the offset its
/// movement holds on it - x ahead, y to the left, as the wire has it - turned by the
/// vehicle's facing. Canonical places, the facing canonical too.
[[nodiscard]] inline glm::vec3 passengerPlace(const glm::vec3& vehicle, float vehicleYaw, const glm::vec3& wireOffset) {
    const float yaw = core::coords::canonicalToServerYaw(vehicleYaw);
    const glm::vec3 turned(wireOffset.x * std::cos(yaw) - wireOffset.y * std::sin(yaw),
                           wireOffset.x * std::sin(yaw) + wireOffset.y * std::cos(yaw), wireOffset.z);
    return core::coords::serverToCanonical(core::coords::canonicalToServer(vehicle) + turned);
}

}  // namespace wowee::game

#pragma once

// Where a SpellCastTargets point is in the world.
//
// The client keeps each of the targets' two points with the guid it was read
// with (0x009ab9c0: the source's at +0x40/+0x50, the destination's at
// +0x48/+0x5c): the object whose frame the point is in, 0 for the world's.
// Asked where the destination is, it answers the point itself for 0, and
// otherwise the point carried through that object's world matrix
// (0x009ab810 -> 0x0074b4c0 -> 0x0074b430) - an identity one when it does
// not know the object, so the point is then taken as it came.
//
// Points come in wire coordinates; what is answered here is canonical.

#include "core/coordinates.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::game::spell_target_point {

/// A point in the frame of an object placed at `canonicalPosition` facing
/// `canonicalYaw` - a unit's or a game object's world matrix (vtable +0xc4):
/// turned by the object's facing about its up axis, then moved to it. The
/// point is the wire's, in the object's own axes.
inline glm::vec3 inObjectFrame(const glm::vec3& canonicalPosition, float canonicalYaw,
                               const glm::vec3& wireLocal) {
    const glm::vec3 origin = core::coords::canonicalToServer(canonicalPosition);
    const float facing = core::coords::canonicalToServerYaw(canonicalYaw);
    const float c = std::cos(facing), s = std::sin(facing);
    const glm::vec3 wireWorld(origin.x + wireLocal.x * c - wireLocal.y * s,
                              origin.y + wireLocal.x * s + wireLocal.y * c,
                              origin.z + wireLocal.z);
    return core::coords::serverToCanonical(wireWorld);
}

/// The point in the world, canonical. `objectFrame(guid, wirePoint)` answers
/// the canonical world place of a point in the frame of the object `guid`
/// names, or nullopt when that object is not known - the point is then the
/// wire's as it came (0x0074b430's identity).
template <class ObjectFrame>
glm::vec3 worldPoint(uint64_t frameGuid, const glm::vec3& wirePoint, ObjectFrame&& objectFrame) {
    if (frameGuid != 0) {
        if (const std::optional<glm::vec3> placed = objectFrame(frameGuid, wirePoint)) return *placed;
    }
    return core::coords::serverToCanonical(wirePoint);
}

}  // namespace wowee::game::spell_target_point

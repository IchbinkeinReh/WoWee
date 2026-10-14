#pragma once

// The axes a passenger's offset on a transport is kept in, and the wire's,
// and the facing it has there.
//
// The wire's offset is in the transport's own frame: boarding puts the
// passenger's place through the inverse of the transport's matrix
// (0x004f47b0 -> 0x0074b4c0 -> 0x004c2300; a game object's matrix is
// 0x00712ee0's), and the place sent is the offset put back through it
// (0x00987140 -> 0x004f4460).
//
// A WMO transport's offsets are used as the wire has them: its render
// transform takes the model's own axes directly. An M2 one keeps a canonical
// delta from its place, unturned, so the wire's is that delta turned back by
// the transport's facing (transportFacing, ActiveTransport::facing). Whatever
// comes in through fromWire goes back out through toWire, so the server is
// sent its own offset back.

#include "core/coordinates.hpp"

#include <cmath>
#include <glm/glm.hpp>

namespace wowee::game::transport_local {

// The wire's axes turned by angle about +Z (the wire's facings turn
// counter-clockwise from +X).
inline glm::vec3 turnOnWire(const glm::vec3& v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

inline glm::vec3 fromWire(bool wmo, const glm::vec3& wireOffset, float transportFacing = 0.0f) {
    return wmo ? wireOffset
               : core::coords::serverToCanonical(turnOnWire(wireOffset, transportFacing));
}

inline glm::vec3 toWire(bool wmo, const glm::vec3& localOffset, float transportFacing = 0.0f) {
    return wmo ? localOffset
               : turnOnWire(core::coords::canonicalToServer(localOffset), -transportFacing);
}

// A passenger's facing is kept on its transport, not in the world. The wire's
// transport orientation is that facing as the client stores it (0x009872c0
// puts it at the movement's +0x20 as it comes) and sends it back (0x00987140
// writes +0x20 as it is). Its facing in the world is the transport's own
// plus it (0x004f42a0), and boarding takes the transport's off (0x004f47b0),
// kept in [0, 2pi) (0x004c5090). So a transport that turns carries its
// passengers round with it.
//
// transportFacing is the transport's own (its GetFacing, vtable +0x34), in
// the wire's convention - ActiveTransport::facing.

// The canonical yaw a passenger faces in the world.
inline float worldYaw(float transportFacing, float localFacing) {
    return core::coords::serverToCanonicalYaw(
        core::coords::normalizeAngleRad(transportFacing + localFacing));
}

// The wire's transport orientation for a passenger facing canonicalYaw.
inline float localFacing(float transportFacing, float canonicalYaw) {
    float facing = std::fmod(core::coords::canonicalToServerYaw(canonicalYaw) - transportFacing,
                             core::coords::TWO_PI);
    if (facing < 0.0f) facing += core::coords::TWO_PI;
    return facing;
}

}  // namespace wowee::game::transport_local

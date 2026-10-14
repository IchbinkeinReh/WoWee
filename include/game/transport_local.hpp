#pragma once

// The axes a passenger's offset on a transport is kept in, and the wire's,
// and the facing it has there.
//
// A WMO transport's offsets are used as the wire has them: its render
// transform takes the model's own axes directly. An M2 one keeps a canonical
// delta from its place. Whatever comes in through fromWire goes back out
// through toWire, so the server is sent its own offset back.

#include "core/coordinates.hpp"

#include <cmath>
#include <glm/glm.hpp>

namespace wowee::game::transport_local {

inline glm::vec3 fromWire(bool wmo, const glm::vec3& wireOffset) {
    return wmo ? wireOffset : core::coords::serverToCanonical(wireOffset);
}

inline glm::vec3 toWire(bool wmo, const glm::vec3& localOffset) {
    return wmo ? localOffset : core::coords::canonicalToServer(localOffset);
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

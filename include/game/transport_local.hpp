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
// A passenger's place in the world is its offset put through the transport's
// matrix (0x004f4460 -> 0x0074b4c0 -> the object's +0xc4, 0x00712ee0's
// matrix -> 0x004c21b0), every time it is asked: a transport that turns
// carries its passengers round with it.
//
// A WMO transport's offsets are kept as the wire has them, an M2 one's on
// canonical axes - the same offset, its x and y swapped - and either is put
// through the same matrix. Whatever comes in through fromWire goes back out
// through toWire, so the server is sent its own offset back.

#include "core/coordinates.hpp"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::game::transport_local {

inline glm::vec3 fromWire(bool wmo, const glm::vec3& wireOffset) {
    return wmo ? wireOffset : core::coords::serverToCanonical(wireOffset);
}

inline glm::vec3 toWire(bool wmo, const glm::vec3& localOffset) {
    return wmo ? localOffset : core::coords::canonicalToServer(localOffset);
}

// The transport's matrix (0x00712ee0): its place, then its rotation, on the
// wire's axes - which are the renderer's.
inline glm::mat4 frame(const glm::vec3& canonicalPosition, const glm::quat& rotation) {
    return glm::translate(glm::mat4(1.0f), core::coords::canonicalToRender(canonicalPosition)) *
           glm::mat4_cast(rotation);
}

// A passenger's canonical place in the world, from its offset (0x004f4460).
inline glm::vec3 toWorld(bool wmo, const glm::mat4& transportFrame, const glm::vec3& localOffset) {
    const glm::vec4 world = transportFrame * glm::vec4(toWire(wmo, localOffset), 1.0f);
    return core::coords::renderToCanonical(glm::vec3(world));
}

// And its offset from a canonical place: through the inverse of the matrix,
// as boarding does (0x004f47b0 -> 0x004c2300).
inline glm::vec3 toLocal(bool wmo, const glm::mat4& transportFrame, const glm::vec3& canonicalWorld) {
    const glm::vec4 local =
        glm::inverse(transportFrame) * glm::vec4(core::coords::canonicalToRender(canonicalWorld), 1.0f);
    return fromWire(wmo, glm::vec3(local));
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

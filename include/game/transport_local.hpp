#pragma once

// The axes a passenger's offset on a transport is kept in, and the wire's.
//
// A WMO transport's offsets are used as the wire has them: its render
// transform takes the model's own axes directly. An M2 one keeps a canonical
// delta from its place. Whatever comes in through fromWire goes back out
// through toWire, so the server is sent its own offset back.

#include "core/coordinates.hpp"

#include <glm/glm.hpp>

namespace wowee::game::transport_local {

inline glm::vec3 fromWire(bool wmo, const glm::vec3& wireOffset) {
    return wmo ? wireOffset : core::coords::serverToCanonical(wireOffset);
}

inline glm::vec3 toWire(bool wmo, const glm::vec3& localOffset) {
    return wmo ? localOffset : core::coords::canonicalToServer(localOffset);
}

}  // namespace wowee::game::transport_local

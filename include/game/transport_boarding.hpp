#pragma once

// Which transport the player is on, decided as the client decides it: by
// what the ground under its feet belongs to.
//
// After a step the client hands the owner of the face its feet stand on to
// 0x006ec7b0 (0x007620f0, 0x00762980; a fall only once it lands on
// something an object owns, 0x007612b0), which makes it the transport
// through 0x006ec400 - but only an object whose GAMEOBJECT_FLAGS have
// GO_FLAG_TRANSPORT (0x0074b3f0 asks vtable +0xec, 0x00712f20). Ground no
// object owns, the world's, takes the mover off its transport, unless the
// transport says the mover is still within it (0x0074b5e0 -> vtable +0xf0
// -> 0x0070b360 -> 0x0077ffb0); ground some other object owns changes
// nothing.

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::game::transport_boarding {

// GO_FLAG_TRANSPORT.
constexpr uint32_t kGoFlagTransport = 0x8;

// 0x00712f20: (GAMEOBJECT_FLAGS >> 3) & 1.
inline bool isTransportObject(uint32_t gameObjectFlags) {
    return (gameObjectFlags & kGoFlagTransport) != 0;
}

// The transport after a step onto ground `owner` owns (0 for the world's),
// from `current`. `ownerIsTransport` is 0x0074b3f0's answer for the owner;
// `currentStillHolds` the current transport's (0x0070b360) for the mover.
inline uint64_t afterGroundStep(uint64_t current, uint64_t owner, bool ownerIsTransport,
                                bool currentStillHolds) {
    if (owner == 0) return (current != 0 && currentStillHolds) ? current : 0;
    // 0x006ec400 does nothing for an owner that is not a transport.
    return ownerIsTransport ? owner : current;
}

// 0x0077ffb0 for an M2 transport: the mover's place on it, in its frame,
// inside the model's collision box (the header's +0xbc) with its top raised
// by 1.6404123 + 0.013888889 - room for someone standing on top.
inline bool insideCollisionBox(const glm::vec3& localWire, const glm::vec3& boxMin,
                               const glm::vec3& boxMax) {
    constexpr float kHeadroom = 1.6404123f + 0.013888889f;
    return localWire.x <= boxMax.x && localWire.y <= boxMax.y && localWire.z <= boxMax.z + kHeadroom &&
           localWire.x >= boxMin.x && localWire.y >= boxMin.y && localWire.z >= boxMin.z;
}

}  // namespace wowee::game::transport_boarding

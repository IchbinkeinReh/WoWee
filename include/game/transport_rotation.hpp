#pragma once

// An elevator's or a tram's turn along its path, from TransportRotation.dbc.
//
// A transport game object (type 11) is posed each frame by 0x007139e0, which
// asks 0x007106d0 for its place and its rotation at the path time. The
// rotation is 0x0070dc10's: with fewer than two rows for the object's entry
// it is the object's own (vtable +0x44) and the transport never turns; with
// two or more, the two rows either side of the path time slerped
// (0x00982460) and multiplied by the object's GAMEOBJECT_PARENTROTATION
// (0x004f4320). 0x007139e0 then makes that the object's rotation (0x004f43b0,
// +0x2c bit 2), so its facing (0x004f4630) and its matrix follow it.
//
// Quaternions here are on the wire's axes, (x, y, z, w) as the DBC and the
// field carry them.

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::game::transport_rotation {

/// One TransportRotation.dbc row: the path time it holds at, and the turn.
struct Key {
    uint32_t timeMs = 0;
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// 0x00982460: the shorter way round, or a when the two are within 2^-21.
inline glm::quat slerp(const glm::quat& a, const glm::quat& b, float t) {
    float cosAngle = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    float sign = 1.0f;
    if (cosAngle < 0.0f) {
        sign = -1.0f;
        cosAngle = -cosAngle;
    }
    const float sinAngle = std::sqrt(std::abs(1.0f - cosAngle * cosAngle));
    if (std::abs(sinAngle) < 4.7683716e-07f) return a;
    const float angle = std::atan2(sinAngle, cosAngle);
    const float wa = std::sin((1.0f - t) * angle) / sinAngle;
    const float wb = sign * std::sin(angle * t) / sinAngle;
    return glm::quat(a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb);
}

/// 0x0070dc10: the rotation at a path time, or none with fewer than two rows
/// (the object keeps its own). The segment after the last row runs to the
/// path's end (its last TransportAnimation node, durationMs), and is weighted
/// as the client weights it: by the distance to the first row's time taken
/// unsigned, so it holds the last row.
inline std::optional<glm::quat> rotationAt(const std::vector<Key>& keys, uint32_t pathTimeMs,
                                           uint32_t durationMs, const glm::quat& parent) {
    if (keys.size() < 2) return std::nullopt;
    for (size_t i = 0; i < keys.size(); ++i) {
        const size_t next = i + 1 == keys.size() ? 0 : i + 1;
        const uint32_t from = keys[i].timeMs;
        uint32_t to = keys[next].timeMs;
        if (to == 0) to = durationMs;
        if (pathTimeMs < from || pathTimeMs >= to) continue;
        const uint32_t span = keys[next].timeMs - from;
        const float t = static_cast<float>(pathTimeMs - from) / static_cast<float>(span);
        return slerp(keys[i].rotation, keys[next].rotation, t) * parent;
    }
    // Before the first row, or past a last row whose wrap the client never
    // finds: it holds the last row.
    return keys.back().rotation * parent;
}

}  // namespace wowee::game::transport_rotation

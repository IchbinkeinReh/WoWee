#pragma once

#include "rendering/blob_shadow.hpp"
#include "rendering/collision_geometry.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The highest face of a placed model's collision at or under (x, y, z) that one stands on: one
/// that faces up - its winding, as the client's collision has it. The underside of a lift's door
/// is none, and who stood in front of it on the building's floor was taken for its passenger.
inline std::optional<float> m2DeckUnder(const glm::mat4& model, const glm::mat4& invModel,
                                        const std::vector<glm::vec3>& verts, const std::vector<uint16_t>& indices,
                                        const std::vector<uint32_t>& triangles, const glm::vec3& at) {
    // World-down through the model's transform, so a car placed with any
    // pitch still sees a ray along gravity.
    const glm::vec3 from(invModel * glm::vec4(at.x, at.y, at.z + 5.0f, 1.0f));
    const glm::vec3 to(invModel * glm::vec4(at.x, at.y, at.z - 10.0f, 1.0f));
    const float length = glm::length(to - from);
    if (length <= 1e-5f) return std::nullopt;
    const glm::vec3 dir = (to - from) / length;
    std::optional<float> best;
    for (const uint32_t ti : triangles) {
        if (static_cast<size_t>(ti) * 3 + 2 >= indices.size()) continue;
        const glm::vec3& a = verts[indices[ti * 3]];
        const glm::vec3& b = verts[indices[ti * 3 + 1]];
        const glm::vec3& c = verts[indices[ti * 3 + 2]];
        if (!blob_shadow::facesUp(model, a, b, c)) continue;
        const float hit = rayTriangleIntersect(from, dir, a, b, c);
        if (hit < 0.0f || hit > length) continue;
        const float z = (model * glm::vec4(from + dir * hit, 1.0f)).z;
        // At or under the probe, and the highest such - the deck rather than
        // whatever structure the car carries beneath it.
        if (z <= at.z && (!best || z > *best)) best = z;
    }
    return best;
}

}  // namespace wowee::rendering

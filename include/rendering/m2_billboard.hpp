#pragma once

// A billboard bone's turn toward the camera, in an M2's own space, for the two
// renderers that pose M2s.

#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The M2 bone flag: a spherical billboard (0x8).
inline constexpr unsigned kM2BoneSphericalBillboard = 0x8;

/// The rotation that faces a card at `pivot` toward `camera`, both in model
/// space, identity where they coincide. M2 model space is X forward, Y left,
/// Z up, and a card is authored in the plane facing +X, so that axis is the one
/// aimed at the viewer. Up is model +Z unless the view is nearly along it, where
/// the cross product collapses and the card would spin.
[[nodiscard]] inline glm::mat4 billboardFacing(const glm::vec3& camera, const glm::vec3& pivot) {
    glm::vec3 toCamera = camera - pivot;
    const float len2 = glm::dot(toCamera, toCamera);
    if (len2 <= 1e-8f) return glm::mat4(1.0f);
    toCamera *= 1.0f / std::sqrt(len2);
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (std::abs(toCamera.z) > 0.999f) up = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(up, toCamera));
    const glm::vec3 trueUp = glm::cross(toCamera, right);
    glm::mat4 face(1.0f);
    face[0] = glm::vec4(toCamera, 0.0f);
    face[1] = glm::vec4(right, 0.0f);
    face[2] = glm::vec4(trueUp, 0.0f);
    return face;
}

}  // namespace wowee::rendering

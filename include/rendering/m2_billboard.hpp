#pragma once

// A billboard bone's turn toward the camera, in an M2's own space, for the two
// renderers that pose M2s.

#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering {

/// The M2 bone flag: a spherical billboard (0x8).
inline constexpr unsigned kM2BoneSphericalBillboard = 0x8;
/// The cylindrical ones, turned about the bone's own X, Y or Z axis
/// (0x0082f0f0's switch on flags & 0x78: 0x10, 0x20, 0x40).
inline constexpr unsigned kM2BoneCylindricalX = 0x10;
inline constexpr unsigned kM2BoneCylindricalY = 0x20;
inline constexpr unsigned kM2BoneCylindricalZ = 0x40;
inline constexpr unsigned kM2BoneAnyBillboard = 0x78;

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

/// A cylindrical billboard's axes (0x0082f0f0, cases 0x10/0x20/0x40): the
/// bone keeps its own axis (`axes`' column for X, Y or Z) and turns about it
/// so the card faces the camera as far as that allows. The client works in
/// view space with the eye's direction e: for Z, Y = Z x e and X = Y x Z; for
/// X, Y = X x e and Z = X x Y; for Y, X = e x Y and Z = X x Y. Here in model
/// space, e the way to the camera. `flag` is one of the three; anything else,
/// or a camera along the axis, keeps `axes`.
[[nodiscard]] inline glm::mat3 billboardCylinder(unsigned flag, const glm::mat3& axes, const glm::vec3& camera,
                                                 const glm::vec3& pivot) {
    glm::vec3 e = camera - pivot;
    const float len2 = glm::dot(e, e);
    if (len2 <= 1e-8f) return axes;
    e *= 1.0f / std::sqrt(len2);
    auto unit = [](const glm::vec3& v, glm::vec3& out) {
        const float l2 = glm::dot(v, v);
        if (l2 <= 1e-10f) return false;
        out = v * (1.0f / std::sqrt(l2));
        return true;
    };
    glm::vec3 x, y, z;
    switch (flag) {
        case kM2BoneCylindricalZ:
            if (!unit(axes[2], z) || !unit(glm::cross(z, e), y)) return axes;
            x = glm::cross(y, z);
            break;
        case kM2BoneCylindricalX:
            if (!unit(axes[0], x) || !unit(glm::cross(x, e), y)) return axes;
            z = glm::cross(x, y);
            break;
        case kM2BoneCylindricalY:
            if (!unit(axes[1], y) || !unit(glm::cross(e, y), x)) return axes;
            z = glm::cross(x, y);
            break;
        default:
            return axes;
    }
    return glm::mat3(x, y, z);
}

}  // namespace wowee::rendering

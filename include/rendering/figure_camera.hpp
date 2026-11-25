#pragma once

/// How a PlayerModel frame looks at its model (CharacterModelBase.cpp).
///
/// As the model comes (0x005977c0 -> 0x005974a0) the frame takes the model's
/// camera of type 1 (0x0095f540(1) -> 0x00827960(1)), the character sheet's, as
/// it stands for the frame's pose at that moment with no facing - and keeps it
/// there: what is turned, moved or scaled after is the model under it. A model
/// without one is looked at from (5.5556, 0, 2.4167), at the middle of its box
/// (0x004f5e80), with a field of view of 0.5. SetCamera's is a camera of the
/// model itself and goes wherever the model does.

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering::figure_camera {

struct Camera {
    glm::vec3 eye{0.0f};
    glm::vec3 target{0.0f};
    float diagonalFov = 0.0f;   ///< radians, as every model's camera's
};

template <typename M2Camera>
[[nodiscard]] Camera of(const M2Camera& camera) {
    return {.eye = camera.positionBase, .target = camera.targetBase, .diagonalFov = camera.fov};
}

/// The camera the frame takes as the model comes; `boxMiddle` is the middle of
/// the model's box.
template <typename Cameras>
[[nodiscard]] Camera asItComes(const Cameras& cameras, const glm::vec3& boxMiddle) {
    for (const auto& camera : cameras)
        if (camera.type == 1) return of(camera);
    return {.eye = glm::vec3(5.5555558f, 0.0f, 2.4166667f), .target = boxMiddle, .diagonalFov = 0.5f};
}

/// How the model stands under the camera it came with, in its own units.
struct Pose {
    glm::vec3 offset{0.0f};
    float facing = 0.0f;
    float scale = 1.0f;
    friend bool operator==(const Pose&, const Pose&) = default;
};

/// What one of the model's own units is in its frame's space.
[[nodiscard]] inline float unit(float cameScale, float ddcScreenHeight) {
    return std::max(std::abs(cameScale) * ddcScreenHeight * (5.0f / 3.0f), 1e-6f);
}

/// A frame's SetPosition, SetModelScale and facing, against the position and
/// the scale it had as the model came. A model is drawn at its position times
/// the frame's effective scale and sized by its scale, the effective scale, the
/// screen's height in DDC and 1.6666 (0x0095fba0).
[[nodiscard]] inline Pose pose(const glm::vec3& position, float modelScale, float facing,
                               const glm::vec3& cameAt, float cameScale, float ddcScreenHeight) {
    Pose out;
    out.offset = (position - cameAt) / unit(cameScale, ddcScreenHeight);
    out.facing = facing;
    out.scale = cameScale != 0.0f ? modelScale / cameScale : 1.0f;
    return out;
}

/// A portrait of a model without a camera for it (0x00619580): from 1.7 radii
/// in front of the middle of its stand's bounds.
[[nodiscard]] inline Camera portraitWithoutOne(const glm::vec3& middle, float radius) {
    return {.eye = middle + glm::vec3(radius * 1.7f, 0.0f, 0.0f), .target = middle, .diagonalFov = 1.57f};
}

}  // namespace wowee::rendering::figure_camera

#pragma once

/// The sphere a model is inside of, whatever it plays: the middle of the
/// header's vertex box (+0xa0) and the header's radius about it (+0xb8), as
/// 0x004f5e80 has it. The header's other box and radius (+0xbc, +0xd4) are its
/// collision's, a man's height or less on many a giant.

#include <algorithm>
#include <glm/glm.hpp>

namespace wowee::rendering {

struct ModelSphere {
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
};

/// Of a model with that box and radius, or without them of the vertices drawn.
[[nodiscard]] inline ModelSphere modelSphere(bool hasBox, const glm::vec3& boxMin, const glm::vec3& boxMax,
                                             float boxRadius, const glm::vec3& drawnMin, const glm::vec3& drawnMax,
                                             float drawnRadius) {
    if (hasBox && boxRadius > 0.01f) return {.centre = (boxMin + boxMax) * 0.5f, .radius = boxRadius};
    if (drawnRadius > 0.01f) return {.centre = (drawnMin + drawnMax) * 0.5f, .radius = drawnRadius};
    return {};
}

/// Where that is for an instance of it, no smaller than `least`.
[[nodiscard]] inline ModelSphere placedSphere(const ModelSphere& sphere, const glm::mat4& model, float scale,
                                              float least) {
    return {.centre = glm::vec3(model * glm::vec4(sphere.centre, 1.0f)),
            .radius = std::max(least, sphere.radius * std::max(0.001f, scale))};
}

}  // namespace wowee::rendering

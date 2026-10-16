#pragma once

/// The scene's depth convention: reversed Z.
///
/// The client's near clip is 0.2 (0x0077f490 sets it whatever the nearclip
/// cvar says), and the camera comes that close to a wall. With the standard
/// mapping a 32-bit float depth keeps its precision at the near plane, where it
/// is least needed, and loses it with the square of the distance over the near
/// clip: at 0.2 a surface a thousand yards off resolved to about a third of a
/// yard, 2.5 times coarser than at the 0.5 this used, and distant terrain and
/// buildings would fight. Reversed - near at 1, far at 0 - the float's own
/// exponent spreads the precision with the distance instead, and the far plane
/// stops mattering.
///
/// The scene's depth buffer, its pipelines and everything that reads it follow
/// this; the shadow maps keep the standard mapping, being their own
/// projections and depth buffers.

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace wowee::rendering::depth {

/// The depth of nothing: what the scene's depth buffer is cleared to and what
/// the sky is drawn at.
inline constexpr float kFar = 0.0f;
/// The near plane's depth.
inline constexpr float kNear = 1.0f;

/// "Nearer than" and "nearer than or as near as", for the scene's depth tests.
inline constexpr VkCompareOp kNearer = VK_COMPARE_OP_GREATER;
inline constexpr VkCompareOp kNearerOrEqual = VK_COMPARE_OP_GREATER_OR_EQUAL;

/// A Vulkan ([0, 1]) projection with its depth reversed: z' = w - z, so the
/// near plane lands at 1 and the far plane at 0. Everything else - x, y, w -
/// is left as it was, so screen positions and view distances do not move.
inline glm::mat4 reversed(const glm::mat4& projection) {
    glm::mat4 r = projection;
    for (int c = 0; c < 4; ++c) r[c][2] = projection[c][3] - projection[c][2];
    return r;
}

/// The distance along the view axis of a reversed depth, for a projection
/// with near plane `nearZ` and far plane `farZ`.
inline float viewDistance(float d, float nearZ, float farZ) {
    return nearZ * farZ / (d * (farZ - nearZ) + nearZ);
}

/// A depth bias written for the standard mapping, in this one: a negative
/// bias pulled a surface toward the camera there, a positive one does here.
inline constexpr float biasFromStandard(float bias) { return -bias; }

}  // namespace wowee::rendering::depth

#pragma once

/// Positions the ray traced lighting takes back out of the scene depth
/// (rt_prepare.comp.glsl, rt_common.glsli).
///
/// The world's coordinates run to some ten thousand yards, where a float
/// steps a thousandth of a yard - as far apart as two neighbouring pixels on
/// a post a yard from the eye. A position unprojected with the inverse
/// view-projection carries that rounding, and a normal made from the
/// difference of two of them points anywhere: the occlusion rays then went
/// into the surface they left, and close surfaces were peppered with black
/// texels. Unprojected relative to the camera instead, the same difference
/// is exact to a few millionths of a yard.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::rendering::rt_reconstruct {

/// NDC -> world position less `cameraPos`, for the view-projection
/// `viewProj` (which includes the camera's translation). Worked out in
/// double, so the camera's distance from the world's origin cancels before
/// anything is rounded to float.
inline glm::mat4 cameraRelativeInverse(const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    const glm::dmat4 rel = glm::dmat4(viewProj) * glm::translate(glm::dmat4(1.0), glm::dvec3(cameraPos));
    return glm::mat4(glm::inverse(rel));
}

/// What the shaders do with it: the camera-relative position at uv (0-1)
/// and depth.
inline glm::vec3 relativeFromDepth(const glm::mat4& invRel, const glm::vec2& uv, float depth) {
    const glm::vec4 w = invRel * glm::vec4(uv * 2.0f - 1.0f, depth, 1.0f);
    return glm::vec3(w) / w.w;
}

/// The reversed depth a view direction is taken at (rt_common.glsli's
/// RT_DIRECTION_DEPTH): a hundred yards out at the client's near clip of
/// 0.2. At 0.5 - under half a yard from the eye - the inverse's rounding was
/// two thousandths of a radian of it, and different for every place the
/// camera stood.
inline constexpr float kDirectionDepth = 0.002f;

/// The unit direction from the camera through uv, as the tracer takes it
/// (rtViewDir) to place a surface at its stored distance (rtGbufWorld).
inline glm::vec3 viewDirection(const glm::mat4& invRel, const glm::vec2& uv,
                               float depth = kDirectionDepth) {
    return glm::normalize(relativeFromDepth(invRel, uv, depth));
}

}  // namespace wowee::rendering::rt_reconstruct

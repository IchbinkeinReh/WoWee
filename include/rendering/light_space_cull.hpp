#pragma once

/// Whether a world box can put anything into a shadow cascade's tile.
///
/// A cascade is an orthographic view: its tile is clip-space x and y in
/// [-1, 1], and the viewport and scissor drop everything outside it. A caster
/// whose box lies wholly to one side of that square cannot write a texel of
/// the tile, however near it is to the cascade's centre. The building shadow
/// pass culled by distance from that centre alone, with a 180-yard floor, so
/// the near cascade - a few dozen yards across - drew every building group
/// within 180 yards, all of them off its tile.
///
/// Depth is not tested: a box in front of the near plane or past the far one
/// is clipped by the rasteriser anyway, and leaving it to that is the
/// conservative side.

#include <glm/glm.hpp>

namespace wowee::rendering {

inline bool boxInLightTile(const glm::mat4& lightSpace, const glm::vec3& boxMin, const glm::vec3& boxMax) {
    glm::vec2 lo(1e30f);
    glm::vec2 hi(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? boxMax.x : boxMin.x,
                               (i & 2) ? boxMax.y : boxMin.y,
                               (i & 4) ? boxMax.z : boxMin.z);
        const glm::vec4 clip = lightSpace * glm::vec4(corner, 1.0f);
        // Orthographic, so w is 1; divided anyway so a projective matrix
        // still gets a sensible answer for what is in front of it.
        const float w = (clip.w > 1e-6f) ? clip.w : 1.0f;
        const glm::vec2 p(clip.x / w, clip.y / w);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    return !(hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f);
}

}  // namespace wowee::rendering

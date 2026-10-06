#pragma once

/// Where the sun is on screen.
///
/// The sun's direction itself is the client's sun curve now
/// (daynight::sunDirection, 0x007eecc0), not the reverse of the light: the two
/// share a side of the sky but not a height. It used to be derived from the
/// light and, before that, mirrored a sun below the horizon back up into the
/// sky, inventing one for the lens flare to draw around at night.

#include <glm/glm.hpp>

namespace wowee::rendering {

/// Where the sun falls on the screen.
struct SunOnScreen {
    /// False when the sun is behind the eye, where it has no place on screen.
    bool inFront = false;
    /// Framebuffer uv, (0,0) at the top left - the way every full-screen pass
    /// samples. Not clamped: a sun just past the edge still streams rays in.
    glm::vec2 uv{0.0f};
};

/// The sun's place on screen, for the view and projection the frame was drawn
/// with. A direction, so it is projected with w = 0: the sun is infinitely far,
/// and moving the camera does not move it.
inline SunOnScreen sunScreenPosition(const glm::mat4& view, const glm::mat4& projection,
                                     const glm::vec3& sunDir) {
    const glm::vec4 clip = projection * view * glm::vec4(sunDir, 0.0f);
    if (clip.w <= 1e-4f) return {};
    return {.inFront = true, .uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f};
}

}  // namespace wowee::rendering

#pragma once

/// The circle on the ground while a spell waits for a place (0x004f8a40,
/// placed by 0x004f66c0): Spell-Shadow-Acceptable or -Unacceptable laid on the
/// ground's own triangles through the blob shadow's projector (0x007e4370),
/// blend mode 2 (alpha) in white.
///
/// Everything here is arithmetic; BlobShadowRenderer gathers and draws.

#include "game/ground_target.hpp"
#include "rendering/blob_shadow.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace wowee::rendering::spell_target_circle {

/// 0x004f8a40 through 0x007e2d60: the box is the place +-r across the ground
/// and +-2 up and down; the texture spans the box turned a quarter back
/// (-pi/2) and no further, since no turn is passed (its third argument is
/// 0); the height fade runs over the box's four yards. Positions are the
/// renderer's, which are the client's world.
inline std::optional<blob_shadow::Projection> project(const glm::vec3& place, float r) {
    if (!(r > 0.0f)) return std::nullopt;
    constexpr float h = game::ground_target::kCircleHalfHeight;
    blob_shadow::Projection p;
    p.boxMin = place - glm::vec3(r, r, h);
    p.boxMax = place + glm::vec3(r, r, h);
    const float k = 1.0f / (2.0f * r);
    // (x, y) about the centre over the box, turned to (y, -x).
    p.uRow = glm::vec4(0.0f, k, 0.0f, 0.5f - k * place.y);
    p.vRow = glm::vec4(-k, 0.0f, 0.0f, 0.5f + k * place.x);
    const float kh = 1.0f / (2.0f * h);
    p.hRow = glm::vec4(0.0f, 0.0f, kh, 0.5f - place.z * kh);
    return p;
}

/// One fragment before blend mode 2 lays it over the ground by its alpha:
/// the texture in white, its alpha by the "ShadowAdd" height fade
/// (0x007e2c60 on the second stage, as the target's circle has it).
inline glm::vec4 shade(const glm::vec4& texel, float heightCoord) {
    const float fade = 1.0f - blob_shadow::modAt(heightCoord);
    return glm::vec4(glm::vec3(texel), texel.a * fade);
}

}  // namespace wowee::rendering::spell_target_circle

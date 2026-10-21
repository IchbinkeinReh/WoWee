#pragma once

// TargetDirectionEnemy / TargetDirectionFriend (0x00525c30, 0x00525cd0): the
// unit to target in a direction on screen, as 0x005252d0 and its per-unit
// scorer 0x00518f40 choose it.
//
// Everything is in world coordinates (WoWee's render space is the client's
// world space) and in the client's screen space: 0x004f6d20 projects a point
// through the camera to (x, y, z) with x and y the 0..1 frame position scaled
// by the screen's diagonal (0x0047bff0: x by a/sqrt(a*a+1), y by
// 1/sqrt(a*a+1)), y up, and z the clip-space depth before the divide.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace wowee::game::target_direction {

/// The camera 0x004f6d20 projects through.
struct View {
    glm::vec3 position{0.0f};   ///< camera +0x08
    glm::vec3 forward{1.0f, 0.0f, 0.0f};  ///< 0x004f6680: the camera's look direction
    /// World to clip, x right and y down (Vulkan) - the y is turned up here.
    glm::mat4 viewProjection{1.0f};
    float nearPlane = 0.2f;     ///< camera +0x38
    float farPlane = 1000.0f;
    float aspect = 4.0f / 3.0f;
};

struct ScreenPoint {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

/// 0x004f6d20: false behind the near plane or off the frame.
inline bool project(const View& view, const glm::vec3& world, ScreenPoint& out) {
    const glm::vec4 clip = view.viewProjection * glm::vec4(world, 1.0f);
    const float depth = glm::dot(world - view.position, view.forward);
    // The client's projection is Direct3D's: z before the divide is
    // far * (depth - near) / (far - near).
    const float z = view.farPlane * (depth - view.nearPlane) / (view.farPlane - view.nearPlane);
    if (z < view.nearPlane || clip.w <= 0.0f) return false;
    const float u = (clip.x / clip.w + 1.0f) * 0.5f;
    const float v = (-clip.y / clip.w + 1.0f) * 0.5f;
    const float diag = std::sqrt(view.aspect * view.aspect + 1.0f);
    const float sx = u * (view.aspect / diag);
    const float sy = v * (1.0f / diag);
    out = {sx, sy, z};
    return sx >= 0.0f && sx <= view.aspect / diag && sy > 0.0f && sy <= 1.0f / diag;
}

/// A unit the scan may take, already passed through the reaction test
/// (0x00518e40) and the creature-type flag (CreatureType.dbc flags 1).
struct Candidate {
    uint64_t guid = 0;
    glm::vec3 position{0.0f};
};

/// 0x005252d0 / 0x00518f40. `facing` is the direction asked for on screen
/// (0 up, growing clockwise), `cone` the width of the cone in radians (never
/// under pi/8). `origin` is where the scan starts - the target, or the player
/// when the target is off screen - and `player` whom the 41 yards are counted
/// from; neither is ever chosen. 0 when nothing qualifies.
/// 0x0048b820: a plane vector made unit, unless it is next to nothing.
inline glm::vec2 normalizedFlat(glm::vec2 v) {
    const float len2 = v.x * v.x + v.y * v.y;
    if (2.3841858e-07f < len2) v *= 1.0f / std::sqrt(len2);
    return v;
}

inline uint64_t choose(const View& view, float facing, float cone, uint64_t originGuid,
                       const glm::vec3& origin, uint64_t playerGuid, const glm::vec3& player,
                       const std::vector<Candidate>& candidates) {
    ScreenPoint originScreen;
    project(view, origin, originScreen);
    // The camera's look turned by -facing about the vertical, in the plane.
    const float c = std::cos(6.2831855f - facing);
    const float s = std::sin(6.2831855f - facing);
    const glm::vec2 look(c * view.forward.x - s * view.forward.y,
                         s * view.forward.x + c * view.forward.y);
    const glm::vec2 screenDir(std::sin(facing), std::cos(facing));
    const float coneCos = std::cos(std::max(cone, 0.3926991f) * 0.5f);

    uint64_t best = 0;
    float bestScore = 3.4028235e+38f;
    bool bestInCone = false;
    for (const auto& cand : candidates) {
        if (cand.guid == playerGuid || cand.guid == originGuid) continue;
        const glm::vec3 fromPlayer = cand.position - player;
        if (glm::dot(fromPlayer, fromPlayer) > 1681.0f) continue;
        ScreenPoint p;
        if (!project(view, cand.position, p)) continue;
        const glm::vec2 ground =
            normalizedFlat({cand.position.x - origin.x, cand.position.y - origin.y});
        const float along = look.x * ground.x + look.y * ground.y;
        const glm::vec3 onScreen(p.x - originScreen.x, p.y - originScreen.y, p.z - originScreen.z);
        const glm::vec2 flat = normalizedFlat({onScreen.x, onScreen.y});
        if (screenDir.x * flat.x + screenDir.y * flat.y < 0.0f) continue;
        bool inCone = true;
        if (coneCos <= along) {
            if (!bestInCone) bestScore = 3.4028235e+38f;
        } else {
            if (bestInCone) continue;
            inCone = false;
        }
        float score = glm::dot(onScreen, onScreen);
        if (!inCone) score *= (1.0f - along) * (1.0f - along);
        if (score <= bestScore) {
            bestScore = score;
            best = cand.guid;
            bestInCone = inCone;
        }
    }
    return best;
}

}  // namespace wowee::game::target_direction

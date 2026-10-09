#pragma once

/// The circle under the target: Textures\UnitSelectTexture.blp laid on the
/// ground's own triangles the way the blob shadow is (0x007e4370), added on
/// in the unit's selection colour (0x00725980, 0x00744eb0).
///
/// Everything here is arithmetic; BlobShadowRenderer gathers the triangles
/// and draws them.

#include "rendering/blob_shadow.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace wowee::rendering::selection_circle {

/// 0x00720330: the circle's radius (the unit's +0xb0c). Half the ground
/// diagonal of the CreatureModelData box (mounted, joined to the mount's,
/// 0x0071ed80) times the unit's scale, and the square root of that; past 5
/// it grows by 0.06 of the square of the excess, and it is held to 10. A box
/// with no width either way gives 1.2.
inline float radius(const blob_shadow::Box& box, float scale) {
    constexpr float kEps = 2.3841858e-07f;
    const float dx = box.max.x - box.min.x;
    const float dy = box.max.y - box.min.y;
    if (std::abs(dx) < kEps && std::abs(dy) < kEps) return 1.2f;
    const float s = scale * (std::sqrt(dx * dx + dy * dy) * 0.5f);
    float r = std::sqrt(s);
    if (s > 5.0f) r += (s - 5.0f) * (s - 5.0f) * 0.06f;
    return r > 10.0f ? 10.0f : r;
}

/// What 0x00521bf0 reads to colour the circle (the unit's virtual +0x78,
/// 0x00718ac0, asks it with its last argument 1).
struct ColorInput {
    /// UNIT_FIELD_FLAGS 0x8: a player, or a unit a player controls.
    bool playerControlled = false;
    /// Health below 1, or UNIT_DYNFLAG_DEAD.
    bool dead = false;
    /// How the target regards the player, 1 (hated) to 8 (exalted), as the
    /// interface numbers them (0x007251c0 answers 0 to 7).
    int reaction = 4;
    /// 0x00729740 both ways.
    bool targetMayAttackPlayer = false;
    bool playerMayAttackTarget = false;
    /// The target's PvP flag (UNIT_FIELD_BYTES_2 byte 1, bit 1), neither it
    /// nor the player in a sanctuary (8), and not in an arena.
    bool pvp = false;
    /// 0x0052d310 while in a group: the player's own pet, or a member.
    bool groupMember = false;
    /// On the friends list (0x006b3510).
    bool friendListed = false;
};

/// The colour 0x00521bf0 hands back, as 0..1 RGBA (its table is D3DCOLOR).
inline glm::vec4 color(const ColorInput& in) {
    auto rgb = [](int r, int g, int b) {
        return glm::vec4(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), 255.0f) / 255.0f;
    };
    // 0x00bd0c94: hated and hostile red, unfriendly orange, neutral yellow,
    // friendly and above green.
    auto byReaction = [&](int reaction) {
        const int i = std::clamp(reaction - 1, 0, 7);
        if (i <= 1) return rgb(255, 0, 0);
        if (i == 2) return rgb(255, 128, 0);
        if (i == 3) return rgb(255, 255, 0);
        return rgb(0, 255, 0);
    };
    if (!in.playerControlled) {
        if (in.dead) return rgb(127, 127, 127);  // 0x00acc3f8
        return byReaction(in.reaction);
    }
    if (in.targetMayAttackPlayer) {
        return in.playerMayAttackTarget ? rgb(255, 0, 0)    // 0x00bd0c98
                                        : rgb(96, 96, 255);  // 0x00bd0c90
    }
    if (in.playerMayAttackTarget) return rgb(255, 255, 0);  // 0x00bd0ca0
    if (in.groupMember) {
        return in.pvp ? rgb(170, 255, 170)    // 0x00bd0c84
                      : rgb(170, 170, 255);   // 0x00bd0c88
    }
    if (in.friendListed) return rgb(83, 201, 255);  // 0x00bd0c80
    if (in.pvp) return byReaction(in.reaction);
    return rgb(96, 96, 255);  // 0x00bd0c8c[1]
}

/// The angle from the camera to the unit across the ground (0x004f5130).
inline float facingAngle(const glm::vec3& camera, const glm::vec3& unit) {
    constexpr float kEps = 2.3841858e-07f;
    constexpr float kPi = 3.1415927f;
    const float dx = unit.x - camera.x;
    const float dy = unit.y - camera.y;
    if (std::abs(dx) < kEps) return dy < 0.0f ? 1.5f * kPi : 0.5f * kPi;
    if (std::abs(dy) >= kEps) return std::atan2(dy, dx);
    return unit.x < camera.x ? kPi : 0.0f;
}

/// 0x00725980 through 0x007e2d60: the box the ground is gathered from - the
/// unit's position and `r` either way on every axis - and the texture's
/// coordinates over it. The texture spans the box, turned a quarter back
/// (-pi/2) and then turned with the camera's bearing on the unit (0x00744150)
/// so that its top lies away from the camera; the height fade runs over the
/// box's height. Positions are the renderer's, which are the client's world.
inline std::optional<blob_shadow::Projection> project(const glm::vec3& unit, float r,
                                                      const glm::vec3& camera) {
    if (!(r > 0.0f)) return std::nullopt;
    blob_shadow::Projection p;
    p.boxMin = unit - glm::vec3(r);
    p.boxMax = unit + glm::vec3(r);
    const float a = facingAngle(camera, unit);
    const float c = std::cos(a);
    const float s = std::sin(a);
    const float k = 1.0f / (2.0f * r);
    // (x, y) about the centre over the box, turned to (y, -x), then by -a.
    p.uRow = glm::vec4(-k * s, k * c, 0.0f, 0.5f + k * (s * unit.x - c * unit.y));
    p.vRow = glm::vec4(-k * c, -k * s, 0.0f, 0.5f + k * (c * unit.x + s * unit.y));
    p.hRow = glm::vec4(0.0f, 0.0f, k, 0.5f - unit.z * k);
    return p;
}

/// The alpha ref the client sets for blend mode 3 (0x00ad8b7c[3]): texels
/// with no alpha are not drawn.
constexpr float kAlphaRef = 1.0f / 255.0f;

/// One fragment's colour and alpha before it is added on (blend mode 3,
/// source alpha and one): every stage modulates (states 0x25, 0x2d, 0x26,
/// 0x2e all 0, 0x00744eb0), so the texture by the circle's colour, the alpha
/// also by the "ShadowAdd" height fade (0x007e36e0) read at `heightCoord`.
inline glm::vec4 shade(const glm::vec4& texel, const glm::vec4& circleColor, float heightCoord) {
    const float fade = 1.0f - blob_shadow::modAt(heightCoord);
    return glm::vec4(glm::vec3(texel) * glm::vec3(circleColor), texel.a * circleColor.a * fade);
}

}  // namespace wowee::rendering::selection_circle

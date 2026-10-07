#pragma once

/// The client's blob shadow under a unit: Textures\ShadowBlob.blp projected
/// down onto the ground's triangles, at the default settings in place of a
/// shadow map (0x007e49e0, 0x007e4480, 0x007e4370, 0x007e2d60, 0x007e3e80).
///
/// Everything here is arithmetic; BlobShadowRenderer gathers the triangles
/// and draws them.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace wowee::rendering::blob_shadow {

/// A box in a model's own space.
struct Box {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// One unit's blob to draw: its model's world matrix (column vectors, scale
/// included), the box it is sized by in that model's space, and the model's
/// alpha (0x007e4480 reads the model's +0x178).
struct Caster {
    glm::mat4 world{1.0f};
    Box box;
    float alpha = 1.0f;
};

/// 0x0070bd20: a box is drawn only when it is open on every axis.
inline bool isEmpty(const Box& b) {
    return !(b.min.x < b.max.x && b.min.y < b.max.y && b.min.z < b.max.z);
}

/// 0x0071ed80: a mounted unit's box. The rider's CreatureModelData box is
/// raised by the mount's MountHeight (+0x40) less half the rider's height,
/// and joined to the mount's own box (+0x44..+0x58).
inline Box mountedBox(Box rider, const Box& mount, float mountHeight) {
    const float lift = mountHeight - (rider.max.z - rider.min.z) * 0.5f;
    rider.min.z += lift;
    rider.max.z += lift;
    return {glm::min(rider.min, mount.min), glm::max(rider.max, mount.max)};
}

/// The default of the client's shadowLOD cvar (0x007e4a40) and the only
/// other value its handler accepts (0x007e3a20).
constexpr int kDefaultShadowLOD = 1;

/// 0x007e49e0: drawn with shadowLOD 1, and only while extShadowQuality is
/// below 1 - the dynamic shadows take its place.
inline bool drawn(int shadowLOD, int extShadowQuality) {
    return shadowLOD == 1 && extShadowQuality < 1;
}

/// The bias the client hands the depth for the projected triangles:
/// 0x007e49e0 passes 0.4 down to 0x007e3e80, which offsets the depth by
/// 2 x 0.4 / 32768 (0x00763c70).
constexpr float kDepthOffset = 0.4f * 2.0f * 3.0518044e-05f;

/// How the blob lies over the world, for one unit.
struct Projection {
    glm::vec3 boxMin{0.0f};  ///< the world box the ground is gathered from
    glm::vec3 boxMax{0.0f};
    /// The blob's texture coordinates: u = dot(uRow, (p, 1)), v likewise.
    glm::vec4 uRow{0.0f};
    glm::vec4 vRow{0.0f};
    /// The height-fade texture's coordinate, 0 at the box's floor and 1 at
    /// its top (0x007e2d60's second matrix).
    glm::vec4 hRow{0.0f};
};

/// 0x007e4480 with 0x007e2d60. `local` is the unit's box in its model's
/// space, `world` the model's world matrix (column vectors, scale included).
///
/// The box is centred on the model's origin across the ground, sized by the
/// scale and held to 5 yards from it on every axis. The ground is gathered
/// from that footprint turned with the model, from 1.67 of its half-height
/// below the origin to one above. The blob's texture runs across the
/// footprint: u along the model's +Y, v against its +X.
inline std::optional<Projection> project(const Box& local, const glm::mat4& world) {
    constexpr float kEps = 2.3841858e-07f;
    const float sx = (local.max.x - local.min.x) * 0.5f;
    const float sy = (local.max.y - local.min.y) * 0.5f;
    if (std::abs(sx) < kEps || std::abs(sy) < kEps) return std::nullopt;
    if (std::abs(local.max.z - local.min.z) < kEps) return std::nullopt;

    const float scale = glm::length(glm::vec3(world[0]));
    if (!(scale > 0.0f)) return std::nullopt;
    glm::vec3 bMin(-sx, -sy, local.min.z);
    glm::vec3 bMax(sx, sy, local.max.z);
    bMin = glm::clamp(bMin * scale, glm::vec3(-5.0f), glm::vec3(5.0f));
    bMax = glm::clamp(bMax * scale, glm::vec3(-5.0f), glm::vec3(5.0f));
    const float halfHeight = (bMax.z - bMin.z) * 0.5f;

    // The rotation alone (0x004c51b0, then 1/scale).
    const glm::mat3 rot = glm::mat3(world) / scale;
    const glm::vec3 origin(world[3]);

    Projection p;
    glm::vec2 lo(1e30f), hi(-1e30f);
    const glm::vec2 corners[4] = {{bMax.x, bMax.y}, {bMax.x, bMin.y}, {bMin.x, bMin.y}, {bMin.x, bMax.y}};
    for (const auto& c : corners) {
        const glm::vec3 w = rot * glm::vec3(c, 0.0f);
        lo = glm::min(lo, glm::vec2(w));
        hi = glm::max(hi, glm::vec2(w));
    }
    p.boxMin = glm::vec3(glm::vec2(origin) + lo, origin.z - 1.6666666f * halfHeight);
    p.boxMax = glm::vec3(glm::vec2(origin) + hi, origin.z + 1.0f * halfHeight);

    const glm::vec3 centre = (p.boxMin + p.boxMax) * 0.5f;
    const float spanX = std::abs(bMax.x - bMin.x);
    const float spanY = std::abs(bMax.y - bMin.y);
    if (spanX < kEps || spanY < kEps) return std::nullopt;
    // Translate by the centre, turn a quarter back about z (-pi/2), undo the
    // world extents, turn into the model's frame and divide by its own
    // extents; then +0.5 (0x007e2d60 with 0x007e4480's matrix).
    const glm::vec3 uAxis = rot[1] / spanY;
    const glm::vec3 vAxis = -rot[0] / spanX;
    p.uRow = glm::vec4(uAxis, 0.5f - glm::dot(uAxis, centre));
    p.vRow = glm::vec4(vAxis, 0.5f - glm::dot(vAxis, centre));
    const float height = p.boxMax.z - p.boxMin.z;
    if (!(height > kEps)) return std::nullopt;
    p.hRow = glm::vec4(0.0f, 0.0f, 1.0f / height, 0.5f - centre.z / height);
    return p;
}

/// The height-fade texture's width (0x007e4a40: 64 by 8).
constexpr int kFadeTexels = 64;

/// One texel of "ShadowAdd"'s alpha (0x007e36e0): in over the first sixth,
/// out over the last.
inline float fadeTexel(int i) {
    const float s = (static_cast<float>(i) / static_cast<float>(kFadeTexels - 1)) * 12.0f;
    float f;
    if (s < 2.0f) f = s * 0.5f;
    else if (s < 10.0f) f = 1.0f;
    else f = std::max((12.0f - s) * 0.5f, 0.0f);
    return std::round(f * 255.0f) / 255.0f;
}

/// One texel of "ShadowMod" (0x007e3820), the one a Mod-blended blob uses:
/// grey 1 - fade, so white where the fade is gone.
inline float modTexel(int i) {
    const float s = (static_cast<float>(i) / static_cast<float>(kFadeTexels - 1)) * 12.0f;
    float f;
    if (s < 2.0f) f = s * 0.5f;
    else if (s < 10.0f) f = 1.0f;
    else f = std::max((12.0f - s) * 0.5f, 0.0f);
    return std::round((1.0f - f) * 255.0f) / 255.0f;
}

/// "ShadowMod" read at `u` as a linearly filtered, edge-clamped texture.
/// blob_shadow.frag.glsl has the same.
inline float modAt(float u) {
    const float x = u * static_cast<float>(kFadeTexels) - 0.5f;
    const float i0 = std::floor(x);
    const float t = x - i0;
    const int a = std::clamp(static_cast<int>(i0), 0, kFadeTexels - 1);
    const int b = std::clamp(static_cast<int>(i0) + 1, 0, kFadeTexels - 1);
    return modTexel(a) + (modTexel(b) - modTexel(a)) * t;
}

/// What a fragment multiplies the ground by. 0x007e4480 binds the blob as
/// texture 0 with colour op Fade (5) - the texture over the vertex colour by
/// the vertex alpha, which is the model's alpha over white - and ShadowMod as
/// texture 1 with op Add (2); the blend is Mod (state 6 = 4).
inline glm::vec3 modulation(const glm::vec3& blob, float modelAlpha, float heightCoord) {
    const glm::vec3 faded = glm::mix(glm::vec3(1.0f), blob, std::clamp(modelAlpha, 0.0f, 1.0f));
    return glm::clamp(faded + glm::vec3(modAt(heightCoord)), glm::vec3(0.0f), glm::vec3(1.0f));
}

/// 0x007e32f0: a gathered triangle is kept when it faces up, its winding
/// counter-clockwise seen from above in the client's world, which is the
/// renderer's. `world` is the placement of the model the corners are local
/// to.
inline bool facesUp(const glm::mat4& world, const glm::vec3& v0, const glm::vec3& v1,
                    const glm::vec3& v2) {
    const glm::vec3 n = glm::mat3(world) * glm::cross(v1 - v0, v2 - v0);
    return n.z >= 0.0f;
}

}  // namespace wowee::rendering::blob_shadow

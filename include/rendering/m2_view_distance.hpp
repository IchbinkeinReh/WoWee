#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

/// The client's doodad distances. Every doodad, of the terrain or of a WMO,
/// falls in one of five size classes by the largest side of its world box
/// (0x007bdb10 against 0x00adf350), and each class has a distance it is drawn
/// to and a band it fades out over before it (0x00adf364, 0x00adf38c; the
/// per-doodad test is 0x00791cb0). Environment Detail scales the middle three
/// distances (0x0078f570), from 0.5 to 1.5 (0x0078dc60).
inline constexpr float kM2DoodadSizeLimit[4] = {1.0f, 4.0f, 15.0f, 100.0f};
inline constexpr float kM2DoodadCullDistance[5] = {30.0f, 100.0f, 200.0f, 750.0f, 1250.0f};
inline constexpr float kM2DoodadFadeLength[5] = {5.0f, 10.0f, 15.0f, 20.0f, 50.0f};

/// The size class of a doodad whose world box's largest side is `maxExtent`:
/// the first limit it does not exceed, 4 past them all (0x007bdb10).
inline uint8_t m2DoodadSizeClass(float maxExtent) {
    uint8_t c = 0;
    while (c < 4 && maxExtent > kM2DoodadSizeLimit[c]) ++c;
    return c;
}

/// The size class of a model whose header vertex box (+0xa0) is `boxMin` to
/// `boxMax`, placed by `model` (0x007bdb10): the box carried into the world,
/// its largest side. A box inverted on every axis stands for none, and the
/// client puts the doodad's box on its position instead - class 0.
inline uint8_t m2DoodadSizeClassOfBox(const glm::mat4& model, const glm::vec3& boxMin,
                                      const glm::vec3& boxMax) {
    if (!(boxMin.x <= boxMax.x || boxMin.y <= boxMax.y || boxMin.z <= boxMax.z)) return 0;
    glm::vec3 lo(INFINITY), hi(-INFINITY);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? boxMax.x : boxMin.x, (i & 2) ? boxMax.y : boxMin.y,
                               (i & 4) ? boxMax.z : boxMin.z);
        const glm::vec3 world(model * glm::vec4(corner, 1.0f));
        lo = glm::min(lo, world);
        hi = glm::max(hi, world);
    }
    const glm::vec3 ext = hi - lo;
    return m2DoodadSizeClass(std::max({ext.x, ext.y, ext.z}));
}

/// How far a doodad of a class is drawn (0x0078f570).
inline float m2DoodadCullDistance(uint8_t sizeClass, float environmentDetail) {
    const uint8_t c = std::min<uint8_t>(sizeClass, 4);
    const float detail = std::clamp(environmentDetail, 0.5f, 1.5f);
    return (c >= 1 && c <= 3) ? kM2DoodadCullDistance[c] * detail : kM2DoodadCullDistance[c];
}

/// A doodad's alpha at `distance` from the camera: 1 short of its class's
/// fade band, falling to 0 over it, and 0 - not drawn - once below 0.01;
/// above 0.99 it is drawn whole (0x00791cb0).
inline float m2DoodadFade(float distance, uint8_t sizeClass, float environmentDetail) {
    const uint8_t c = std::min<uint8_t>(sizeClass, 4);
    const float cull = m2DoodadCullDistance(c, environmentDetail);
    if (distance > cull) return 0.0f;
    const float fadeLen = kM2DoodadFadeLength[c];
    const float fadeStart = cull - fadeLen;
    if (distance <= fadeStart) return 1.0f;
    const float a = 1.0f - (distance - fadeStart) / fadeLen;
    if (a > 0.99f) return 1.0f;
    return a < 0.01f ? 0.0f : a;
}

/// MDDF flag 0x1 makes a terrain doodad one the distance test passes over:
/// the client gives it flag 0x800 (0x007becd0), and 0x00791cb0 then neither
/// culls nor fades it by its distance.
inline constexpr uint16_t kMddfNoDistanceCull = 0x1;

/// The smallest size class a terrain chunk draws doodads of, by its depth
/// along the view (0x0078fb60): the first class whose distance the depth is
/// short of, 4 past the first four; 0 behind the camera. The client walks a
/// chunk's doodads with it before any doodad's own test (0x00799d40,
/// 0x00799980), so for every other doodad it removes nothing that test would
/// keep - but it is all that bounds one with flag 0x800.
inline uint8_t m2ChunkMinSizeClass(float depth, float environmentDetail) {
    if (depth < 0.0f) return 0;
    const float depthSq = depth * depth;
    for (uint8_t c = 0; c < 4; ++c) {
        const float cull = m2DoodadCullDistance(c, environmentDetail);
        if (depthSq < cull * cull) return c;
    }
    return 4;
}

/// A chunk's depth for that test (0x007c3e70): along the view direction, from
/// the camera to the corner of the chunk's box nearest along it - the box's
/// low side on an axis the view looks up, its high side otherwise
/// (0x00790650).
inline float m2ChunkViewDepth(const glm::vec3& boxMin, const glm::vec3& boxMax,
                              const glm::vec3& cameraPos, const glm::vec3& viewDir) {
    const glm::vec3 corner(viewDir.x >= 0.0f ? boxMin.x : boxMax.x,
                           viewDir.y >= 0.0f ? boxMin.y : boxMax.y,
                           viewDir.z >= 0.0f ? boxMin.z : boxMax.z);
    return glm::dot(viewDir, corner - cameraPos);
}

/// The terrain chunks a doodad is listed in are the ones its box overlaps.
/// The box over them all: the doodad's world box widened in x and y to the
/// 33.3-yard chunk grid. Its height is the doodad's own; the chunks' heights
/// are the terrain's, which the doodad renderer does not hold.
inline void m2ChunkBoxAround(const glm::vec3& worldMin, const glm::vec3& worldMax,
                             glm::vec3& boxMin, glm::vec3& boxMax) {
    constexpr float kChunk = 533.33333f / 16.0f;
    boxMin = glm::vec3(std::floor(worldMin.x / kChunk) * kChunk,
                       std::floor(worldMin.y / kChunk) * kChunk, worldMin.z);
    boxMax = glm::vec3((std::floor(worldMax.x / kChunk) + 1.0f) * kChunk,
                       (std::floor(worldMax.y / kChunk) + 1.0f) * kChunk, worldMax.z);
}

/// The distance at which one M2 instance stops drawing, squared.
///
/// A doodad stops where its size class says. A server game object is not a
/// doodad and is not held by that rule: it is drawn as far as the world is.
/// The ground cover is its own setting (groundEffectDist, at most 140 yards in
/// the client, 0x0078db10). Nothing is drawn past the view distance, where
/// the terrain and the WMOs stop: a doodad there is a tree standing on nothing.
inline float m2InstanceMaxDistSq(uint8_t sizeClass,
                                 float environmentDetail,
                                 bool isGameObject,
                                 float viewDistanceAbsolute,
                                 bool isGroundDetail = false,
                                 float groundDetailMaxDistance = 0.0f,
                                 bool noDistanceCull = false) {
    float maxDist = (isGameObject || noDistanceCull)
                        ? viewDistanceAbsolute
                        : m2DoodadCullDistance(sizeClass, environmentDetail);
    if (isGroundDetail && groundDetailMaxDistance > 0.0f) maxDist = groundDetailMaxDistance;
    maxDist = std::min(maxDist, viewDistanceAbsolute);
    return maxDist * maxDist;
}

} // namespace rendering
} // namespace wowee

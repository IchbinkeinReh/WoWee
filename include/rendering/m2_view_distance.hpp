#pragma once

#include <algorithm>
#include <cstdint>

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
                                 float groundDetailMaxDistance = 0.0f) {
    float maxDist = isGameObject ? viewDistanceAbsolute
                                 : m2DoodadCullDistance(sizeClass, environmentDetail);
    if (isGroundDetail && groundDetailMaxDistance > 0.0f) maxDist = groundDetailMaxDistance;
    maxDist = std::min(maxDist, viewDistanceAbsolute);
    return maxDist * maxDist;
}

} // namespace rendering
} // namespace wowee

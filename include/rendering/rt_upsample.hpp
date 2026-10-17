#pragma once

/// How a pixel finds its own surface among the half-resolution traced texels
/// and how a texel keeps its history at the edges of what is drawn: the pure
/// functions of assets/shaders/rt_upsample.glsli, mirrored for the unit tests.
/// See that file and docs/deferred_rt_design.md.

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/glm.hpp>

namespace wowee::rendering::rt_upsample {

inline float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// rtTakesFarthest: the farthest surface on one colour of a checkerboard fixed
/// to the screen, the nearest on the other.
inline bool takesFarthest(glm::ivec2 texel) { return ((texel.x + texel.y) & 1) != 0; }

/// rtBlockPrefers: whether a sample (reversed depth di, larger nearer; vi a
/// surface of the tracing's) replaces the one kept so far.
inline bool blockPrefers(bool farthest, bool found, float d, bool valid, float di, bool vi) {
    if (!found) return true;
    if (vi != valid) return vi;
    return farthest ? di < d : di > d;
}

/// rt_prepare's choice of a 2x2 block's sample (x fastest): the index, or -1
/// where every sample is the sky (depth 0).
inline int pickBlockSample(const std::array<float, 4>& depth, const std::array<bool, 4>& valid,
                           glm::ivec2 texel) {
    const bool farthest = takesFarthest(texel);
    int sub = -1;
    float d = 0.0f;
    bool v = false;
    for (int i = 0; i < 4; ++i) {
        if (depth[i] <= 0.0f) continue;
        if (blockPrefers(farthest, sub >= 0, d, v, depth[i], valid[i])) {
            d = depth[i];
            v = valid[i];
            sub = i;
        }
    }
    return sub;
}

/// rtDistanceMatch.
inline float distanceMatch(float texelDist, float expected) {
    if (texelDist <= 0.0f) return 0.0f;
    const float e = (texelDist - expected) / (0.03f * expected + 0.05f);
    return std::exp(-e * e);
}

/// rtPlaneMatch.
inline float planeMatch(glm::vec3 pixelRel, glm::vec3 texelRel, glm::vec3 texelNormal, float expected,
                        float texelDist) {
    if (texelDist <= 0.0f) return 0.0f;
    const float off = std::abs(glm::dot(pixelRel - texelRel, texelNormal));
    const float e = off / (0.01f * expected + 0.03f);
    const float along = std::abs(texelDist - expected) / std::max(expected, 1e-3f);
    return std::exp(-e * e) * (1.0f - smoothstep(0.15f, 0.3f, along));
}

/// rtNormalWeight.
inline float normalWeight(glm::vec3 np, glm::vec3 nt) {
    return 0.05f + 0.95f * smoothstep(0.5f, 0.85f, glm::dot(np, nt));
}

/// rtClampWindow, per channel.
inline float clampWindow(float sigma, float count) {
    return 1.5f * sigma + 0.2f + 0.6f * std::clamp((4.0f - count) / 3.0f, 0.0f, 1.0f);
}

/// rtHistoryWeight.
inline float historyWeight(glm::vec2 traced, glm::vec2 reprojected, float blockSize) {
    const float t = std::max(1.0f - glm::length(traced - reprojected) / blockSize, 0.0f);
    return t * t + 1e-3f;
}

}  // namespace wowee::rendering::rt_upsample

#pragma once
// How far the sun's shadow map moves a receiver's depth back before the
// compare, in yards (shadow_csm.glsli, csmSampleCascade, which works the same
// sum in the shader).
//
// The shaders gave the bias in depth units of the shared light-space range,
// 0.0005 x (1 - N.L): with the default 500-yard shadow distance that range is
// some 3250 yards, so under a low sun the bias was over a yard along the
// light - and a character's shadow began a yard and more away from its feet
// (U22, Durotar at 08:30). A depth bias has to cover what one texel of the
// cascade spans in depth on the receiver - its size in yards times the tangent
// of the angle between the surface and the light - and no more.
#include <algorithm>
#include <cmath>

namespace wowee::rendering::shadow_bias {

/// The steepest slope the bias follows: a surface the sun all but grazes
/// takes no more than this many texels' depth.
inline constexpr float kMaxTangent = 4.0f;

/// The bias in yards for a cascade whose texel is `texelYards` across, on a
/// surface whose normal has `nl` (cosine) with the direction to the sun:
/// one texel, plus one and a half texels' depth along the slope (the 3x3
/// filter reads a texel either side).
inline float depthBiasYards(float texelYards, float nl) {
    const float c = std::clamp(std::abs(nl), 0.05f, 1.0f);
    const float tangent = std::min(std::sqrt(std::max(1.0f - c * c, 0.0f)) / c, kMaxTangent);
    return texelYards * (1.0f + 1.5f * tangent);
}

/// What the shaders' old bias came to in yards, for `depthRangeYards` of
/// light-space depth (Renderer's shadowDepthRange) - for comparison only.
inline float legacyDepthBiasYards(float depthRangeYards, float nl) {
    return std::max(0.0005f * (1.0f - std::abs(nl)), 0.00005f) * depthRangeYards;
}

}  // namespace wowee::rendering::shadow_bias

#pragma once

/// The ray traced lighting's G-buffer as the shaders pack and apply it
/// (assets/shaders/rt_gbuffer.glsli, rt_apply.frag.glsl), for the unit tests
/// that hold the two to the forward shaders' own formulas.
///
/// An opaque surface draws its colour C0 with the traced terms neutral and
/// writes how the colour depends on them; the apply pass turns the traced
/// terms at the pixel into a factor and an addend, dst * mul + add. See
/// docs/deferred_rt_design.md.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/rt_gi_volume.hpp"

namespace wowee::rendering::rt_gbuffer {

inline constexpr uint32_t kValid = 1u;
inline constexpr uint32_t kSun = 2u;
inline constexpr uint32_t kBaked = 4u;
inline constexpr uint32_t kLiftAfter = 8u;
inline constexpr uint32_t kInterior = 16u;
inline constexpr uint32_t kUnitFloor = 32u;
inline constexpr uint32_t kTransition = 64u;

/// What a surface knows of how its colour depends on the traced terms, in the
/// colour's own units: C = C0 + g (r - 1) - r max(0, occY (1 - ao) - occE) + kp P.
struct Surface {
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float map = 1.0f;
    uint32_t flags = 0;
    glm::vec3 g{0.0f};
    glm::vec3 occY{0.0f};
    glm::vec3 occE{0.0f};
    glm::vec3 kp{0.0f};
    float outside = 1.0f;  ///< kTransition: the outside pass's share of g
    /// Otherwise: 1 for a building's baked colour, which its building's
    /// lights shade from their own storey only (rt_light_storey.hpp). It
    /// takes the outside share's bits.
    uint32_t storeyBound = 0;
};

/// The same read back from the targets, relative to C0.
struct Texel {
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float map = 1.0f;
    uint32_t flags = 0;
    glm::vec3 g{0.0f};
    glm::vec3 occ{0.0f};
    glm::vec3 knee{1.0f};
    glm::vec3 kp{0.0f};
    float outside = 1.0f;
    uint32_t storeyBound = 0;  ///< 0 for a transition batch
};

/// The eight 16-bit words of the two R16G16B16A16_UNORM targets.
using Words = std::array<uint16_t, 8>;

// The same octahedral mapping as the probe tiles (rt_gi_volume.hpp).
using gi::octDecode;
using gi::octEncode;

inline uint32_t quantise(float v, float steps) {
    return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * steps + 0.5f);
}

inline Words pack(const Surface& s, const glm::vec3& c0) {
    const glm::vec2 oct = octEncode(s.normal) * 0.5f + 0.5f;
    glm::vec3 g(0.0f), occ(0.0f), knee(1.0f);
    for (int c = 0; c < 3; ++c) {
        const float inv = c0[c] > 1e-5f ? 1.0f / c0[c] : 0.0f;
        g[c] = s.g[c] * inv;
        occ[c] = std::max(s.occY[c] - s.occE[c], 0.0f) * inv;
        knee[c] = s.occY[c] > 1e-6f ? s.occE[c] / s.occY[c] : 1.0f;
    }
    const glm::vec3 kp = s.kp * 0.5f;
    const auto byte = [](float v) { return quantise(v, 255.0f); };
    Words w{};
    const uint32_t f = (s.flags & kTransition) != 0 ? quantise(s.outside, 63.0f) : (s.storeyBound != 0 ? 1u : 0u);
    w[0] = static_cast<uint16_t>(quantise(oct.x, 1023.0f) | (quantise(knee.r, 31.0f) << 10) | ((f & 1u) << 15));
    w[1] = static_cast<uint16_t>(quantise(oct.y, 1023.0f) | (quantise(knee.g, 31.0f) << 10) |
                                 (((f >> 1) & 1u) << 15));
    w[2] = static_cast<uint16_t>(byte(s.map) | ((s.flags & 127u) << 8) | (((f >> 2) & 1u) << 15));
    w[3] = static_cast<uint16_t>(byte(g.r) | (byte(g.g) << 8));
    w[4] = static_cast<uint16_t>(byte(g.b) | (byte(occ.r) << 8));
    w[5] = static_cast<uint16_t>(byte(occ.g) | (byte(occ.b) << 8));
    w[6] = static_cast<uint16_t>(byte(kp.r) | (byte(kp.g) << 8));
    w[7] = static_cast<uint16_t>(byte(kp.b) | (quantise(knee.b, 31.0f) << 8) | ((f >> 3) << 13));
    return w;
}

inline Texel unpack(const Words& w) {
    const auto unbyte = [](uint32_t v, uint32_t shift) { return static_cast<float>((v >> shift) & 255u) / 255.0f; };
    Texel t;
    const glm::vec2 oct(static_cast<float>(w[0] & 1023u) / 1023.0f, static_cast<float>(w[1] & 1023u) / 1023.0f);
    t.normal = octDecode(oct * 2.0f - 1.0f);
    t.knee = glm::vec3(static_cast<float>((w[0] >> 10) & 31u), static_cast<float>((w[1] >> 10) & 31u),
                       static_cast<float>((w[7] >> 8) & 31u)) / 31.0f;
    t.map = unbyte(w[2], 0);
    t.flags = (w[2] >> 8) & 127u;
    const uint32_t share = ((w[0] >> 15) & 1u) | (((w[1] >> 15) & 1u) << 1) | (((w[2] >> 15) & 1u) << 2) |
                           (((w[7] >> 13) & 7u) << 3);
    t.outside = static_cast<float>(share) / 63.0f;
    t.storeyBound = (t.flags & kTransition) != 0 ? 0u : share;
    t.g = glm::vec3(unbyte(w[3], 0), unbyte(w[3], 8), unbyte(w[4], 0));
    t.occ = glm::vec3(unbyte(w[4], 8), unbyte(w[5], 0), unbyte(w[5], 8));
    t.kp = glm::vec3(unbyte(w[6], 0), unbyte(w[6], 8), unbyte(w[7], 0)) * 2.0f;
    return t;
}

/// What the occlusion ao takes of C0, as a fraction of it (rtOcclusionLoss).
inline glm::vec3 occlusionLoss(const Texel& t, float ao) {
    glm::vec3 loss(0.0f);
    for (int c = 0; c < 3; ++c) {
        const float span = std::max(1.0f - t.knee[c], 1e-3f);
        loss[c] = t.occ[c] * std::clamp((1.0f - ao - t.knee[c]) / span, 0.0f, 1.0f);
    }
    return loss;
}

/// The traced terms at a pixel, as rt_lighting.glsli's RtLight carries them:
/// the weight the pixel found its own surface with, and the terms with the
/// Debug Graphics strengths of the point lights and the baked shade applied.
struct Traced {
    float weight = 0.0f;
    float sun = 1.0f;
    float ao = 1.0f;
    glm::vec3 point{0.0f};
    glm::vec3 pointOwn{0.0f};
    float bakedShade = 1.0f;
    float interiorShade = 1.0f;
};

/// rtBakedDarkness: what of a baked colour the building's lamps' shadows
/// take, before the strength: their share of it times the blocked part.
inline constexpr float kBakeRest = 0.5f;
inline float bakedDarkness(float baked, float vis) {
    baked = std::max(baked, 0.0f);
    return baked / (baked + kBakeRest) * (1.0f - vis);
}

/// rtFaceBasedNormal: the normal a room's baked colour takes the traced
/// lamps by - its face's, with the bumps' tilt (shading less vertex normal)
/// laid on it, so the smoothed vertex normals (MONR) do not light it.
inline glm::vec3 faceBasedNormal(const glm::vec3& face, const glm::vec3& vertexNormal, const glm::vec3& shading) {
    const glm::vec3 n = face + (shading - vertexNormal);
    return glm::dot(n, n) > 1e-8f ? glm::normalize(n) : face;
}

/// rtNormalAgreement: how much of a normal's own the lamps light by - all
/// of it within 15 degrees of its face, none past 35, smoothstep between.
inline constexpr float kNormalAgreeNear = 0.9659f;  // cos 15 degrees
inline constexpr float kNormalAgreeFar = 0.8192f;   // cos 35 degrees
inline float normalAgreement(const glm::vec3& face, const glm::vec3& n, float nearCos = kNormalAgreeNear,
                             float farCos = kNormalAgreeFar) {
    const float t = std::clamp((glm::dot(face, n) - farCos) / (nearCos - farCos), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// rtInteriorLampNormal: a room's baked colour's normal for the lamps - its
/// own where its vertex normal agrees with the face, faceBasedNormal past
/// 35 degrees, blended between.
inline glm::vec3 interiorLampNormal(const glm::vec3& face, const glm::vec3& vertexNormal, const glm::vec3& shading,
                                    float nearCos = kNormalAgreeNear, float farCos = kNormalAgreeFar) {
    const glm::vec3 n = glm::mix(faceBasedNormal(face, vertexNormal, shading), shading,
                                 normalAgreement(face, vertexNormal, nearCos, farCos));
    return glm::dot(n, n) > 1e-8f ? glm::normalize(n) : face;
}

/// rtInteriorShade: inside, what a baked colour is scaled by - `base` where
/// no point light reaches, all of it where their shadowed light (its
/// brightest channel) is `fullAt` or more, linear between, weighed in by the
/// weight the pixel found its own traced result with.
inline float interiorShade(const glm::vec3& pointLight, float weight, float base, float fullAt) {
    const float l = std::max(pointLight.r, std::max(pointLight.g, pointLight.b));
    float lit = std::clamp(l / std::max(fullAt, 1e-3f), 0.0f, 1.0f);
    lit = lit * (2.0f - lit);
    const float w = std::clamp(weight, 0.0f, 1.0f);
    return (1.0f - w) + w * (base + (1.0f - base) * lit);
}

/// rtCapLight: the light the traced lights add to a place, held to `cap` -
/// scaled whole so its brightest channel is the cap, its hue kept. 0 or less
/// is no limit.
inline glm::vec3 capLight(const glm::vec3& light, float cap) {
    const float m = std::max(light.r, std::max(light.g, light.b));
    return cap > 0.0f && m > cap ? light * (cap / m) : light;
}

/// The Debug Graphics strengths the apply pass reads (rtTune.x, lightTune.y,
/// lightTune.w), the most light the traced lights add (tune3.z, 0 no limit)
/// and the mode (occlusion from 2 on).
struct Tune {
    float shadowStrength = 0.65f;
    float sunShadow = 1.0f;
    float occlusion = 1.0f;
    float maxLight = 0.0f;
    int mode = 2;
};

/// The edge-on lift the shadow variants give a face turned toward the light's
/// edge (rtEdgeOnLift).
inline float edgeOnLift(const glm::vec3& n, const glm::vec3& toLight) {
    return std::clamp(std::pow(std::abs(1.2f - std::abs(glm::dot(n, toLight))), 4.0f), 0.0f, 1.0f);
}

struct Factors {
    glm::vec3 mul{1.0f};
    glm::vec3 add{0.0f};
};

/// rt_apply.frag.glsl: the factor and addend for a pixel's colour.
inline Factors apply(const Texel& t, const Traced& r, const Tune& tune, const glm::vec3& toSun) {
    Factors f;
    if ((t.flags & kValid) == 0) return f;
    float rs = 1.0f;
    if ((t.flags & kSun) != 0) {
        const float k = tune.shadowStrength;
        const bool after = (t.flags & kLiftAfter) != 0;
        const float lift = after ? edgeOnLift(t.normal, toSun) : 0.0f;
        const float lit0 = t.map + (1.0f - t.map) * lift;
        const float m = std::min(t.map, 1.0f - r.weight * tune.sunShadow * (1.0f - r.sun));
        const float lit = m + (1.0f - m) * lift;
        const float sigma0 = (1.0f - k) + k * lit0;
        const float sigma = (1.0f - r.weight) * (0.7f + 0.3f * lit) + r.weight * ((1.0f - k) + k * lit);
        rs = sigma / std::max(sigma0, 1e-4f);
    }
    const bool baked = (t.flags & kBaked) != 0;
    const float shade = (t.flags & kUnitFloor) != 0 ? 1.0f
                      : (t.flags & kInterior) != 0 ? std::min(r.bakedShade, r.interiorShade)
                                                    : r.bakedShade;
    const float rIn = baked ? shade : 1.0f;
    const glm::vec3 point = capLight(r.point, tune.maxLight);
    const glm::vec3 pointIn = baked ? capLight(r.pointOwn, tune.maxLight) : point;
    const float ao = tune.mode >= 2 ? 1.0f - r.weight * tune.occlusion * (1.0f - r.ao) : 1.0f;
    if ((t.flags & kTransition) != 0) {
        const float o = t.outside;
        const float rr = (1.0f - o) * rIn + o * rs;
        f.mul = glm::vec3(1.0f) + t.g * (o * (rs - 1.0f) + (1.0f - o) * (rIn - 1.0f)) - rr * occlusionLoss(t, ao);
        f.add = t.kp * ((1.0f - o) * pointIn + o * point);
        return f;
    }
    const float rr = rs * rIn;
    f.mul = glm::vec3(1.0f) + t.g * (rr - 1.0f) - rr * occlusionLoss(t, ao);
    f.add = t.kp * pointIn;
    return f;
}

}  // namespace wowee::rendering::rt_gbuffer

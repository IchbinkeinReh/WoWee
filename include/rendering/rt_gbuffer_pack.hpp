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
inline constexpr uint32_t kUnit = 32u;

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
    w[0] = static_cast<uint16_t>(quantise(oct.x, 1023.0f) | (quantise(knee.r, 63.0f) << 10));
    w[1] = static_cast<uint16_t>(quantise(oct.y, 1023.0f) | (quantise(knee.g, 63.0f) << 10));
    w[2] = static_cast<uint16_t>(byte(s.map) | ((s.flags & 63u) << 8));
    w[3] = static_cast<uint16_t>(byte(g.r) | (byte(g.g) << 8));
    w[4] = static_cast<uint16_t>(byte(g.b) | (byte(occ.r) << 8));
    w[5] = static_cast<uint16_t>(byte(occ.g) | (byte(occ.b) << 8));
    w[6] = static_cast<uint16_t>(byte(kp.r) | (byte(kp.g) << 8));
    w[7] = static_cast<uint16_t>(byte(kp.b) | (quantise(knee.b, 63.0f) << 8));
    return w;
}

inline Texel unpack(const Words& w) {
    const auto unbyte = [](uint32_t v, uint32_t shift) { return static_cast<float>((v >> shift) & 255u) / 255.0f; };
    Texel t;
    const glm::vec2 oct(static_cast<float>(w[0] & 1023u) / 1023.0f, static_cast<float>(w[1] & 1023u) / 1023.0f);
    t.normal = octDecode(oct * 2.0f - 1.0f);
    t.knee = glm::vec3(static_cast<float>(w[0] >> 10), static_cast<float>(w[1] >> 10),
                       static_cast<float>(w[7] >> 8)) / 63.0f;
    t.map = unbyte(w[2], 0);
    t.flags = (w[2] >> 8) & 63u;
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

/// rtInteriorShade: inside, what a baked colour is scaled by - `base` where
/// no point light reaches, all of it where their shadowed light (its
/// brightest channel) is `fullAt` or more, linear between, weighed in by the
/// weight the pixel found its own traced result with.
inline float interiorShade(const glm::vec3& pointLight, float weight, float base, float fullAt) {
    const float l = std::max(pointLight.r, std::max(pointLight.g, pointLight.b));
    const float lit = std::clamp(l / std::max(fullAt, 1e-3f), 0.0f, 1.0f);
    const float w = std::clamp(weight, 0.0f, 1.0f);
    return (1.0f - w) + w * (base + (1.0f - base) * lit);
}

/// The Debug Graphics strengths the apply pass reads (rtTune.x, lightTune.y,
/// lightTune.w) and the mode (occlusion from 2 on).
struct Tune {
    float shadowStrength = 0.6f;
    float sunShadow = 1.0f;
    float occlusion = 1.0f;
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
    const float shade = (t.flags & kInterior) != 0 ? std::min(r.bakedShade, r.interiorShade) : r.bakedShade;
    const float rr = rs * (baked ? shade : 1.0f);
    const float ao = tune.mode >= 2 ? 1.0f - r.weight * tune.occlusion * (1.0f - r.ao) : 1.0f;
    f.mul = glm::vec3(1.0f) + t.g * (rr - 1.0f) - rr * occlusionLoss(t, ao);
    f.add = t.kp * (baked ? r.pointOwn : r.point);
    return f;
}

}  // namespace wowee::rendering::rt_gbuffer

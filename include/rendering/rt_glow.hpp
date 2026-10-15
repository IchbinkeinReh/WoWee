#pragma once

/// The light of what glows: a model's or a building's emissive surfaces - its
/// unlit and additive batches, glow cards, lava, crystals, lamp glass
/// - and a particle emitter's live particles. The original
/// lights nothing with either; RtLighting does, in its modes only, beside the
/// lights the files carry (rt_point_lights.hpp).
///
/// A surface's glow is gathered once, when its model loads: the glowing
/// triangles binned by place into a few patches, each with its area, its
/// middle and its colour - the glowing texture's mean (VkTexture::averageColor,
/// from the BLP's small mip) times the batch's colour. Per frame each patch
/// near the camera is one light. An emitter's glow is gathered from its live
/// particles while they are written out for drawing, so one light per emitter
/// costs a few additions per particle.
///
/// Pure: the parts here are what tests/test_rt_glow.cpp checks.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/rt_point_lights.hpp"

namespace wowee::rendering::rt_lights {

/// A glowing patch of a model in its own space.
struct GlowPatch {
    glm::vec3 centre{0.0f};  // area-weighted middle
    float area = 0.0f;       // square yards, before the instance's scale
    glm::vec3 color{0.0f};   // area-weighted emitted colour, 0..1 a channel
    /// The batch that gives most of it, whose colour and transparency tracks
    /// scale it per frame where they move (~0 where none does).
    uint32_t animatedBatch = ~0u;
    /// The bone that carries most of it (rt_lights::boneCarried), ~0 for the
    /// model's own space.
    uint32_t bone = ~0u;
};

/// How large a cell glowing triangles are binned by, in yards of the model's
/// own space: one light for a lamp's glass, a few along a lava flow.
inline constexpr float kGlowCell = 6.0f;
/// The most lights one model's glow becomes.
inline constexpr size_t kMaxGlowPatches = 4;
/// A patch dimmer than this (its brightest channel) or smaller than this
/// (square yards) lights nothing worth a light.
inline constexpr float kMinGlowBrightness = 0.12f;
inline constexpr float kMinGlowArea = 0.0005f;

/// Gathers glowing triangles into patches (GlowPatch).
class GlowBuilder {
public:
    /// One triangle of a glowing batch, `color` what it emits (the texture's
    /// mean times the batch's colour), `batch` the batch it is of and whether
    /// that batch's colour moves, `bone` the bone that carries it (~0 none).
    void addTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color,
                     uint32_t batch, bool animated, uint32_t bone = ~0u) {
        const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
        if (!(area > 0.0f) || !std::isfinite(area)) return;
        const glm::vec3 mid = (a + b + c) / 3.0f;
        const glm::ivec3 k = glm::ivec3(glm::floor(mid / kGlowCell));
        const int64_t key = (int64_t(k.x & 0x1fffff) << 42) | (int64_t(k.y & 0x1fffff) << 21) | int64_t(k.z & 0x1fffff);
        Cell& cell = cells_[key];
        cell.area += area;
        cell.centre += glm::dvec3(mid) * double(area);
        cell.color += glm::dvec3(color) * double(area);
        if (animated && area > cell.animatedArea) {
            cell.animatedArea = area;
            cell.animatedBatch = batch;
        }
        if (bone != ~0u) {
            float& w = cell.boneArea[bone];
            w += area;
            if (w > cell.bestBoneArea) {
                cell.bestBoneArea = w;
                cell.bone = bone;
            }
        }
    }

    /// The patches: the brightest cells, at most `maxPatches`, the rest merged
    /// into the nearest of them so their light is not lost; the dim and the
    /// tiny dropped.
    std::vector<GlowPatch> finish(size_t maxPatches = kMaxGlowPatches) const {
        struct Ranked {
            GlowPatch patch;
            double power;
        };
        std::vector<Ranked> all;
        all.reserve(cells_.size());
        for (const auto& [key, cell] : cells_) {
            if (!(cell.area > 0.0)) continue;
            GlowPatch p;
            p.area = float(cell.area);
            p.centre = glm::vec3(cell.centre / cell.area);
            p.color = glm::vec3(cell.color / cell.area);
            p.animatedBatch = cell.animatedBatch;
            p.bone = cell.bone;
            const float bright = std::max(p.color.r, std::max(p.color.g, p.color.b));
            all.push_back({p, double(bright) * cell.area});
        }
        // By power, then by place, so the same model gives the same patches.
        std::sort(all.begin(), all.end(), [](const Ranked& lhs, const Ranked& rhs) {
            if (lhs.power != rhs.power) return lhs.power > rhs.power;
            if (lhs.patch.centre.x != rhs.patch.centre.x) return lhs.patch.centre.x < rhs.patch.centre.x;
            if (lhs.patch.centre.y != rhs.patch.centre.y) return lhs.patch.centre.y < rhs.patch.centre.y;
            return lhs.patch.centre.z < rhs.patch.centre.z;
        });
        const size_t kept = std::min(all.size(), maxPatches);
        std::vector<GlowPatch> out;
        for (size_t i = 0; i < kept; ++i) out.push_back(all[i].patch);
        // The rest into the nearest kept patch, area-weighted.
        for (size_t i = kept; i < all.size(); ++i) {
            const GlowPatch& r = all[i].patch;
            size_t best = 0;
            float bestD = INFINITY;
            for (size_t j = 0; j < out.size(); ++j) {
                const float d = glm::length(out[j].centre - r.centre);
                if (d < bestD) {
                    bestD = d;
                    best = j;
                }
            }
            GlowPatch& m = out[best];
            const float total = m.area + r.area;
            m.centre = (m.centre * m.area + r.centre * r.area) / total;
            m.color = (m.color * m.area + r.color * r.area) / total;
            m.area = total;
        }
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [](const GlowPatch& p) {
                                     const float bright = std::max(p.color.r, std::max(p.color.g, p.color.b));
                                     return !(bright >= kMinGlowBrightness) || !(p.area >= kMinGlowArea);
                                 }),
                  out.end());
        return out;
    }

    [[nodiscard]] bool empty() const { return cells_.empty(); }

private:
    struct Cell {
        double area = 0.0;
        glm::dvec3 centre{0.0};
        glm::dvec3 color{0.0};
        float animatedArea = 0.0f;
        uint32_t animatedBatch = ~0u;
        std::unordered_map<uint32_t, float> boneArea;
        float bestBoneArea = 0.0f;
        uint32_t bone = ~0u;
    };
    std::unordered_map<int64_t, Cell> cells_;
};

/// A glow's light by its power - its mean brightness times the square yards
/// it glows over, what it sends out in all: its strength and its reach
/// grow with the power's square root (the distance a given light level is
/// reached at), up to a ceiling. Set so a lamp's glass (some 0.2) lights as a
/// lamp does, about 0.9 and five yards, a hearth fire's flames (some 0.5) as
/// a fire does, and a candle's (some 0.01) a little, close by; a lava flow or
/// a wall of glowing crystals stops at the ceiling.
inline constexpr float kGlowStrengthPerRootPower = 2.0f;
inline constexpr float kGlowMaxStrength = 1.2f;
inline constexpr float kGlowReachPerRootPower = 12.0f;
inline constexpr float kGlowMinReach = 2.5f;
inline constexpr float kGlowMaxReach = 16.0f;

/// The light of a glowing patch or emitter at `position` (world), emitting
/// `color` (its mean over the glow, 0..1 a channel) over `area` square yards
/// (world): of the glow's hue, as strong and reaching as far as its power
/// says. Zero colour for one that gives none (selectLights drops it).
inline PointLight glowLight(const glm::vec3& position, const glm::vec3& color, float area, Source source) {
    PointLight out;
    out.position = position;
    out.source = source;
    const glm::vec3 c = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    const float bright = std::max(c.r, std::max(c.g, c.b));
    if (!(bright > 0.0f) || !(area > 0.0f) || !std::isfinite(area)) return out;
    const float root = std::sqrt(bright * area);
    out.attenuationEnd = std::clamp(kGlowReachPerRootPower * root, kGlowMinReach, kGlowMaxReach);
    out.attenuationStart = 0.15f * out.attenuationEnd;
    out.color = c / bright * std::min(kGlowStrengthPerRootPower * root, kGlowMaxStrength);
    return out;
}

/// What an emitter's particles give of light by how they are blended: the
/// adds (3 and 4) all of theirs, an unlit alpha or opaque one some (its
/// colour is its own, but it covers as much as it shows), and none for one
/// lit by the world (smoke, dust) or a modulate, which darkens.
inline float particleGlowWeight(uint16_t blendType, bool unlit) {
    if (blendType == 3 || blendType == 4) return 1.0f;
    if (blendType >= 5) return 0.0f;
    return unlit ? 0.35f : 0.0f;
}

/// An emitter's light against a surface's of the same power: a fire is
/// several emitters (flames, embers, sparks) over one another, each summed
/// on its own, and its particles crowd into a small space, so as a surface
/// would each of them lit a hearth's room like a bonfire. A third of the
/// strength, and a reach no longer than a fire's (fireLight).
inline constexpr float kParticleGain = 0.35f;
inline constexpr float kParticleMaxReach = 10.0f;

/// The most one model's emitters give together (their brightest channels
/// summed): a fire's few emitters about a fireLight's, while a model with
/// a flame on every hoof and along its mane (the Dreadsteed, a dozen and
/// more emitters) gives no more than that, not a dozen fires' worth.
inline constexpr float kMaxParticleModelStrength = 1.0f;

/// Scale lights[first..] down together so their strengths sum to at most
/// `cap`; what they light keeps its share of each.
inline void capTotalStrength(std::vector<PointLight>& lights, size_t first, float cap) {
    float total = 0.0f;
    for (size_t i = first; i < lights.size(); ++i) {
        const glm::vec3& c = lights[i].color;
        total += std::max(c.r, std::max(c.g, c.b));
    }
    if (!(total > cap)) return;
    const float scale = cap / total;
    for (size_t i = first; i < lights.size(); ++i) lights[i].color *= scale;
}

/// One emitter's live particles, summed as they are written out
/// (M2Renderer::prepareM2Particles).
struct ParticleGlow {
    glm::vec3 weightedPosition{0.0f};
    glm::vec3 weightedColor{0.0f};
    float weight = 0.0f;  // the area its particles emit over, by alpha and blend
    uint32_t count = 0;

    /// One particle at `position`, its colour (times its texture's mean),
    /// alpha and half size, and its emitter's particleGlowWeight.
    void add(const glm::vec3& position, const glm::vec3& color, float alpha, float halfSize, float blendWeight) {
        const float w = std::max(alpha, 0.0f) * 4.0f * halfSize * halfSize * blendWeight;
        if (!(w > 0.0f) || !std::isfinite(w)) return;
        weightedPosition += position * w;
        weightedColor += color * w;
        weight += w;
        ++count;
    }

    /// Its light: at the middle of its particles, of their mean colour, as
    /// large as they are together.
    [[nodiscard]] std::optional<PointLight> light() const {
        if (!(weight > 0.0f)) return std::nullopt;
        PointLight l = glowLight(weightedPosition / weight, weightedColor / weight, weight, Source::Particle);
        l.color *= kParticleGain;
        l.attenuationEnd = std::min(l.attenuationEnd, kParticleMaxReach);
        l.attenuationStart = 0.15f * l.attenuationEnd;
        return l;
    }
};

}  // namespace wowee::rendering::rt_lights

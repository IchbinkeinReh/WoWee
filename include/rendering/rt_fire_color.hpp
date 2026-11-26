#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/rt_glow.hpp"
#include "rendering/rt_point_lights.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

/// What colour a fire without a light of its own burns in (rt_lights::fireLight): that of its
/// own flames. A green brazier's lit the room orange.
namespace wowee::rendering::rt_lights {

/// A particle's colour, alpha or size at a share of its life, as the renderer draws it;
/// `none` of a block without keys.
template <typename T>
[[nodiscard]] T overLife(const std::vector<float>& times, const std::vector<T>& values, float share, const T& none) {
    if (values.empty()) return none;
    if (values.size() == 1 || times.empty()) return values[0];
    share = std::clamp(share, 0.0f, 1.0f);
    for (size_t i = 0; i + 1 < times.size(); ++i) {
        if (share > times[i + 1]) continue;
        const float span = times[i + 1] - times[i];
        const float f = span > 0.0f ? (share - times[i]) / span : 0.0f;
        return glm::mix(values[std::min(i, values.size() - 1)], values[std::min(i + 1, values.size() - 1)], f);
    }
    return values.back();
}

/// A model's flames summed by what of them is drawn: of each emitter its particles over their
/// lives - colour by alpha, area and how many there are at a time - times the mean of its
/// texture (`texel`, what of it shows); of each glowing card its colour by its area. What is
/// blended rather than added is smoke over the flames (isFlameEmitter), and counts where
/// there is nothing else.
struct FlameColor {
    glm::vec3 added{0.0f};
    glm::vec3 blended{0.0f};

    void addEmitter(const pipeline::M2ParticleEmitter& e, float perSecond, float life, const glm::vec3& texel) {
        const float weight = particleGlowWeight(e.blendingType, (e.flags & 0x1u) != 0);
        // The twinkle's share of the area (0x0097be80) among it.
        const float many = std::max(perSecond, 0.0f) * std::max(life, 0.0f) *
                           m2_particle::twinkleMeanArea(e.twinklePercent, e.twinkleScaleMin, e.twinkleScaleMax);
        if (!(weight * many > 0.0f)) return;
        glm::vec3& sum = isFlameEmitter(e) ? added : blended;
        constexpr int kSamples = 16;
        for (int i = 0; i < kSamples; ++i) {
            const float share = (static_cast<float>(i) + 0.5f) / kSamples;
            const float size = overLife(e.particleScale.timestamps, e.particleScale.floatValues, share, 1.0f);
            const float alpha =
                std::clamp(overLife(e.particleAlpha.timestamps, e.particleAlpha.floatValues, share, 1.0f), 0.0f, 1.0f);
            sum += overLife(e.particleColor.timestamps, e.particleColor.vec3Values, share, glm::vec3(1.0f)) * texel *
                   (weight * many * alpha * 4.0f * size * size / static_cast<float>(kSamples));
        }
    }
    /// Of the brightest channel 1; none where there is nothing to see.
    [[nodiscard]] std::optional<glm::vec3> color() const {
        for (const glm::vec3& sum : {added, blended}) {
            const float most = std::max(sum.r, std::max(sum.g, sum.b));
            if (most > 1e-6f && std::isfinite(most)) return glm::max(sum / most, glm::vec3(0.0f));
        }
        return std::nullopt;
    }
};

/// Where a colour is on the wheel: its hue as the direction, its saturation as how far out.
[[nodiscard]] inline glm::vec2 onWheel(const glm::vec3& c) {
    const float most = std::max(c.r, std::max(c.g, c.b));
    if (!(most > 0.0f)) return glm::vec2(0.0f);
    const float least = std::min(c.r, std::min(c.g, c.b));
    const glm::vec2 at(c.r - 0.5f * (c.g + c.b), 0.8660254f * (c.g - c.b));
    const float length = glm::length(at);
    return length > 0.0f ? at / length * ((most - least) / most) : glm::vec2(0.0f);
}

/// The colour of a fire's light: fireLight's own where its flames are about that - of the
/// client's fires all the ordinary ones, which stay as they were -, theirs where they are
/// another (green, blue, violet, red, white), and in between in between.
[[nodiscard]] inline glm::vec3 fireColor(bool torch, const std::optional<glm::vec3>& flames) {
    const glm::vec3 own = torch ? kTorchColor : kFireColor;
    if (!flames) return own;
    constexpr float kOrdinary = 0.33f, kOther = 0.42f;
    const float t =
        std::clamp((glm::length(onWheel(*flames) - onWheel(own)) - kOrdinary) / (kOther - kOrdinary), 0.0f, 1.0f);
    const glm::vec3 c = glm::mix(own, *flames, t * t * (3.0f - 2.0f * t));
    const float most = std::max(c.r, std::max(c.g, c.b));
    return most > 1e-6f ? c / most : own;
}

}  // namespace wowee::rendering::rt_lights

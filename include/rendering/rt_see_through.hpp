#pragma once
// The ray traced point lights of a unit drawn see-through - the player's body
// as the camera closes in, a ghost, a unit fading: it is in no G-buffer, so
// the trace finds nothing of it, and what the result holds at its pixels is
// what stands behind it, which its own torch lights or not as it happens to
// stand. Its lights are picked here instead, the few that give its body most,
// each with the share of it the buildings let through to it, and summed at
// its vertices (character.vert, rt_lamp_light.glsli).
#include "rendering/rt_point_lights.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::rt_see_through {

inline constexpr int kLights = 7;
/// What a light must give a body to be picked: less is not seen.
inline constexpr float kLeast = 0.004f;
/// How far over its centre a glow is taken to shine from (rt_trace's
/// RT_GLOW_LIFT, rtLightAim).
inline constexpr float kGlowLift = 1.0f;
/// How short of its light a ray stops (rt_trace's RT_LIGHT_STANDOFF).
inline constexpr float kStandoff = 0.45f;

/// Where a light shines from (rt_trace's rtLightAim).
inline glm::vec3 aim(const rt_lights::PointLight& l) {
    const bool glow = l.source == rt_lights::Source::Unit || l.source == rt_lights::Source::Emissive ||
                      l.source == rt_lights::Source::Particle;
    return glow ? l.position + glm::vec3(0.0f, 0.0f, kGlowLift) : l.position;
}

struct Picked {
    struct Lamp {
        glm::vec3 position{0.0f};
        float end = 0.0f;
        glm::vec3 color{0.0f};  // of what arrives
        float start = 0.0f;
    };
    std::array<Lamp, kLights> lamps{};
    int count = 0;
};

/// The lights of a body of `radius` yards round `centre`, the strongest there
/// first. `share(from)`: the part of a light shining from there that reaches
/// the body, asked of the ones picked alone; one within the body - the torch
/// in its hand - reaches it.
template <class Share>
Picked pick(const std::vector<rt_lights::PointLight>& lights, const glm::vec3& centre, float radius,
            bool realistic, Share&& share) {
    struct Ranked {
        float gives = 0.0f;
        size_t index = 0;
    };
    std::array<Ranked, kLights> best{};
    int n = 0;
    for (size_t i = 0; i < lights.size(); ++i) {
        const rt_lights::PointLight& l = lights[i];
        // At its nearest: a light beside an arm lights the arm.
        const float d = std::max(glm::length(aim(l) - centre) - radius, 0.0f);
        if (d >= l.attenuationEnd) continue;
        const float falloff = realistic ? rt_lights::realisticAttenuation(d, l.attenuationStart, l.attenuationEnd)
                                        : rt_lights::attenuation(d, l.attenuationStart, l.attenuationEnd);
        const float gives = std::max(l.color.r, std::max(l.color.g, l.color.b)) * falloff;
        if (!(gives >= kLeast)) continue;
        int k = n;
        if (n == kLights) {
            if (gives <= best[kLights - 1].gives) continue;
            k = kLights - 1;
        }
        for (; k > 0 && best[static_cast<size_t>(k - 1)].gives < gives; --k) {
            best[static_cast<size_t>(k)] = best[static_cast<size_t>(k - 1)];
        }
        best[static_cast<size_t>(k)] = {gives, i};
        n = std::min(n + 1, kLights);
    }
    Picked out;
    for (int k = 0; k < n; ++k) {
        const rt_lights::PointLight& l = lights[best[static_cast<size_t>(k)].index];
        const glm::vec3 from = aim(l);
        const bool own = glm::length(l.position - centre) <= radius + kStandoff;
        const float arrives = own ? 1.0f : share(from);
        if (!(arrives > 0.0f)) continue;
        Picked::Lamp& lamp = out.lamps[static_cast<size_t>(out.count++)];
        lamp.position = from;
        lamp.end = l.attenuationEnd;
        lamp.color = l.color * arrives;
        lamp.start = l.attenuationStart;
    }
    return out;
}

/// What a bone buffer holds past its bones (character.vert): the lights, a
/// position with its end and a colour with its start each, and in the last
/// column the wrap, the realistic falloff, the count and 1. All 0 without.
inline constexpr int kMatrices = 4;
inline std::array<glm::mat4, kMatrices> pack(const Picked& picked, float wrap, bool realistic) {
    static_assert(2 * kLights + 1 <= 4 * kMatrices);
    std::array<glm::mat4, kMatrices> m;
    m.fill(glm::mat4(0.0f));
    for (int i = 0; i < picked.count; ++i) {
        const Picked::Lamp& l = picked.lamps[static_cast<size_t>(i)];
        m[static_cast<size_t>(2 * i / 4)][(2 * i) % 4] = glm::vec4(l.position, l.end);
        m[static_cast<size_t>((2 * i + 1) / 4)][(2 * i + 1) % 4] = glm::vec4(l.color, l.start);
    }
    m[kMatrices - 1][3] = glm::vec4(wrap, realistic ? 1.0f : 0.0f, static_cast<float>(picked.count), 1.0f);
    return m;
}

}  // namespace wowee::rendering::rt_see_through

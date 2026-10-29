#pragma once

/// The Debug Graphics view "Kind of each light" (dbgview 14): every point
/// light's light drawn in the colour of what gave it (rt_lights::Source),
/// a marker at each light, and a legend naming the colours.
///
/// The colours are written twice, here for the markers and the legend and
/// in rt_trace.comp.glsl (rtLightKindColour) for the light itself; the two
/// lists must agree, index for index. Pure: tests/test_rt_light_kinds.cpp.

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/rt_point_lights.hpp"

namespace wowee::rendering::rt_light_kinds {

/// The Debug Graphics view that shows them.
inline constexpr int kDebugView = 14;

struct Kind {
    const char* name;    ///< what the legend calls it
    glm::vec3 colour;    ///< linear 0..1, as the trace draws it
};

/// By rt_lights::Source, in its order. Far apart, so each reads against the
/// others and against an unlit wall: white, red, yellow, blue, green, cyan
/// and magenta - no orange, which beside a fire's red and a lamp's yellow
/// could not be told from either.
inline constexpr std::array<Kind, rt_lights::kSourceCount> kKinds = {{
    {"Model light (doodad, object, spell)", {1.0f, 1.0f, 1.0f}},    // Model: white
    {"Fire", {1.0f, 0.12f, 0.05f}},                                 // Fire: red
    {"Lamp", {1.0f, 0.85f, 0.1f}},                                  // Lamp: yellow
    {"Building light (WMO)", {0.15f, 0.35f, 1.0f}},                 // Building: blue
    {"Unit light (character, creature)", {0.1f, 1.0f, 0.2f}},       // Unit: green
    {"Glow (emissive surface, lava)", {0.1f, 0.95f, 1.0f}},         // Emissive: cyan
    {"Particles", {1.0f, 0.15f, 0.85f}},                            // Particle: magenta
}};

/// The kind of a source, the last for one out of range.
inline const Kind& kindOf(rt_lights::Source source) {
    const size_t i = static_cast<size_t>(source);
    return kKinds[i < kKinds.size() ? i : kKinds.size() - 1];
}

/// What the view draws of a light of colour `light`: its brightness, in its
/// kind's colour - so a torch shows as bright as it lights, whatever its own
/// colour, and two kinds side by side mix as their lights do.
inline glm::vec3 tinted(rt_lights::Source source, const glm::vec3& light) {
    const float luminance = glm::dot(light, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return kindOf(source).colour * luminance;
}

/// A light as the view's markers need it: where, and what gave it.
struct Marker {
    glm::vec3 position{0.0f};
    rt_lights::Source source = rt_lights::Source::Model;
};

}  // namespace wowee::rendering::rt_light_kinds

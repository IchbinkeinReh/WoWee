#pragma once

// Rain on the ray traced water: a ring where a drop meets the surface, as
// many as the rain is hard, none where a roof, a bridge or the rock over a
// cave keeps the rain off. Not the client's (it has no traced water, and its
// rain leaves only the splash sprites, 0x00783c10); it goes with the swimmer's
// waves (swim_wave.hpp) under the same setting.
//
// The rings are made in the water shader (water.frag.glsl, rainRippleTilt):
// the surface is cut into cells, each with one drop a cycle at a place and a
// moment of its own, and a cell's drop falls in a cycle only while a number
// drawn for that cycle lies under the rain's strength. Nothing is carried
// from one frame to the next. ringSlope below is the shader's ring.

#include <algorithm>
#include <cmath>

namespace wowee::rendering::rain_ripple {

inline constexpr float kCell = 0.55f;       ///< yards a side of a drop's cell
inline constexpr float kPeriod = 0.9f;      ///< seconds from a drop to the next in its cell
inline constexpr float kMaxRadius = 0.3f;   ///< how far a ring runs out over its life
inline constexpr float kRingWidth = 0.045f;  ///< its crest's half-width
inline constexpr float kSecondLayer = 0.73f; ///< the second layer's cell, of kCell
inline constexpr float kRingSlope = 0.35f;  ///< the steepest it tilts the surface
inline constexpr float kFadeNear = 25.0f;   ///< yards from the camera where the rings start to fade
inline constexpr float kFadeFar = 70.0f;    ///< and where they are gone (they alias to shimmer past it)

/// How hard the rain falls on the water this frame, 0 for none: the
/// weather's eased intensity while it rains (not snow or sand), with the
/// weather drawn (weatherDensity above 0, the camera not in an interior
/// group) and the setting on.
inline float strength(bool raining, float intensity, int densityLevel, bool cameraIndoors, bool settingOn) {
    if (!raining || densityLevel <= 0 || cameraIndoors || !settingOn) return 0.0f;
    if (!(intensity > 0.0f)) return 0.0f;
    return std::clamp(intensity, 0.0f, 1.0f);
}

/// A ring `age` (0..1) through its life, at `dist` yards from where its drop
/// fell: the surface's slope outward, as the shader takes it.
inline float ringSlope(float dist, float age) {
    if (age < 0.0f || age >= 1.0f) return 0.0f;
    const float radius = age * kMaxRadius;
    const float x = (dist - radius) / kRingWidth;
    const float fade = (1.0f - age) * (1.0f - age);
    return std::exp(-x * x) * std::sin(x * 3.0f) * fade * kRingSlope;
}

}  // namespace wowee::rendering::rain_ripple

#pragma once

// The tessellated water (Graphics, Water page, "Tessellated water"): near the
// camera the ray traced water's liquid grid is cut finer by the GPU, so the
// short waves and the swimmers' rings and the trails' wakes move the surface
// itself instead of only tilting its per-pixel normal.
//
// This is the C++ twin of assets/shaders/water_tess.glsli (how finely an
// edge is cut, and what the finer surface carries) and of the ring heights
// in assets/shaders/water_interaction.glsli. Change one, change both; the
// tests check the curve's ends and that each height's slope is the slope the
// fragment stage shades with.
//
// Units are the world's: yards and seconds.

#include "core/coordinates.hpp"
#include "rendering/wave_spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::rendering::water_tess {

/// The liquid grid's cell (WaterRenderer's TILE_SIZE): an eighth of a chunk,
/// a chunk a sixteenth of an ADT tile.
inline constexpr float kGridEdge = core::coords::TILE_SIZE / 16.0f / 8.0f;
/// Near the camera an edge is cut into pieces this long...
inline constexpr float kTargetEdge = 0.3f;
/// ...into at most this many (every device allows 64).
inline constexpr float kMaxLevel = 16.0f;
/// Full cutting out to here; from kFar the edge is drawn whole.
inline constexpr float kNear = 20.0f;
inline constexpr float kFar = 90.0f;
/// The shortest wave the whole grid carries as height (water.vert's), and the
/// shortest the finely cut surface is given.
inline constexpr float kGridWavelength = 9.0f;
inline constexpr float kMinWavelength = 0.5f;

inline float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// No piece shorter than kMinPixels of the screen (WT_MIN_PIXELS): a
/// triangle a few pixels wide has its pixels shaded two or three times over.
inline constexpr float kMinPixels = 20.0f;

/// The "Water tessellation" slider (Graphics, Water page, watertessdensity):
/// how finely the grid is cut, in percent of the default. 100 is the cut
/// above exactly; 50 pieces twice as long (half as many along an edge), 200
/// half as long - the 0.3 yards near the camera and the 20-pixel floor alike,
/// the cap raised with it above 100 so the nearest edges are cut finer too.
/// Its "Tessellated water" switch turns the cutting off altogether.
inline constexpr int kDensityMinPercent = 25;
inline constexpr int kDensityMaxPercent = 200;
inline constexpr int kDensityDefaultPercent = 100;
/// The slider's percent as the factor the shaders take (push windBlend.w).
inline float densityFromPercent(int percent) {
    return static_cast<float>(std::clamp(percent, kDensityMinPercent, kDensityMaxPercent)) / 100.0f;
}
/// The most pieces an edge is cut into at `density` (WT_MAX_LEVEL x it above 1).
inline float maxLevel(float density) { return kMaxLevel * std::max(density, 1.0f); }

/// How many pieces an edge `edgeLength` yards long is cut into when its
/// midpoint is `dist` yards from the camera (water.tesc's edgeLevel), a pixel
/// being `yardsPerPixel` yards wide a yard from the camera (0: no pixel floor),
/// at the slider's `density` (densityFromPercent).
inline float level(float edgeLength, float dist, float yardsPerPixel = 0.0f, float density = 1.0f) {
    const float target = std::max(kTargetEdge, dist * yardsPerPixel * kMinPixels) / density;
    const float full = std::clamp(edgeLength / target, 1.0f, maxLevel(density));
    return 1.0f + (full - 1.0f) * (1.0f - smoothstep(kNear, kFar, dist));
}

/// The shortest wave moved as height `dist` yards from the camera.
inline float minWavelength(float dist, float density = 1.0f) {
    return std::max(kMinWavelength, kGridWavelength / level(kGridEdge, dist, 0.0f, density));
}

/// How much of the rings' and wakes' height is raised `dist` yards away.
inline float interaction(float dist, float density = 1.0f) {
    return smoothstep(2.0f, 8.0f, level(kGridEdge, dist, 0.0f, density));
}

/// The pieces the heights' integrals are taken in (the shader's PIECES).
inline constexpr int kHeightPieces = 8;

/// The physics waves' rings of an impulse (water_interaction.glsli's
/// impulseRingTilt): the slope along the radius, away from the impulse, the
/// fragment stage tilts the normal by, `r` yards out and `t` seconds after.
inline float impulseRingSlope(float r, float t, float sigma, float strength) {
    if (t <= 0.0f) return 0.0f;
    r = std::max(r, 0.05f);
    const float ks = wave_spectrum::impulseWavenumber(r, t) * sigma;
    const float spread = std::sqrt(sigma / (sigma + r));
    return strength * 0.6f * std::exp(-ks * ks) * ks * std::sin(wave_spectrum::impulsePhase(r, t)) * spread;
}

/// Their height (impulseRingHeight): the slope integrated in from the still
/// water outside, over the phase, by a linear Filon rule in kHeightPieces.
inline float impulseRingHeight(float r, float t, float sigma, float strength) {
    if (t <= 0.0f) return 0.0f;
    r = std::max(r, 0.05f);
    const float gt2 = wave_spectrum::kGravity * t * t;
    const float beta = gt2 / (4.0f * sigma);
    const float top = std::min(gt2 / (4.0f * r), 1.65f * std::sqrt(beta));
    const float du = top / static_cast<float>(kHeightPieces);
    float fa = 0.0f, sa = 0.0f, sum = 0.0f;
    for (int i = 1; i <= kHeightPieces; ++i) {
        const float u = du * static_cast<float>(i);
        const float q = u * u / beta;
        const float fb = std::exp(-q * q) * std::sqrt(sigma * u / (sigma * u + 0.25f * gt2));
        const float sb = std::sin(u);
        sum += (fb - fa) / du * (sb - sa);
        fa = fb;
        sa = sb;
    }
    const float integral = sum - fa * std::cos(top);
    return -0.6f * strength * sigma * integral;
}

/// The rings above are unfaded; their strength is faded by their age.
/// A swimmer's stroke's over its 1.6 s (water_interaction.glsli's
/// swimRingFade).
inline float swimRingFade(float t) { return 1.0f - smoothstep(1.2f, 2.2f, t); }

/// A trail point's (the wake with the physics waves; wakeRingFade and
/// WAKE_RING_* there): kWakeRingStrength of the point's strength, dying away
/// by e every kWakeRingDecay seconds and gone by kWakeRingLife, which the
/// renderer keeps the point for (WaterRenderer::updateWake). At half the
/// strength and gone in 2.2 s, a run of a few yards through the shallows
/// heaved the water by three quarters of a yard and was still a second after
/// the runner stopped.
inline constexpr float kWakeRingStrength = 0.15f;
inline constexpr float kWakeRingDecay = 2.0f;
inline constexpr float kWakeRingLife = 5.0f;
inline float wakeRingFade(float t) {
    return std::exp(-t / kWakeRingDecay) * (1.0f - smoothstep(0.6f * kWakeRingLife, kWakeRingLife, t));
}

/// The classic rings' crest (physics waves off): slope amp exp(-a^2) sin(b a)
/// along the ring's radius, a = (d - radius) / width...
inline float ringCrestSlope(float a, float b, float amp) { return amp * std::exp(-a * a) * std::sin(b * a); }
/// ...and its height (ringCrestHeight): amp width times the integral of
/// exp(-x^2) sin(b x) from -3 to -|a|, by the same rule.
inline float ringCrestHeight(float a, float b, float width, float amp) {
    const float lo = -3.0f;
    const float hi = -std::min(std::abs(a), 3.0f);
    if (hi <= lo) return 0.0f;
    const float dx = (hi - lo) / static_cast<float>(kHeightPieces);
    float fa = std::exp(-lo * lo);
    float sa = std::sin(b * lo);
    float cb = std::cos(b * lo);
    float sum = fa * cb / b;
    for (int i = 1; i <= kHeightPieces; ++i) {
        const float x = lo + dx * static_cast<float>(i);
        const float fb = std::exp(-x * x);
        const float sb = std::sin(b * x);
        cb = std::cos(b * x);
        sum += (fb - fa) / dx * (sb - sa) / (b * b);
        fa = fb;
        sa = sb;
    }
    sum -= fa * cb / b;
    return amp * width * sum;
}

}  // namespace wowee::rendering::water_tess

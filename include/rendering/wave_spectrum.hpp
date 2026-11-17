#pragma once

// Physically based water waves: a JONSWAP wind-sea spectrum sampled into a
// small set of Gerstner waves, each running at the speed the deep-water
// dispersion relation gives its length (omega^2 = g k), so long swells
// outrun short chop as they do on real water.
//
// This is the C++ twin of evaluatePhysicalWaves in
// assets/shaders/water_waves.glsli: the shader samples the same spectrum the
// same way every frame from three numbers (wind speed, fetch, wind
// direction), and the tests check these figures against the textbook ones.
// Change one, change both.
//
// Units are the world's: yards and seconds. Wind speed is given in metres per
// second, as people know it, and converted here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace wowee::rendering::wave_spectrum {

inline constexpr float kYardsPerMetre = 1.0936133f;
/// Gravity in yards per second squared.
inline constexpr float kGravity = 9.80665f * kYardsPerMetre;
inline constexpr float kPi = 3.14159265f;

/// How many waves the spectrum is sampled into (the shader's loop).
inline constexpr std::size_t kWaveCount = 12;
/// The band sampled, as multiples of the spectrum's peak frequency.
inline constexpr float kLowestOfPeak = 0.70f;
inline constexpr float kHighestOfPeak = 4.0f;
/// The sum of the waves' steepness (Q k a) is held under this, so the
/// Gerstner surface never folds over itself.
inline constexpr float kTotalSteepness = 0.75f;

/// Fetch, in yards: how far the wind has blown over open water. The open sea
/// is fully developed; a lake or river has a short one.
inline constexpr float kOceanFetch = 220000.0f;  // about 200 km
inline constexpr float kInlandFetch = 1100.0f;   // about 1 km
/// The prevailing wind's direction, radians counter-clockwise from +X.
inline constexpr float kWindDirection = 0.6f;

/// The spectrum's peak angular frequency (rad/s) for a wind speed (yd/s) and a
/// fetch (yd): JONSWAP's fetch-limited peak, never lower than the
/// Pierson-Moskowitz peak of a fully developed sea.
inline float peakOmega(float windYds, float fetch) {
    const float g = kGravity;
    const float u = std::max(windYds, 0.1f);
    const float f = std::max(fetch, 1.0f);
    const float jonswap = 22.0f * std::cbrt(g * g / (u * f));
    const float fullyDeveloped = 0.855f * g / u;
    return std::max(jonswap, fullyDeveloped);
}

/// JONSWAP's Phillips constant for a wind and fetch.
inline float alpha(float windYds, float fetch) {
    const float u = std::max(windYds, 0.1f);
    const float f = std::max(fetch, 1.0f);
    return 0.076f * std::pow(u * u / (f * kGravity), 0.22f);
}

/// The JONSWAP spectral density S(omega), in yd^2 s.
inline float density(float omega, float omegaP, float a) {
    if (omega <= 0.0f) return 0.0f;
    const float g = kGravity;
    const float sigma = omega <= omegaP ? 0.07f : 0.09f;
    const float d = (omega - omegaP) / (sigma * omegaP);
    const float r = std::exp(-0.5f * d * d);
    const float ratio = omegaP / omega;
    return a * g * g / std::pow(omega, 5.0f) * std::exp(-1.25f * ratio * ratio * ratio * ratio) *
           std::pow(3.3f, r);
}

/// Deep water: omega^2 = g k.
inline float wavenumber(float omega) { return omega * omega / kGravity; }
inline float phaseSpeed(float omega) { return omega / wavenumber(omega); }

struct Wave {
    float dirX = 1.0f, dirY = 0.0f;
    float k = 0.0f;       ///< wavenumber, rad/yd
    float omega = 0.0f;   ///< angular frequency, rad/s
    float amplitude = 0.0f; ///< yd
    float steepness = 0.0f; ///< Gerstner Q
    float phase = 0.0f;   ///< fixed offset, rad
};

/// The waves for a wind (m/s), a fetch (yd) and the wind's direction (rad,
/// counter-clockwise from +X), each amplitude scaled by `amplitudeScale`.
inline std::array<Wave, kWaveCount> sample(float windMps, float fetch, float windDir, float amplitudeScale = 1.0f) {
    std::array<Wave, kWaveCount> waves{};
    const float u = windMps * kYardsPerMetre;
    const float wp = peakOmega(u, fetch);
    const float a = alpha(u, fetch);
    const float lo = kLowestOfPeak * wp;
    const float step = std::pow(kHighestOfPeak / kLowestOfPeak, 1.0f / static_cast<float>(kWaveCount));
    float kaSum = 0.0f;
    for (std::size_t i = 0; i < kWaveCount; ++i) {
        // Geometric bins: each wave stands for the band [w0, w1).
        const float w0 = lo * std::pow(step, static_cast<float>(i));
        const float w1 = w0 * step;
        const float w = std::sqrt(w0 * w1);
        Wave& s = waves[i];
        s.omega = w;
        s.k = wavenumber(w);
        s.amplitude = std::sqrt(2.0f * density(w, wp, a) * (w1 - w0)) * amplitudeScale;
        // Spread about the wind (a cos^2 spreading, sampled at fixed angles
        // a golden-ratio step apart): the longest waves run nearest the wind.
        const float spread = (std::fmod(static_cast<float>(i) * 0.6180340f, 1.0f) - 0.5f) * kPi * 0.8f;
        const float angle = windDir + spread;
        s.dirX = std::cos(angle);
        s.dirY = std::sin(angle);
        s.phase = static_cast<float>(i) * 2.399963f;  // the golden angle, no two in step
        kaSum += s.k * s.amplitude;
    }
    // Steepness shared so that sum(Q k a) is kTotalSteepness at most.
    const float q = kaSum > 0.0f ? std::min(1.0f, kTotalSteepness / kaSum) : 0.0f;
    for (Wave& s : waves) s.steepness = q;
    return waves;
}

/// A disturbance's rings on deep water (Cauchy-Poisson; water.frag's
/// impulseRingTilt): `t` seconds after an impulse, the wave found `r` yards
/// away is the one whose group velocity (half its phase speed) has carried it
/// there, k = g t^2 / (4 r^2), at phase g t^2 / (4 r).
inline float impulseWavenumber(float r, float t) { return kGravity * t * t / (4.0f * r * r); }
inline float impulsePhase(float r, float t) { return kGravity * t * t / (4.0f * r); }
inline float groupSpeed(float k) { return 0.5f * std::sqrt(kGravity / k); }

/// The significant wave height (4 sigma) of the sampled waves, in yards.
inline float significantHeight(const std::array<Wave, kWaveCount>& waves) {
    float variance = 0.0f;
    for (const Wave& s : waves) variance += 0.5f * s.amplitude * s.amplitude;
    return 4.0f * std::sqrt(variance);
}

}  // namespace wowee::rendering::wave_spectrum

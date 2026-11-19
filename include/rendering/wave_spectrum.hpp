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

/// The waves for a wind between two rungs of a ladder (windRungs): the lower
/// rung's waves and the upper's, each set whole - its wavelengths and its
/// pace - and only their heights shared, by sqrt(1 - upperShare) and
/// sqrt(upperShare), so the sea's energy moves from one to the other in
/// step with the share. Each rung's waves have phases of their own, the
/// golden angle on from the rung under it (rung `lowerRung` and the one over
/// it), so a rung's sea is the same whichever side of the blend it is on as
/// the wind crosses it; and all take one steepness, kept so
/// that sum(Q k a) is kTotalSteepness at most. Without an upper share, the
/// lower rung's waves alone, as sample() gives them.
struct Spectrum {
    std::array<Wave, 2 * kWaveCount> waves{};
    std::size_t count = 0;
};
inline Spectrum sampleBlend(float windMps, float fetch, float windDir, float amplitudeScale, float upperShare,
                            float upperWind, float lowerRung = 0.0f) {
    Spectrum sp;
    const float f = std::clamp(upperShare, 0.0f, 1.0f);
    const bool blend = f > 0.0f && upperWind > 0.0f;
    const auto lower = sample(windMps, fetch, windDir, amplitudeScale * (blend ? std::sqrt(1.0f - f) : 1.0f));
    const float rungPhase = lowerRung * static_cast<float>(kWaveCount) * 2.399963f;
    for (std::size_t i = 0; i < kWaveCount; ++i) {
        sp.waves[i] = lower[i];
        sp.waves[i].phase += rungPhase;
    }
    sp.count = kWaveCount;
    if (blend) {
        const auto upper = sample(upperWind, fetch, windDir, amplitudeScale * std::sqrt(f));
        for (std::size_t i = 0; i < kWaveCount; ++i) {
            sp.waves[kWaveCount + i] = upper[i];
            sp.waves[kWaveCount + i].phase = rungPhase + static_cast<float>(kWaveCount + i) * 2.399963f;
        }
        sp.count = 2 * kWaveCount;
    }
    float kaSum = 0.0f;
    for (std::size_t i = 0; i < sp.count; ++i) kaSum += sp.waves[i].k * sp.waves[i].amplitude;
    const float q = kaSum > 0.0f ? std::min(1.0f, kTotalSteepness / kaSum) : 0.0f;
    for (std::size_t i = 0; i < sp.count; ++i) sp.waves[i].steepness = q;
    return sp;
}

/// The weather's wind (the slider's `baseWind` times the weather's
/// `windScale`) on a ladder of winds a factor kWindRung apart, from the
/// slider's: the rung under it, the one over it, and how far up between the
/// two it stands (by the logarithm). The waves are the two rungs' spectra,
/// crossfaded (sampleBlend), never one spectrum resampled for the wind of
/// the moment: a wave's phase is k x - omega t on the shader's clock, which
/// runs from the start of the session, and with k and omega following an
/// easing wind the crests raced - their speed is omega + t d(omega)/dt, a
/// thousand seconds in some thirty times their own, and as fast across the
/// world's thousands of yards by x dk/dt - so while the weather came in or
/// went the water looked as in a gale far stronger than either. On a rung
/// (the slider's own wind, and a full thunderstorm's 2.25 times it at the
/// default) one spectrum only.
/// sqrt(1 + kRainWind + kStormWind): a full thunderstorm is two rungs up.
inline constexpr float kWindRung = 1.6124515f;
struct WindRungs {
    float lower = 0.0f;       ///< m/s
    float upper = 0.0f;       ///< m/s, 0 when on a rung
    float upperShare = 0.0f;  ///< 0-1
    float lowerRung = 0.0f;   ///< the lower rung's place on the ladder, 0 the slider's wind
};
inline WindRungs windRungs(float baseWind, float windScale) {
    WindRungs r;
    r.lower = baseWind * windScale;
    if (!(windScale > 1.0f) || !(baseWind > 0.0f)) return r;
    float steps = std::log(windScale) / std::log(kWindRung);
    float n = std::floor(steps);
    float f = steps - n;
    if (f > 1.0f - 1e-4f) {
        n += 1.0f;
        f = 0.0f;
    }
    if (f < 1e-4f) f = 0.0f;
    r.lower = baseWind * std::pow(kWindRung, n);
    r.lowerRung = n;
    if (f > 0.0f) {
        r.upper = r.lower * kWindRung;
        r.upperShare = f;
    }
    return r;
}

/// The place on the ladder of winds (windRungs' steps: 0 the slider's wind,
/// 1 a rung up) a weather's wind scale stands at.
inline float rungPosition(float windScale) {
    return windScale > 1.0f ? std::log(windScale) / std::log(kWindRung) : 0.0f;
}
/// And back: the wind scale at a place on the ladder.
inline float windScaleAtRung(float position) {
    return position > 0.0f ? std::pow(kWindRung, position) : 1.0f;
}

/// The physics waves' place on the ladder, eased toward the weather's
/// (`target`, rungPosition of its wind scale) by `dt` seconds: as the
/// weather's own ease, a time constant of kRungEaseTime, but never slower
/// than kRungMinPace rungs a second, so that it arrives. Between two rungs
/// every pixel and vertex sums both rungs' waves (24, not 12), and the
/// weather's exponential ease, which the place followed before, came within
/// a part in ten thousand of a rung only a minute after any change: for that
/// minute the water cost half as much again (21 ms at Ratchet's sea in a
/// storm's first seconds, 14 steady). With the floor the ease is done in
/// under 8 seconds from clear to a full thunderstorm (two rungs) and back. In
/// its last stretch the place moves at the floor's pace, which the
/// exponential's has slowed to there: no step, no kink. It moves one way
/// only toward the target, so the waves' strength, which grows with it (the
/// energy moves from the lower rung's waves to the upper's in step with the
/// share), only rises or only falls, and never past either weather's.
inline constexpr float kRungEaseTime = 6.0f;  ///< seconds, as the weather's (Renderer: waterStorm_)
inline constexpr float kRungMinPace = 0.25f;  ///< rungs a second
inline float easeRung(float position, float target, float dt) {
    const float gap = target - position;
    const float distance = std::abs(gap);
    if (!(distance > 0.0f)) return target;
    const float t = std::max(dt, 0.0f);
    const float step = std::max(distance * (1.0f - std::exp(-t / kRungEaseTime)), kRungMinPace * t);
    if (step >= distance) return target;
    return position + (gap > 0.0f ? step : -step);
}

/// A disturbance's rings on deep water (Cauchy-Poisson; water_interaction.glsli's
/// impulseRingTilt): `t` seconds after an impulse, the wave found `r` yards
/// away is the one whose group velocity (half its phase speed) has carried it
/// there, k = g t^2 / (4 r^2), at phase g t^2 / (4 r).
inline float impulseWavenumber(float r, float t) { return kGravity * t * t / (4.0f * r * r); }
inline float impulsePhase(float r, float t) { return kGravity * t * t / (4.0f * r); }
inline float groupSpeed(float k) { return 0.5f * std::sqrt(kGravity / k); }

/// How much rougher the water is in rain and in a storm. `rain` is how hard it
/// rains (0-1, the weather's eased intensity), `storm` how stormy (0-1: a
/// thunderstorm's intensity, and part of a downpour's), `strength` the
/// slider (1 the default, 0 none, 2 twice). The physics waves take the wind
/// up by `windScale`; the hand-made waves their height by `amplitudeScale`
/// and their pace by `speedScale`. A storm counts far more than rain: at the
/// default, a full downpour raises the wind a third, a full thunderstorm
/// more than doubles it.
///
/// Calm weather - no rain, no storm - calms the physics waves the other way:
/// their height by `physicsAmplitudeScale`, the shortest wave left in the
/// surface's normal raised to `calmMinWavelength` yards, and the hand-made
/// detail normal laid over them by `detailScale`. At the default 7 m/s the
/// wind's full spectrum (its short chop, a slope sum of 0.8 on a lake) drew a
/// still, clear night's lake as a busy field of glints with a regular pattern;
/// as the rain and the storm come up these go back to 1, 0 and 1, so the
/// waves in a storm are what they were. The slider scales the calm as it
/// scales the rest: 0, the weather does nothing.
struct WeatherWaves {
    float windScale = 1.0f;
    float amplitudeScale = 1.0f;
    float speedScale = 1.0f;
    float physicsAmplitudeScale = 1.0f;
    float calmMinWavelength = 0.0f;
    float detailScale = 1.0f;
};
inline constexpr float kRainWind = 0.35f;
inline constexpr float kStormWind = 1.25f;
inline constexpr float kRainAmplitude = 0.5f;
inline constexpr float kStormAmplitude = 1.8f;
inline constexpr float kStormSpeed = 0.35f;
inline constexpr float kCalmAmplitude = 0.4f;      ///< 1 - physicsAmplitudeScale, wholly calm
inline constexpr float kCalmMinWavelength = 0.75f; ///< yards, wholly calm
inline constexpr float kCalmDetail = 0.75f;        ///< 1 - detailScale, wholly calm
inline WeatherWaves weatherWaves(float rain, float storm, float strength) {
    const float r = std::clamp(rain, 0.0f, 1.0f);
    const float s = std::clamp(storm, 0.0f, 1.0f);
    const float k = std::max(strength, 0.0f);
    WeatherWaves w;
    w.windScale = 1.0f + k * (kRainWind * r + kStormWind * s);
    w.amplitudeScale = 1.0f + k * (kRainAmplitude * r + kStormAmplitude * s);
    w.speedScale = 1.0f + k * kStormSpeed * s;
    // How calm: wholly without rain or storm, not at all once either is full.
    const float calm = std::min(k, 1.0f) * (1.0f - std::min(r + s, 1.0f));
    w.physicsAmplitudeScale = 1.0f - kCalmAmplitude * calm;
    w.calmMinWavelength = kCalmMinWavelength * calm;
    w.detailScale = 1.0f - kCalmDetail * calm;
    return w;
}

/// The hand-made waves' pace handed to the shader, whose phase is the pace
/// times its clock (`clock`, globalTime): the pace summed over the time
/// (`summedPace`, the seconds of the clock each at its weather's speedScale)
/// over that clock, so that the product is the sum and the phase moves on at
/// the pace of the moment, however the weather eases. 1 before the clock has
/// run.
inline float paceOverClock(double summedPace, double clock) {
    return clock > 1e-3 ? static_cast<float>(summedPace / clock) : 1.0f;
}

/// How stormy a rainy weather is: a thunderstorm (Weather.dbc row 86, the
/// server's WEATHER_STATE_THUNDERS) wholly, by its intensity; a downpour
/// (intensity over two thirds) half at most.
inline constexpr unsigned kThunderstormWeather = 86;
inline float storminess(bool raining, unsigned weatherId, float intensity) {
    const float i = std::clamp(intensity, 0.0f, 1.0f);
    if (weatherId == kThunderstormWeather) return i;
    if (!raining) return 0.0f;
    const float t = std::clamp((i - 0.66f) / 0.34f, 0.0f, 1.0f);
    return 0.5f * t * t * (3.0f - 2.0f * t);
}

/// The significant wave height (4 sigma) of the sampled waves, in yards.
inline float significantHeight(const std::array<Wave, kWaveCount>& waves) {
    float variance = 0.0f;
    for (const Wave& s : waves) variance += 0.5f * s.amplitude * s.amplitude;
    return 4.0f * std::sqrt(variance);
}

}  // namespace wowee::rendering::wave_spectrum

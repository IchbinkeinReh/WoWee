#pragma once

// The ray traced water's waves on the CPU: where the surface the GPU draws
// stands at a point, so what swims or floats there can ride it (the "Ride the
// waves" setting, Renderer::updateWaveRide). Visual only: nothing here is
// ever handed to the movement, the collision or the server, which keep the
// map's flat liquid level.
//
// This is the C++ twin of what assets/shaders/water.vert.glsl moves its
// vertices by: water_waves.glsli's evaluateGerstnerWaves,
// evaluatePhysicalWaves, evaluatePhysicalWavesShore and
// evaluateGerstnerWavesShore, with the push constants water_renderer.cpp
// fills for each surface (paramsFor below, which the renderer itself uses to
// fill them, so the two cannot drift apart) and the shader's clock (the
// PerFrame block's fogParams.z, Renderer::globalTime). The spectrum is
// wave_spectrum::sample and the shallow-water maths shallow_water.hpp's, the
// twins of pwSample and the sw* helpers. Change the shader, change this.
//
// Only the waves the coarse liquid grid moves as height are counted: those
// of kVertexMinWavelength and longer (water.vert's cut-off). The tessellated
// water moves shorter ones near the camera too, but a body's size filters
// those out, and they are under half a yard.

#include "rendering/shallow_water.hpp"
#include "rendering/wave_spectrum.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wowee::rendering {

/// The traced water's waves at their default (rtwaveamp 100%): half the
/// height they were first made with, which in the traced mirror read as a
/// choppy sea on a still pond.
inline constexpr float kRtWaveAmpScale = 0.5f;

namespace wave_surface {

/// The shortest wave the liquid grid's vertices move as height, yards: the
/// vertex stage's cut-off (water_waves.glsli's PW_VERTEX_MIN_WAVELENGTH,
/// which water.vert passes). Change one, change both.
inline constexpr float kVertexMinWavelength = 9.0f;

/// What the shader is handed for one surface (WaterPushConstants): the
/// hand-made waves' height, frequency and pace, or, with physics waves on
/// (`wind` over 0), the amplitude slider, the fetch and the wind's direction
/// in their places; the basic liquid type (0 water, 1 ocean); and whether the
/// shore waves are on for it.
struct WaveParams {
    float amp = 0.0f;        ///< push.waveAmp
    float freq = 0.0f;       ///< push.waveFreq
    float speed = 0.0f;      ///< push.waveSpeed
    float basicType = 0.0f;  ///< push.liquidBasicType
    float wind = 0.0f;       ///< push.pad2, m/s; 0 the hand-made waves
    bool shore = false;      ///< push.shore.x
    float surf = 1.0f;       ///< push.shore.y, the fragment stage's
    /// push.shore.z, the fragment stage's: the shortest physics wave kept in
    /// the normal for the weather, yards (WeatherWaves::calmMinWavelength).
    float normalMinWavelength = 0.0f;
    /// push.shore.w, the fragment stage's: the share of the hand-made detail
    /// normal laid over the physics waves (WeatherWaves::detailScale).
    float detail = 1.0f;
    /// push.windBlend: the physics waves' upper rung's wind, m/s, and its
    /// share (wave_spectrum::windRungs); `wind` is then the lower rung's.
    float windUpper = 0.0f;
    float windUpperShare = 0.0f;
    float windRung = 0.0f;  ///< push.windBlend.z, the lower rung's place (its waves' phases)
};

/// The waves' settings this frame, as the renderer holds them (RtWaterFrame).
struct WaveSettings {
    float waveAmp = 1.0f;   ///< rtwaveamp, 1 the default
    float waveFreq = 1.0f;  ///< rtwavefreq
    bool physicsWaves = false;
    float windSpeed = 7.0f;  ///< m/s
    wave_spectrum::WeatherWaves weather{};
    bool shoreWaves = true;
};

/// The basic liquid type of a surface's liquidType, as the water renderer
/// reads it: 0 water, 1 ocean, 2 magma, 3 slime.
inline uint8_t basicTypeOf(uint16_t liquidType) {
    return liquidType == 0 ? 0 : static_cast<uint8_t>((liquidType - 1) % 4);
}

/// The push constants the traced water draws a surface with, the waves'
/// part: WaterRenderer::render fills them from this. None for magma and
/// slime, which the traced water does not draw (the client's own liquid
/// does, flat). A building's water has no waves at all: its hand-made waves
/// are of no height, and it takes neither the physics nor the shore waves.
inline std::optional<WaveParams> paramsFor(uint16_t liquidType, bool wmoWater, const WaveSettings& s) {
    const uint8_t basicType = basicTypeOf(liquidType);
    if (basicType >= 2) return std::nullopt;
    const bool canalProfile = wmoWater || liquidType == 5;
    // WMO water gets no wave displacement - prevents visible slosh at
    // geometry edges (bridges, docks) where water is far below the surface.
    const float waveAmp = wmoWater ? 0.0f : (basicType == 1 ? 0.35f : 0.08f);
    const float waveFreq = canalProfile ? 0.35f : (basicType == 1 ? 0.20f : 0.30f);
    const float waveSpeed = canalProfile ? 1.00f : (basicType == 1 ? 1.20f : 1.40f);
    WaveParams p;
    // Rain and storms roughen the water (wave_spectrum::weatherWaves): the
    // hand-made waves higher and quicker, the physics waves' wind stronger.
    p.amp = waveAmp * kRtWaveAmpScale * s.waveAmp * s.weather.amplitudeScale;
    p.freq = waveFreq * s.waveFreq;
    p.speed = waveSpeed * s.weather.speedScale;
    // Physics waves (water_waves.glsli, wave_spectrum.hpp): the wind's
    // spectrum, with the open sea's fetch for an ocean and a lake's for the
    // rest. amp is the amplitude slider alone (100% the spectrum's own
    // height), freq the fetch, speed the wind's direction and wind the wind.
    if (s.physicsWaves && !wmoWater) {
        // Calm weather calms them (WeatherWaves::physicsAmplitudeScale).
        p.amp = s.waveAmp * s.weather.physicsAmplitudeScale;
        p.freq = basicType == 1 ? wave_spectrum::kOceanFetch : wave_spectrum::kInlandFetch;
        p.speed = wave_spectrum::kWindDirection;
        // Two rungs of a ladder of winds, crossfaded (windRungs), never the
        // wind of the moment: the waves' phases race as the weather eases.
        const auto rungs = wave_spectrum::windRungs(s.windSpeed, s.weather.windScale);
        p.wind = std::max(rungs.lower, 0.5f);
        p.windUpper = rungs.upper;
        p.windUpperShare = rungs.upperShare;
        p.windRung = rungs.lowerRung;
        p.normalMinWavelength = s.weather.calmMinWavelength;
        p.detail = s.weather.detailScale;
    }
    // The shore waves on the ground's water, which alone knows its bed; their
    // surf whitens more in a storm.
    if (s.shoreWaves && !wmoWater) {
        p.shore = true;
        p.surf = s.weather.amplitudeScale;
    }
    p.basicType = static_cast<float>(basicType);
    return p;
}

/// The waves a rider rides on a surface: none with the setting off, while
/// the traced water is not drawn (the client's own liquid is flat), on
/// magma and slime, and on a building's water, which has no waves.
inline std::optional<WaveParams> rideParams(bool rideOn, bool tracedWaterDrawn, uint16_t liquidType, bool wmoWater,
                                            const WaveSettings& s) {
    if (!rideOn || !tracedWaterDrawn || wmoWater) return std::nullopt;
    return paramsFor(liquidType, wmoWater, s);
}

/// The waves at a point of the flat surface (GerstnerResult's geometric part):
/// how far they move it, and the surface's tangent along X and Y there.
struct Waves {
    glm::vec3 displacement{0.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    glm::vec3 binormal{0.0f, 1.0f, 0.0f};
};

namespace detail {

inline float smoothstep(float e0, float e1, float x) { return shallow_water::smoothstep(e0, e1, x); }

/// water_waves.glsli's pwKeep with its two lengths the same: all of a wave
/// at or over the cut-off, none under it.
inline float keep(float wavelength, float minWavelength) { return wavelength >= minWavelength ? 1.0f : 0.0f; }

/// One Gerstner wave added (swAddWave), `keep` of it moved as height. The
/// tangents here take the kept waves only: the slope of the surface the grid
/// draws, which a body rides. (The shader's normal counts every wave, the
/// short ones as well, for its shading.)
inline void addWave(Waves& r, glm::vec2 dir, float k, float A, float Q, float lean, float phase, float keep) {
    const float s = std::sin(phase);
    const float c = std::cos(phase);
    // Along the wave by Q c (and the lean's share of the height), up by s.
    const float horizontal = Q * c + lean * s;
    r.displacement += glm::vec3(dir * (keep * A * horizontal), keep * A * s);
    // The surface's slope along X and along Y: the sideways part of each
    // tangent is the wave's direction times the slope of the sideways
    // movement, the upward part the height's.
    const float WA = keep * k * A;
    const glm::vec2 sideways = dir * (WA * (lean * c - Q * s));
    r.tangent += glm::vec3(sideways * dir.x, dir.x * WA * c);
    r.binormal += glm::vec3(sideways * dir.y, dir.y * WA * c);
}

inline glm::vec2 direction(const wave_spectrum::Wave& w) { return {w.dirX, w.dirY}; }

}  // namespace detail

/// evaluateGerstnerWaves: the hand-made set of six (an ocean's or inland
/// water's), or magma's and slime's slow undulation.
inline Waves gerstner(glm::vec2 pos, float time, float amp, float freq, float spd, float basicType) {
    Waves r;
    if (basicType >= 1.5f) {
        const float a = pos.x * freq * 0.5f + time * spd * 0.3f;
        const float b = pos.y * freq * 0.3f + time * spd * 0.5f;
        const float wave = std::sin(a) * 0.4f + std::sin(b) * 0.3f;
        r.displacement.z = wave * amp * 0.5f;
        r.tangent = glm::vec3(1.0f, 0.0f, std::cos(a) * freq * 0.5f * amp * 0.5f * 0.4f);
        r.binormal = glm::vec3(0.0f, 1.0f, std::cos(b) * freq * 0.3f * amp * 0.5f * 0.3f);
        return r;
    }
    static const std::array<glm::vec2, 6> dirs = {
        glm::normalize(glm::vec2(0.86f, 0.51f)),  glm::normalize(glm::vec2(-0.47f, 0.88f)),
        glm::normalize(glm::vec2(0.32f, -0.95f)), glm::normalize(glm::vec2(-0.93f, -0.37f)),
        glm::normalize(glm::vec2(0.67f, -0.29f)), glm::normalize(glm::vec2(-0.15f, 0.74f)),
    };
    const bool ocean = basicType > 0.5f;
    // Ocean: a broad range of scales for chop; inland water gentler.
    static constexpr std::array<float, 6> kOceanAmp = {1.0f, 0.55f, 0.30f, 0.18f, 0.10f, 0.06f};
    static constexpr std::array<float, 6> kOceanFreq = {0.7f, 1.3f, 2.1f, 3.4f, 5.0f, 7.5f};
    static constexpr std::array<float, 6> kOceanSpeed = {0.8f, 1.0f, 1.3f, 1.6f, 2.0f, 2.5f};
    static constexpr std::array<float, 6> kOceanSteep = {0.35f, 0.30f, 0.25f, 0.20f, 0.15f, 0.10f};
    static constexpr std::array<float, 6> kInlandAmp = {0.5f, 0.25f, 0.15f, 0.08f, 0.05f, 0.03f};
    static constexpr std::array<float, 6> kInlandFreq = {1.0f, 1.8f, 3.0f, 4.5f, 7.0f, 10.0f};
    static constexpr std::array<float, 6> kInlandSpeed = {0.6f, 0.9f, 1.2f, 1.5f, 1.9f, 2.3f};
    static constexpr std::array<float, 6> kInlandSteep = {0.20f, 0.18f, 0.15f, 0.12f, 0.10f, 0.08f};
    for (size_t i = 0; i < 6; ++i) {
        const float w = freq * (ocean ? kOceanFreq[i] : kInlandFreq[i]);
        const float A = amp * (ocean ? kOceanAmp[i] : kInlandAmp[i]);
        if (!(A > 0.0f) || !(w > 0.0f)) continue;  // no height: nothing moves (the shader's Q is then 1)
        const float phi = spd * (ocean ? kOceanSpeed[i] : kInlandSpeed[i]) * w;
        const float Q = std::clamp((ocean ? kOceanSteep[i] : kInlandSteep[i]) / (w * A * 6.0f), 0.0f, 1.0f);
        // The shader's sum, written out there wave by wave: a Gerstner wave
        // with no lean, all of it moved.
        detail::addWave(r, dirs[i], w, A, Q, 0.0f, w * glm::dot(dirs[i], pos) + phi * time, 1.0f);
    }
    return r;
}

/// pwWaves: the sampled spectrum's waves in deep water.
inline Waves physicalDeep(const wave_spectrum::Spectrum& sp, glm::vec2 pos, float time, float minWavelength) {
    Waves r;
    for (size_t i = 0; i < sp.count; ++i) {
        const auto& w = sp.waves[i];
        const float keep = detail::keep(2.0f * wave_spectrum::kPi / w.k, minWavelength);
        const glm::vec2 dir = detail::direction(w);
        detail::addWave(r, dir, w.k, w.amplitude, w.steepness, 0.0f,
                        w.k * glm::dot(dir, pos) - w.omega * time + w.phase, keep);
    }
    return r;
}

/// evaluatePhysicalWaves (`fetch` push.waveFreq, `windMps` push.pad2,
/// `windDir` push.waveSpeed).
inline Waves physical(glm::vec2 pos, float time, float ampScale, float fetch, float windMps, float windDir,
                      float minWavelength, float upperShare = 0.0f, float upperWind = 0.0f, float rung = 0.0f) {
    return physicalDeep(wave_spectrum::sampleBlend(windMps, fetch, windDir, ampScale, upperShare, upperWind, rung),
                        pos, time, minWavelength);
}

/// evaluatePhysicalWavesShore: the physics waves over the bed (x the depth,
/// yz the way to the shallows, w the slope; WaterSurface::bed).
inline Waves physicalShore(glm::vec2 pos, float time, float ampScale, float fetch, float windMps, float windDir,
                           float minWavelength, const glm::vec4& bed, float upperShare = 0.0f,
                           float upperWind = 0.0f, float rung = 0.0f) {
    namespace sw = shallow_water;
    constexpr size_t kMax = 2 * wave_spectrum::kWaveCount;
    const auto spectrum = wave_spectrum::sampleBlend(windMps, fetch, windDir, ampScale, upperShare, upperWind, rung);
    const auto& sp = spectrum.waves;
    const size_t N = spectrum.count;
    const float h = bed.x;
    // Deep enough that even the longest wave (either rung's first) hardly
    // feels the bed.
    const float longestK = N > wave_spectrum::kWaveCount ? std::min(sp[0].k, sp[wave_spectrum::kWaveCount].k) : sp[0].k;
    if (h * longestK > sw::kDeepKh) return physicalDeep(spectrum, pos, time, minWavelength);

    const float sloped = std::min(glm::length(glm::vec2(bed.y, bed.z)), 1.0f);
    const glm::vec2 toShore = sloped > 1e-4f ? glm::normalize(glm::vec2(bed.y, bed.z)) : glm::vec2(0.0f);
    const float bedSlope = std::max(bed.w, 0.0f);
    const float fade = sw::shallowFade(h);

    std::array<float, kMax> amps{};
    std::array<float, kMax> shares{};
    std::array<float, kMax> kShore{};
    float sumA2 = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        const float w = sp[i].omega;
        const float k = sw::wavenumber(w, h);
        amps[i] = sp[i].amplitude * sw::shoalingCoefficient(w, k, h) * fade;
        kShore[i] = sw::shoreWavenumber(w, h, bedSlope);
        const float toward = detail::smoothstep(-0.2f, 0.4f, glm::dot(detail::direction(sp[i]), toShore));
        const float longEnough = detail::smoothstep(sw::kMinShoreWavelength, 2.0f * sw::kMinShoreWavelength,
                                                    2.0f * wave_spectrum::kPi / std::max(kShore[i], 1e-4f));
        shares[i] = sw::refractionBlend(k * std::max(h, 0.0f)) * sloped * toward * longEnough;
        sumA2 += amps[i] * amps[i];
    }

    const float breaker = sw::kSignificantHeightFactor * std::sqrt(sumA2) / (sw::kBreakerGamma * std::max(h, 1e-3f));
    const float limit = sw::limitScale(breaker);
    float kaSum = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        amps[i] *= limit;
        kaSum += amps[i] * (sp[i].k + (kShore[i] - sp[i].k) * shares[i]);
    }
    const sw::Steepness st = sw::steepness(kaSum, breaker);

    Waves r;
    for (size_t i = 0; i < N; ++i) {
        const glm::vec2 dir = detail::direction(sp[i]);
        const float w = sp[i].omega;
        const float k = sp[i].k;
        const float A = amps[i];
        // The deep part, at its deep phase.
        detail::addWave(r, dir, k, A * (1.0f - shares[i]), st.q, st.lean,
                        k * glm::dot(dir, pos) - w * time + sp[i].phase,
                        detail::keep(2.0f * wave_spectrum::kPi / k, minWavelength));
        // The shore part, at the phase the depth gives it.
        const float shoreAmp = A * shares[i];
        if (shoreAmp > 0.0f) {
            detail::addWave(r, toShore, kShore[i], shoreAmp, st.q, st.lean,
                            -sw::shorePhase(w, h) - w * time + sp[i].phase,
                            detail::keep(2.0f * wave_spectrum::kPi / std::max(kShore[i], 1e-4f), minWavelength));
        }
    }
    return r;
}

/// evaluateGerstnerWavesShore: the hand-made waves held under the depth's
/// limit and died away at the waterline.
inline Waves gerstnerShore(glm::vec2 pos, float time, float amp, float freq, float spd, float basicType,
                           const glm::vec4& bed) {
    namespace sw = shallow_water;
    Waves r = gerstner(pos, time, amp, freq, spd, basicType);
    if (basicType >= 1.5f) return r;
    // The six waves' sum of squared heights over amp^2 (the two sets above).
    const float sumA2 = amp * amp * (basicType > 0.5f ? 1.4385f : 0.3448f);
    const float h = bed.x;
    const float fade = sw::shallowFade(h);
    const float seaHeight = sw::kSignificantHeightFactor * std::sqrt(sumA2);
    const float breaking = seaHeight * fade / (sw::kBreakerGamma * std::max(h, 1e-3f));
    const float scale = fade * sw::limitScale(breaking);
    r.displacement *= scale;
    r.tangent = glm::vec3(1.0f, 0.0f, 0.0f) + (r.tangent - glm::vec3(1.0f, 0.0f, 0.0f)) * scale;
    r.binormal = glm::vec3(0.0f, 1.0f, 0.0f) + (r.binormal - glm::vec3(0.0f, 1.0f, 0.0f)) * scale;
    return r;
}

/// What water.vert moves the grid's vertex at `pos` by, over the bed `bed`.
inline Waves evaluate(const WaveParams& p, glm::vec2 pos, float time, const glm::vec4& bed,
                      float minWavelength = kVertexMinWavelength) {
    if (p.shore) {
        return p.wind > 0.0f ? physicalShore(pos, time, p.amp, p.freq, p.wind, p.speed, minWavelength, bed,
                                             p.windUpperShare, p.windUpper, p.windRung)
                             : gerstnerShore(pos, time, p.amp, p.freq, p.speed, p.basicType, bed);
    }
    return p.wind > 0.0f ? physical(pos, time, p.amp, p.freq, p.wind, p.speed, minWavelength, p.windUpperShare,
                                    p.windUpper, p.windRung)
                         : gerstner(pos, time, p.amp, p.freq, p.speed, p.basicType);
}

/// The displaced surface over a point of the world.
struct SurfaceSample {
    float height = 0.0f;                 ///< above the flat level, yards
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  ///< the surface's, unit
    glm::vec2 base{0.0f};                ///< the flat surface's point the waves moved there
};

/// How many times sample() steps toward the point the waves carried over
/// the one asked about. The kept waves' sum of Q k a, the most a step can
/// be out by as a share of the last, is under 0.3 for the hand-made waves
/// and at most 0.75 for the physics waves (far less for the long ones the
/// grid keeps), so four leave a fraction of the horizontal movement over.
inline constexpr int kInvertIterations = 4;

/// The displaced surface at the world's `q`: the Gerstner waves move points
/// sideways as well as up, so the point of the flat surface they carry over
/// `q` is found first, by fixed-point iteration (base = q - d(base)), and
/// its height and normal taken. `bedAt(xy)` gives the bed there (a vec4 as
/// WaterSurface::bed; ignored unless the shore waves are on).
template <class BedAt>
SurfaceSample sample(const WaveParams& p, glm::vec2 q, float time, BedAt&& bedAt,
                     int iterations = kInvertIterations, float minWavelength = kVertexMinWavelength) {
    glm::vec2 base = q;
    for (int i = 0; i < iterations; ++i) {
        const Waves w = evaluate(p, base, time, bedAt(base), minWavelength);
        base = q - glm::vec2(w.displacement.x, w.displacement.y);
    }
    const Waves w = evaluate(p, base, time, bedAt(base), minWavelength);
    SurfaceSample s;
    s.height = w.displacement.z;
    const glm::vec3 n = glm::cross(w.tangent, w.binormal);
    const float len = glm::length(n);
    s.normal = len > 1e-6f && n.z > 0.0f ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
    s.base = base;
    return s;
}

// --- Riding it ---

/// A critically damped spring toward a moving target, stepped exactly (any
/// frame time is stable): it follows without overshoot and without the
/// corner a plain exponential ease leaves where the target turns. `tau` is
/// the time constant, seconds.
struct CriticalSpring {
    float value = 0.0f;
    float velocity = 0.0f;
    void step(float target, float dt, float tau) {
        if (!(dt > 0.0f)) return;
        const float omega = 1.0f / std::max(tau, 1e-3f);
        const float y = value - target;
        const float a = velocity + omega * y;
        const float e = std::exp(-omega * dt);
        value = target + (y + a * dt) * e;
        velocity = (velocity - omega * a * dt) * e;
    }
    void reset(float v = 0.0f) {
        value = v;
        velocity = 0.0f;
    }
};

/// The time constant the riders follow the waves with, seconds.
inline constexpr float kRideTau = 0.15f;
/// The most a rider is tilted by the surface, radians (15 degrees).
inline constexpr float kMaxTilt = 0.2617994f;
/// How far a swimmer may be under the flat surface and still ride the waves
/// fully, and past which it does not ride them at all, yards: a swimmer at
/// the surface floats some three quarters of its height down; one diving is
/// left where it is.
inline constexpr float kRideFullDepth = 3.0f;
inline constexpr float kRideNoneDepth = 6.0f;

/// How much of the waves a swimmer `depth` yards under the flat surface
/// rides: all of it near the surface, none of it deep down or out of the
/// water.
inline float swimmerRideWeight(float depth) {
    if (!(depth >= 0.0f)) return 0.0f;
    return 1.0f - detail::smoothstep(kRideFullDepth, kRideNoneDepth, depth);
}

/// A surface normal as the slopes a rider is tilted by, held within
/// `maxTilt` of upright: x the rise along +X, y along +Y (the normal is
/// (-x, -y, 1) normalised).
inline glm::vec2 slopesOf(const glm::vec3& normal, float maxTilt = kMaxTilt) {
    if (!(normal.z > 1e-4f)) return glm::vec2(0.0f);
    glm::vec2 slope(-normal.x / normal.z, -normal.y / normal.z);
    const float len = glm::length(slope);
    const float most = std::tan(maxTilt);
    if (len > most) slope *= most / len;
    return slope;
}

/// The rotation that stands the world's up on the surface of `slopes`.
inline glm::quat tiltOf(const glm::vec2& slopes) {
    const glm::vec3 up = glm::normalize(glm::vec3(-slopes.x, -slopes.y, 1.0f));
    const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), up);
    const float s = glm::length(axis);
    if (s < 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::angleAxis(std::atan2(s, up.z), axis / s);
}

/// A rider's pose on the waves, eased: how far it is raised and the slopes
/// it leans by.
struct RidePose {
    CriticalSpring heave;
    CriticalSpring slopeX;
    CriticalSpring slopeY;
    void step(float targetHeave, const glm::vec2& targetSlopes, float dt, float tau = kRideTau) {
        heave.step(targetHeave, dt, tau);
        // Leaning eases twice as slowly, so a short chop does not rock it.
        slopeX.step(targetSlopes.x, dt, 2.0f * tau);
        slopeY.step(targetSlopes.y, dt, 2.0f * tau);
    }
    [[nodiscard]] glm::vec2 slopes() const { return {slopeX.value, slopeY.value}; }
    [[nodiscard]] bool atRest() const {
        return std::abs(heave.value) < 1e-3f && std::abs(heave.velocity) < 1e-3f && std::abs(slopeX.value) < 1e-4f &&
               std::abs(slopeY.value) < 1e-4f;
    }
};

/// The world-space change that carries a body standing at `pivot` onto the
/// waves: raised by `heave` and leant by `tilt` about the pivot.
inline glm::mat4 rideMatrix(const glm::vec3& pivot, float heave, const glm::quat& tilt) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, pivot + glm::vec3(0.0f, 0.0f, heave));
    m = m * glm::mat4_cast(tilt);
    m = glm::translate(m, -pivot);
    return m;
}

/// A long body's place on the waves - a ship's hull - from the surface at
/// its middle, bow, stern and either side (`heightAt(xy)` the surface's
/// height over the flat level there): its heave the mean of the five, its
/// slopes along and across from the ends' and the sides' differences, in
/// the world's X and Y. Waves shorter than the body mostly cancel.
struct BodyOnWaves {
    float heave = 0.0f;
    glm::vec2 slopes{0.0f};
};
template <class HeightAt>
BodyOnWaves bodyOnWaves(glm::vec2 centre, glm::vec2 forward, float halfLength, float halfWidth, HeightAt&& heightAt) {
    const float fl = glm::length(forward);
    const glm::vec2 f = fl > 1e-6f ? forward / fl : glm::vec2(1.0f, 0.0f);
    const glm::vec2 side(-f.y, f.x);
    const float mid = heightAt(centre);
    const float bow = heightAt(centre + f * halfLength);
    const float stern = heightAt(centre - f * halfLength);
    const float left = heightAt(centre + side * halfWidth);
    const float right = heightAt(centre - side * halfWidth);
    BodyOnWaves b;
    b.heave = (mid + bow + stern + left + right) / 5.0f;
    const float along = halfLength > 0.0f ? (bow - stern) / (2.0f * halfLength) : 0.0f;
    const float across = halfWidth > 0.0f ? (left - right) / (2.0f * halfWidth) : 0.0f;
    b.slopes = f * along + side * across;
    return b;
}

/// The short waves' variety (water_waves.glsli's pwVarietyAt, pwShortness,
/// pwDirection): waves shorter than the vertex grid carries come and go in
/// drifting patches, their crests bend, and they spread over more headings.
/// Only waves under kVertexMinWavelength are touched, so what this file rides
/// is the same with it or without. For the tests: the shader alone draws it.
namespace variety {
inline constexpr float kCalm = 0.85f;
inline constexpr float kStorm = 0.35f;
inline constexpr float kPatchScale = 1.0f / 26.0f;
inline constexpr float kPatchDrift = 0.8f;
inline constexpr float kBendScale = 1.0f / 11.0f;
inline constexpr float kBend = 2.2f;
inline constexpr float kShortSpread = 1.6f;
inline float fromDetail(float detailShare) {
    const float calm = std::clamp((1.0f - detailShare) / 0.75f, 0.0f, 1.0f);
    return kStorm + (kCalm - kStorm) * calm;
}
inline float shortness(float k) {
    const float wavelength = 6.2831853f / std::max(k, 1e-4f);
    const float x = std::clamp((wavelength - 0.75f * kVertexMinWavelength) / (0.25f * kVertexMinWavelength), 0.0f, 1.0f);
    return 1.0f - x * x * (3.0f - 2.0f * x);
}
inline float hash(glm::vec2 p) {
    p = glm::fract(p * glm::vec2(0.1031f, 0.1030f));
    p += glm::dot(p, glm::vec2(p.y, p.x) + 33.33f);
    return glm::fract((p.x + p.y) * p.x);
}
inline float noise(const glm::vec2& p) {
    const glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = hash(i), b = hash(i + glm::vec2(1, 0)), c = hash(i + glm::vec2(0, 1)), d = hash(i + glm::vec2(1, 1));
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}
/// Group j's (0-2) bend (phase, radians) and patch height at `pos`.
inline glm::vec2 field(int j, const glm::vec2& pos, float time, float windDir, float v) {
    const glm::vec2 wind(std::cos(windDir), std::sin(windDir));
    const glm::vec2 o(static_cast<float>(j) * 37.17f, static_cast<float>(j) * 59.83f);
    const float bend = (noise(pos * kBendScale + o) - 0.5f) * 2.0f * kBend;
    float t = std::clamp((noise((pos - wind * (time * kPatchDrift)) * kPatchScale + o * 1.7f) - 0.2f) / 0.6f, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return {bend, 1.0f - v + 2.0f * v * t};
}
/// The slope (d height / d x, d / d y) of the sampled waves at `pos`, the
/// short ones with the variety `v` (0: none), as water.frag's normal has it.
inline glm::vec2 slope(const wave_spectrum::Spectrum& sp, const glm::vec2& pos, float time, float windDir, float v) {
    glm::vec2 g(0.0f);
    for (std::size_t i = 0; i < sp.count; ++i) {
        const auto& w = sp.waves[i];
        const float sh = shortness(w.k);
        float spread = (std::fmod(static_cast<float>(i % wave_spectrum::kWaveCount) * 0.6180340f, 1.0f) - 0.5f) *
                       wave_spectrum::kPi * 0.8f;
        float A = w.amplitude, bend = 0.0f;
        if (v > 0.0f) {
            spread *= 1.0f + (kShortSpread - 1.0f) * sh;
            const glm::vec2 f = field(static_cast<int>(i % 3), pos, time, windDir, v);
            A *= 1.0f + (f.y - 1.0f) * sh;
            bend = f.x * sh;
        }
        const glm::vec2 dir(std::cos(windDir + spread), std::sin(windDir + spread));
        const float phase = w.k * glm::dot(dir, pos) - w.omega * time + w.phase + bend;
        g += dir * (w.k * A * std::cos(phase));
    }
    return g;
}
}  // namespace variety

}  // namespace wave_surface
}  // namespace wowee::rendering

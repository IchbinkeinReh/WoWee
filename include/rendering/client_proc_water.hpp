#pragma once

/// Procedural water as the 3.3.5a client draws it: CMaterialProcWater
/// (0x008a48f0) with vsLiquidProcWater and psLiquidProcWater, LiquidType
/// material 3 (0x008a1fa0). What is here is the arithmetic the exe does on the
/// CPU for it: the wave manager a chunk liquid carries (CWaveManager,
/// 0x007d6240 and 0x007d62a0), the ripple constants c46 to c57 it hands the
/// vertex program (0x008a3620, 0x008a3710), the pixel constants c10 and c11
/// from the LiquidType floats (0x008a3810) and the texture matrices.
///
/// Nothing here touches a device, so it can be checked on its own.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::rendering::client_proc_water {

/// The texture units the material binds (0x008a48f0 sets render states 0x15
/// to 0x1a) and the LiquidType texture each takes. Units 0 and 1 are the
/// program's two cube maps, 2 the depth colour, 3 the highlight mask (its
/// alpha), 4 the far normals and 5 the near ones (two maps, RG and AB).
inline constexpr std::array<int, 6> kUnitTexture{0, 1, 4, 5, 2, 3};

/// Which LiquidType int a texture's frames run over, or -1 for the fixed
/// period 0x4e2 (0x008a1d60's second argument): Textures 0, 1 and 4 fixed,
/// 2 over Int[1], 3 over Int[2], 5 over Int[3].
inline constexpr std::array<int, 6> kTexturePeriodInt{-1, -1, 1, 2, -1, 3};

/// The MSVC CRT rand() the wave manager draws from.
class CrtRand {
public:
    explicit CrtRand(uint32_t seed = 1) : state_(seed) {}
    int operator()() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7fff);
    }

private:
    uint32_t state_;
};

/// One plane wave, as the manager keeps it and 0x008a48f0 reads it: eight
/// floats - centre, direction, wavelength, falloff, amplitude, speed.
struct Wave {
    glm::vec2 center{0.0f};
    glm::vec2 dir{0.0f};
    float wavelength = 0.0f;
    float falloff = 0.0f;
    float amplitude = 0.0f;
    float speed = 0.0f;
};

/// One circular wave: six floats - centre, wavelength, falloff, amplitude,
/// speed (0x008a3620's arguments in order).
struct CircularWave {
    glm::vec2 center{0.0f};
    float wavelength = 0.0f;
    float falloff = 0.0f;
    float amplitude = 0.0f;
    float speed = 0.0f;
};

/// The fade a wave takes in and out over, in milliseconds (0xaf1694, 0xaf1690).
inline constexpr float kWaveFadeMs = 5000.0f;

/// CWaveManager (0x007d6240): three circular waves, which nothing sets and so
/// stay flat, and three plane waves it respawns ahead of the camera, each for
/// 20 to 40 seconds. The client keeps one for all chunk liquids and steps it
/// once a frame as it draws one (0x007cf9a0); WMO liquids get none.
class WaveManager {
public:
    /// 0x007d62a0: `nowMs` the frame time, `eye` and `target` the camera's
    /// position and look-at point (0xcd8f5c, 0xcd8f68), on the client's axes.
    template <typename Rand>
    void update(uint32_t nowMs, const glm::vec3& eye, const glm::vec3& target, Rand& rand) {
        if (!started_) {
            started_ = true;
            lastMs_ = nowMs;
        }
        const uint32_t dt = nowMs - lastMs_;
        lastMs_ = nowMs;
        for (int i = 0; i < 3; ++i) {
            age_[i] += dt;
            const float in = std::min(static_cast<float>(age_[i]) / kWaveFadeMs, 1.0f);
            float out = 0.0f;
            if (age_[i] < life_[i]) out = std::min(static_cast<float>(life_[i] - age_[i]) / kWaveFadeMs, 1.0f);
            waves_[i].amplitude = std::min(in, out) * master_[i].amplitude;
            if (life_[i] > age_[i]) continue;

            age_[i] = 0;
            life_[i] = static_cast<uint32_t>(rand() % 20000 + 20000);
            // Ahead of the camera, 150 to 200 along its heading (normalised in
            // three dimensions, so a pitched camera puts it nearer), give or
            // take 50 across.
            glm::vec3 fwd = target - eye;
            const float len2 = glm::dot(fwd, fwd);
            if (len2 > 2.3841858e-07f) fwd /= std::sqrt(len2);
            const float alongX = (static_cast<float>(rand() % 50) + 150.0f) * fwd.x;
            const float cx = static_cast<float>(rand() % 100 - 50) + eye.x + alongX;
            const float alongY = (static_cast<float>(rand() % 50) + 150.0f) * fwd.y;
            const float cy = static_cast<float>(rand() % 100 - 50) + alongY + eye.y;
            Wave w;
            w.center = glm::vec2(cx, cy);
            // Running back toward the camera, turned up to 45 degrees.
            glm::vec2 back(eye.x - cx, eye.y - cy);
            const float b2 = glm::dot(back, back);
            if (b2 > 2.3841858e-07f) back /= std::sqrt(b2);
            const float angle = (static_cast<float>(rand() % 2000) - 1000.0f) * 0.0007853982f;
            const float c = std::cos(angle), s = std::sin(angle);
            w.dir = glm::vec2(back.x * c - back.y * s, back.x * s + back.y * c);
            w.wavelength = static_cast<float>(rand() % 1000) * 0.04f + 10.0f;
            w.falloff = static_cast<float>(rand() % 1000) * 0.2f + 200.0f;
            w.amplitude = static_cast<float>(rand() % 1000) * 0.0005f + 0.5f;
            w.speed = static_cast<float>(rand() % 1000) * 3.0000001e-05f + 0.02f;
            master_[i] = w;
            waves_[i] = w;
            waves_[i].amplitude = 0.0f;
        }
    }

    const std::array<Wave, 3>& waves() const { return waves_; }
    const std::array<CircularWave, 3>& circularWaves() const { return circular_; }
    uint32_t lifetime(int i) const { return life_[i]; }

private:
    bool started_ = false;
    uint32_t lastMs_ = 0;
    std::array<uint32_t, 3> age_{};
    std::array<uint32_t, 3> life_{};
    std::array<Wave, 3> master_{};
    std::array<Wave, 3> waves_{};
    std::array<CircularWave, 3> circular_{};
};

/// A wave's phase at `timeMs` (0x008a3620, 0x008a3710): the time times its
/// speed, rounded, kept to 13 bits and taken as 1024ths of a turn.
inline float wavePhase(uint32_t timeMs, float speed) {
    const auto steps = static_cast<uint32_t>(
        std::llrint(static_cast<double>(static_cast<float>(timeMs)) * static_cast<double>(speed)));
    return static_cast<float>(steps & 0x1fff) * 0.0009765625f * 6.2831855f;
}

/// The vertex program's wave constants, c46 to c57 (0x008a48f0 uploads twelve
/// from 0xd44f88).
struct RippleConstants {
    glm::vec4 circlePhase{0.0f};           // c46: the circular waves' phases
    glm::vec4 planePhase{0.0f};            // c47: the plane waves'
    std::array<glm::vec4, 3> circle{};     // c48-c50: centre, 1/wavelength, amplitude
    glm::vec4 circleFalloff{0.0f};         // c51: 1/falloff
    glm::vec4 planeFalloff{0.0f};          // c52: 1/falloff
    std::array<glm::vec4, 3> plane{};      // c53-c55: centre, direction
    glm::vec4 planeFrequency{0.0f};        // c56: 1/wavelength
    glm::vec4 planeAmplitude{0.0f};        // c57
};

/// 0x008a3620 for the circular waves and 0x008a3710 for the plane ones; no
/// wave manager (a WMO liquid) is every argument 0.
inline RippleConstants rippleConstants(uint32_t timeMs, const std::array<CircularWave, 3>& circles,
                                       const std::array<Wave, 3>& planes) {
    RippleConstants r;
    for (int i = 0; i < 3; ++i) {
        const CircularWave& c = circles[i];
        r.circlePhase[i] = wavePhase(timeMs, c.speed);
        r.circle[i] = glm::vec4(c.center, 1.0f / std::max(c.wavelength, 0.001f), c.amplitude);
        // A falloff under 0.001 gives 0x4479ffff, not its reciprocal.
        r.circleFalloff[i] = c.falloff < 0.001f ? 999.99994f : 1.0f / c.falloff;

        const Wave& p = planes[i];
        r.planePhase[i] = wavePhase(timeMs, p.speed);
        r.planeFalloff[i] = 1.0f / std::max(p.falloff, 0.001f);
        r.plane[i] = glm::vec4(p.center, p.dir);
        r.planeFrequency[i] = 1.0f / std::max(p.wavelength, 0.001f);
        r.planeAmplitude[i] = p.amplitude;
    }
    return r;
}

/// The pixel program's c10 and c11 (0x008a3810, from Floats 11 to 17 with
/// Float 11 times 0xb23f64, which is 1): c10 = (1 / F11, F12, F13, F14),
/// c11 = (F17, F15, F16, 0). F11 is the distance the far normals take over
/// across, F13 + F14 x fresnel the reflection's share, F15 + F16 x fresnel
/// the highlight's and F17 x fresnel what the alpha gains.
struct PixelConstants {
    glm::vec4 c10{0.0f};
    glm::vec4 c11{0.0f};
};

inline PixelConstants pixelConstants(const std::array<float, 18>& f) {
    PixelConstants p;
    p.c10 = glm::vec4(1.0f / std::max(f[11], 0.001f), f[12], f[13], f[14]);
    p.c11 = glm::vec4(f[17], f[15], f[16], 0.0f);
    return p;
}

/// The highlight's exponent, c9.w (0x008a3c90 writes 50.0 to 0xd44c84).
inline constexpr float kSpecularPower = 50.0f;

/// A texture matrix's 2x2 as 0x004c3290 then 0x004c1bf0 build it: rotated by
/// the float times 57.29578 radians, scaled by the other. Columns (c, s) and
/// (-s, c) times the scale, as client_liquid::animStageMatrix.
inline glm::mat2 stageMatrix(float rotationFloat, float scaleFloat) {
    const float angle = rotationFloat * 57.29578f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return glm::mat2(glm::vec2(c, s) * scaleFloat, glm::vec2(-s, c) * scaleFloat);
}

/// The surface coordinate's four normal-map matrices (c9 to c24): matrix i
/// rotated by Float[i + 4], scaled by Float[i]. The program samples Texture2
/// through the first and Texture3 through the third and fourth; the second
/// it computes and leaves.
inline std::array<glm::mat2, 4> normalMatrices(const std::array<float, 18>& f) {
    std::array<glm::mat2, 4> m{};
    for (int i = 0; i < 4; ++i) m[i] = stageMatrix(f[i + 4], f[i]);
    return m;
}

/// The highlight mask's matrix (c29 to c32): Float[10] its rotation, Float[9]
/// its scale. The depth coordinate's (c25 to c28) scales v by Float[8].
inline glm::mat2 maskMatrix(const std::array<float, 18>& f) { return stageMatrix(f[10], f[9]); }

}  // namespace wowee::rendering::client_proc_water

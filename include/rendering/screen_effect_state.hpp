#pragma once

// What the client's full-screen effects decide on the CPU, apart from Vulkan:
// which ScreenEffect row is up, the glow's blend and wave, the nether world's
// blur field, the "special" fog's seed noise and fade, and the wave texture.
// ScreenEffects draws from these; Renderer keeps the State.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wowee {
namespace rendering {
namespace screen_effect {

/// ScreenEffect.dbc's Effect column: which full-screen effect object the row
/// switches to (0x004f7020 on the four made by 0x004fabd0).
enum class Kind : uint32_t {
    Glow = 0,         ///< ffxGlow, 0x008bfe80 - also what an unknown row gets
    Death = 1,        ///< ffxDeath, 0x007ea260
    NetherWorld = 2,  ///< ffxNetherWorld, 0x007ea470
    Special = 3,      ///< ffxSpecial, 0x007ea5f0: the propagating fog
};

/// One ScreenEffect.dbc row as 0x004f7020 reads it (+0x8 the effect, +0xc
/// four params, +0x1c the light override slot, +0x20/+0x24 sound).
struct Row {
    uint32_t id = 0;
    Kind kind = Kind::Glow;
    int32_t params[4] = {0, 0, 0, 0};
    uint32_t lightOverride = 0xFFFFFFFFu;  ///< 0..7 a Light param slot, else none (0x007ecec0)
};

/// SPELL_AURA_SCREEN_EFFECT: the aura's EffectMiscValue names the row
/// (0x004f88b0 looks for 0x104).
inline constexpr uint32_t kAuraScreenEffect = 260;
/// The row a ghost gets, and the one PLAYER_FIELD_BYTES2's invisibility glow
/// (byte 3, 0x40) gets (0x004f88b0).
inline constexpr uint32_t kRowDeath = 1;
inline constexpr uint32_t kRowInvisibility = 0x51;
/// The Map.dbc instance type of an arena: a ghost there keeps the world as it
/// is (0x004f88b0 tests 0xbea570, the battlefield's map type, against 4).
inline constexpr uint32_t kInstanceTypeArena = 4;

/// One aura slot's three spell effects, as 0x004f88b0 reads them.
struct AuraEffects {
    uint32_t spellId = 0;
    uint32_t auraType[3] = {0, 0, 0};
    uint32_t miscValue[3] = {0, 0, 0};
};

/// The ScreenEffect row for the player (0x004f88b0): the first screen-effect
/// aura from the last slot back, by effect index; then a ghost (not in an
/// arena) row 1; then PLAYER_FIELD_BYTES2's invisibility glow row 0x51;
/// else 0, the glow.
inline uint32_t chooseRow(const std::vector<AuraEffects>& slots, bool ghost, bool inArena,
                          uint32_t fieldBytes2) {
    for (size_t s = slots.size(); s-- > 0;) {
        const AuraEffects& a = slots[s];
        if (a.spellId == 0) continue;
        for (int i = 0; i < 3; ++i) {
            if (a.auraType[i] == kAuraScreenEffect) return a.miscValue[i];
        }
    }
    if (ghost && !inArena) return kRowDeath;
    if (((fieldBytes2 >> 24) & 0x40u) != 0) return kRowInvisibility;
    return 0;
}

/// How drunk the player is, 0..1 (0x004f7290): PLAYER_BYTES_3's byte 1, or
/// PLAYER_FAKE_INEBRIATION if that is more, a hundredth each, all of it at
/// 100.
inline float drunkAmount(uint32_t playerBytes3, uint32_t fakeInebriation) {
    const int32_t real = static_cast<int32_t>((playerBytes3 >> 8) & 0xFFu);
    const int32_t fake = static_cast<int32_t>(fakeInebriation);
    const int32_t most = real <= fake ? fake : real;
    if (most >= 100) return 1.0f;
    if (fake < real) return static_cast<float>(real) * 0.01f;
    return static_cast<float>(static_cast<uint32_t>(fake) & 0xFFu) * 0.01f;
}

/// A 0..1 value as the byte 0x004f8770 makes of it: the float bits of
/// v * 255 + 512, shifted down 14 - the whole part of v * 255, wrapping past
/// 255.
inline uint8_t clientByte(float v) {
    const float f = v * 255.0f + 512.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return static_cast<uint8_t>(bits >> 14);
}

/// The glow's vertex colour and pass list (0x004f8770 into 0x008bfde0).
struct GlowBlend {
    bool wave = false;  ///< FFXGlowWave in FFXGlow's place
    uint8_t glow = 0;   ///< alpha: LightParams.Glow, the squared blur added
    uint8_t blend = 0;  ///< blue: how far the frame is mixed toward the blur
};

/// With the camera in liquid the wave and a third of the blur (0x54); drunk,
/// the blur by how drunk if that is more. No player, neither.
inline GlowBlend glowBlend(float lightGlow, bool havePlayer, float drunk, bool cameraInLiquid) {
    GlowBlend g;
    g.glow = clientByte(lightGlow);
    if (!havePlayer) return g;
    const uint8_t drunkByte = clientByte(drunk);
    if (cameraInLiquid) {
        g.blend = 0x54;
        g.wave = true;
    }
    if (g.blend < drunkByte) g.blend = drunkByte;
    return g;
}

/// The wave texture's 128 values along one axis (0x008c2920, V8U8): a cubic
/// stand-in for sin(2 pi i / 128), as signed bytes.
inline int8_t waveValue(uint32_t i) {
    const float x = static_cast<float>(i) * 0.0078125f * 6.2831855f * 0.31830987f - 0.5f;
    // 0x005fe800: the nearest whole number (one less at or below zero) and
    // what is left over.
    int32_t whole = static_cast<int32_t>(std::nearbyint(x));
    if (!(x > 0.0f)) whole -= 1;
    const float f = x - static_cast<float>(whole);
    float v = 1.0f - (6.0f - f * 4.0f) * f * f;
    if (whole & 1) v = -v;
    v *= 128.0f;
    if (v < -128.0f) v = -128.0f;
    if (v > 127.0f) v = 127.0f;
    return static_cast<int8_t>(static_cast<int32_t>(v));  // _ftol truncates
}

/// The 128x128 wave texture, RGBA8 signed: R from the column, G from the row
/// (U and V of the client's V8U8).
inline std::vector<uint8_t> waveTexture() {
    constexpr uint32_t kSize = 128;
    std::vector<uint8_t> px(kSize * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            uint8_t* p = &px[(y * kSize + x) * 4];
            p[0] = static_cast<uint8_t>(waveValue(x));
            p[1] = static_cast<uint8_t>(waveValue(y));
            p[2] = 0;
            p[3] = 0;
        }
    }
    return px;
}

/// Where the wave texture is read for a screen uv (0x008c2350's texture
/// matrix): turned 10 degrees, one texture every 128 pixels across and every
/// 128 / 0.88 down, scrolled once every 3.174 s across and 2.805 s down.
struct WaveTransform {
    float m00, m01, m10, m11;  ///< wave.x = m00 u + m01 v + tx, wave.y = m10 u + m11 v + ty
    float tx, ty;
};
inline WaveTransform waveTransform(uint32_t timeMs, uint32_t width, uint32_t height) {
    constexpr float kAngle = 0.17453292f;  // 0x3e32b8c3
    const float c = std::cos(kAngle);
    const float s = std::sin(kAngle);
    const float sx = static_cast<float>(width) * 1.0f * 0.0078125f;     // 0xb24b28
    const float sy = static_cast<float>(height) * 0.88f * 0.0078125f;   // 0xb24b2c
    WaveTransform w{};
    // Row vectors as the client multiplies them: turned, then scaled, then
    // moved.
    w.m00 = c * sx;
    w.m01 = -s * sx;
    w.m10 = s * sy;
    w.m11 = c * sy;
    w.tx = static_cast<float>(timeMs % 3174u) / 3174.0f;  // 0xb24b20
    w.ty = static_cast<float>(timeMs % 2805u) / 2805.0f;  // 0xb24b24
    return w;
}

/// The random walk 0x00464580 draws from, seeded as 0x004c1510 seeds it.
class Random {
public:
    explicit Random(uint32_t seed) {
        state_ = seed;
        taps_ = (seed % 0x3bu) * 0x400u | (seed % 0x3du) * 4u | (seed % 0x35u) * 0x40000u |
                ((seed / 0x2fu) * 0x11u + seed) * 0x4000000u;
    }
    uint32_t next() {
        static constexpr uint32_t kTable[64] = {
            0x9927148e, 0x08c7aafd, 0x1f3ee6d5, 0xda55bbf6, 0x6a4aa075, 0xff97bde8, 0x9fbc9bde, 0x46a18a81,
            0x63e30b6e, 0x5d6c7a76, 0xca69d388, 0x25b947c3, 0x3fa2ab83, 0xba7c41a6, 0x0195ace5, 0xc109cf7e,
            0x717062d9, 0x0205db8d, 0x54ef8724, 0x3037d4c6, 0x7bcb1bd0, 0xecd8e4b8, 0xdcadce49, 0xc494a913,
            0x0dae398f, 0x0edd5218, 0x85f5fa78, 0x6dafd258, 0x3b53b2a4, 0xbe50a551, 0x11f42dfc, 0xf1169848,
            0x663ddf86, 0x2f2e445e, 0x176b0736, 0xb64c298b, 0xe75f89e2, 0xe121a7cd, 0xed65c94d, 0x239ceefe,
            0x04b77d33, 0x402a9a9e, 0xf35b10b3, 0x921c7782, 0x571e4e20, 0x8c067222, 0xfb732c67, 0xbf0ac259,
            0x0cf95c79, 0x68121a28, 0x42193474, 0xf884c0b1, 0x9d15f038, 0x6f3af260, 0x91eb90b4, 0x61357f1d,
            0x5603325a, 0x932bc5a3, 0x434b0f80, 0x3ce0a8f7, 0x2664d196, 0x4fcc45d7, 0xb5e9b0c8, 0xea31d600};
        const auto at = [](int32_t byteOffset) {
            return kTable[(static_cast<uint32_t>(byteOffset) / 4u) & 63u];
        };
        const auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
        int32_t b3 = static_cast<int32_t>(taps_ >> 24) - 4;
        if (b3 < 0) b3 = static_cast<int32_t>(taps_ >> 24) + 0xb8;
        int32_t b2 = static_cast<int32_t>((taps_ >> 16) & 0xFFu) - 0xc;
        if (b2 < 0) b2 = static_cast<int32_t>((taps_ >> 16) & 0xFFu) + 200;
        int32_t b1 = static_cast<int32_t>((taps_ >> 8) & 0xFFu) - 0x18;
        if (b1 < 0) b1 = static_cast<int32_t>((taps_ >> 8) & 0xFFu) + 0xd4;
        int32_t b0 = static_cast<int32_t>(taps_ & 0xFFu) - 0x1c;
        if (b0 < 0) b0 = static_cast<int32_t>(taps_ & 0xFFu) + 0xd8;
        state_ += rol(at(b1), 3) ^ rol(at(b2), 2) ^ at(b0) ^ rol(at(b3), 1);
        taps_ = (((static_cast<uint32_t>(b3) << 8 | static_cast<uint32_t>(b2)) << 8 |
                  static_cast<uint32_t>(b1)) << 8) | static_cast<uint32_t>(b0);
        return state_;
    }
    /// -1..1, as 0x007e9b10 makes a value of one draw: the bits as a float
    /// of 1..2, to -1..3 and back inside (-1, 1).
    float nextSigned() {
        const uint32_t bits = (next() & 0x7FFFFFu) | 0x3F800000u;
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return std::fmod((f - 1.0f) * 2.0f - 1.0f, 1.0f);
    }

private:
    uint32_t state_ = 0;
    uint32_t taps_ = 0;
};

/// ffxNetherWorld's blur field (0x007e8990, 0x007e9b10): a 6x6 grid over the
/// screen, each point a value in (-1, 1) that eases from one random set to the
/// next in two thirds of a second. The blur at a point runs along the angle
/// 3 x value + the camera's angle + half the cosine of a phase turning two
/// radians a second (FFXNetherBlur.bls).
class NetherField {
public:
    static constexpr int kSide = 6;
    static constexpr int kPoints = kSide * kSide;

    NetherField() : random_(0xabcdef01u) {
        for (auto& set : sets_) {
            for (float& v : set) v = random_.nextSigned();
        }
        for (int i = 0; i < kPoints; ++i) values_[i] = sets_[0][i];
    }

    void advance(float dt) {
        phase_ = std::fmod(dt * 2.0f + phase_, 6.2831855f);
        if (t_ > 1.0f) {
            t_ = std::fmod(t_, 1.0f);
            from_ = (from_ + 1) % 3;  // 0x274 <- 0x278 <- 0x27c <- 0x274
            const int fresh = (from_ + 2) % 3;
            for (float& v : sets_[fresh]) v = random_.nextSigned();
        }
        const auto& a = sets_[from_];
        const auto& b = sets_[(from_ + 1) % 3];
        for (int i = 0; i < kPoints; ++i) values_[i] = b[i] * t_ + (1.0f - t_) * a[i];
        t_ += dt * 1.5f;
    }

    /// The values as the grid's vertex colours carry them back to the shader:
    /// round((v + 1) x 127.5) a byte, read as byte / 255 x 2 - 1.
    [[nodiscard]] float pointValue(int i) const {
        const float b = std::nearbyint((values_[i] + 1.0f) * 127.5f);
        return b / 255.0f * 2.0f - 1.0f;
    }
    [[nodiscard]] float phase() const { return phase_; }

    /// The shared angle: the screen direction of the world's x axis (the
    /// view matrix's first row, x and y over z), as an angle 0..2pi, plus
    /// half the phase's cosine.
    [[nodiscard]] float angle(float viewX, float viewY, float viewZ) const {
        float x = viewX, y = viewY;
        if (std::fabs(viewZ) > 0.0001f) {
            x /= viewZ;
            y /= viewZ;
        }
        const float len = std::sqrt(x * x + y * y);
        float a = 0.0f;
        if (len > 0.0f) {
            const float c = x / len;
            a = std::acos(c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c));
            if (y / len < 0.0f) a = 6.2831855f - a;
        }
        return a + std::cos(phase_) * 0.5f;
    }

private:
    Random random_;
    std::array<std::array<float, kPoints>, 3> sets_{};
    std::array<float, kPoints> values_{};
    int from_ = 0;
    float t_ = 0.0f;
    float phase_ = 0.0f;
};

/// The value noise the "special" fog's seed texture is made of (0x009852a0,
/// 0x009854d0, 0x00985580): a hashed lattice, each point smoothed with its
/// eight neighbours, bilinear between points, five octaves.
inline float latticeNoise(int32_t x, int32_t y) {
    const uint32_t base = static_cast<uint32_t>(y) * 0x39u + static_cast<uint32_t>(x);
    const auto one = [](uint32_t n) {
        n = n ^ (n << 13);
        const uint32_t h = ((n * n * 0x3d73u + 0xc0ae5u) * n + 0xd208dd03u) & 0x7FFFFFFFu;
        return 1.0f - static_cast<float>(h) * 9.313226e-10f;
    };
    const float corners = one(base - 0x3au) + one(base - 0x38u) + one(base + 0x38u) + one(base + 0x3au);
    const float sides = one(base - 1u) + one(base + 1u) + one(base - 0x39u) + one(base + 0x39u);
    return one(base) * 0.25f + corners * 0.0625f + sides * 0.125f;
}
inline float smoothNoise(float x, float y) {
    const float fx0 = std::floor(x);
    const float fy0 = std::floor(y);
    const int32_t ix = static_cast<int32_t>(fx0);
    const int32_t iy = static_cast<int32_t>(fy0);
    const float fx = x - fx0;
    const float fy = y - fy0;
    const float s00 = latticeNoise(ix, iy);
    const float s01 = latticeNoise(ix, iy + 1);
    const float s10 = latticeNoise(ix + 1, iy);
    const float s11 = latticeNoise(ix + 1, iy + 1);
    const float a = s00 + (s10 - s00) * fx;
    const float b = s01 + (s11 - s01) * fx;
    return a + (b - a) * fy;
}
inline float fractalNoise(float x, float y, int octaves) {
    float sum = 0.0f;
    float amp = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        const float f = static_cast<float>(1 << i);
        sum = smoothNoise(f * x, y * f) * amp + sum;
        amp *= 0.5f;
    }
    return sum;
}

/// The seed texture (0x007e8e40): 256x256, white, the alpha the noise at a
/// quarter unit a texel, (n + 3) / 4 of 255 - the byte wrapping where n
/// passes 1, as the client's does.
inline constexpr uint32_t kFogNoiseSize = 256;
inline std::vector<uint8_t> fogNoiseTexture() {
    std::vector<uint8_t> px(kFogNoiseSize * kFogNoiseSize * 4);
    const float step = 64.0f / static_cast<float>(kFogNoiseSize);
    for (uint32_t y = 0; y < kFogNoiseSize; ++y) {
        for (uint32_t x = 0; x < kFogNoiseSize; ++x) {
            const float n = fractalNoise(static_cast<float>(x) * step, static_cast<float>(y) * step, 5);
            uint8_t* p = &px[(y * kFogNoiseSize + x) * 4];
            p[0] = p[1] = p[2] = 0xFF;
            p[3] = static_cast<uint8_t>(
                static_cast<int32_t>(std::nearbyint((n + 3.0f) * 0.25f * 255.0f + 0.5f)));
        }
    }
    return px;
}

/// The fog's own target (0x007ea5f0): 256 across, 128 down. The seed is laid
/// on the row three up from the bottom (0x007e92a0's quad from y 3 to 4).
inline constexpr uint32_t kFogWidth = 256;
inline constexpr uint32_t kFogHeight = 128;
inline constexpr uint32_t kFogSeedRow = kFogHeight - 4;

/// What the effects keep from one frame to the next.
class State {
public:
    /// Switch to `row` (nullptr: the glow). The nether world's fade and the
    /// fog's ramp start again when they come up (0x007e8e20 clears the fade
    /// as the effect goes; 0x007e9010 sets the ramp to 3 s).
    void select(const Row* row) {
        const Kind kind = row ? row->kind : Kind::Glow;
        const uint32_t id = row ? row->id : 0;
        if (kind != kind_ || id != rowId_) {
            if (kind_ == Kind::NetherWorld) netherFade_ = 0.0f;
            if (kind == Kind::Special) fogRamp_ = 3.0f;
        }
        kind_ = kind;
        rowId_ = id;
        if (row) std::memcpy(params_, row->params, sizeof(params_));
        else std::memset(params_, 0, sizeof(params_));
    }

    /// One frame of `dt` seconds.
    void advance(float dt) {
        if (kind_ == Kind::NetherWorld) {
            nether_.advance(dt);
            // FFXNetherCombine's strength, up by dt to 0.75 (0x007e8c80).
            if (netherFade_ < 0.75f) {
                netherFade_ += dt;
                if (netherFade_ >= 0.75f) netherFade_ = 0.75f;
            }
        } else if (kind_ == Kind::Special) {
            // 0x007e9670: the ramp counts down from 3 s.
            float ramp = 1.0f;
            if (fogRamp_ > 0.0f) {
                ramp = 1.0f - fogRamp_ * 0.33333334f;
                fogRamp_ -= dt;
                if (fogRamp_ < 0.0f) fogRamp_ = 0.0f;
            }
            fogDesaturate_ = static_cast<float>(params_[2]) * 0.01f * ramp;
            fogBrighten_ = 0.1f * ramp;
            // 0x007e92a0: one row of the noise a frame, 0 to its height.
            fogNoiseRow_ = fogNoiseCounter_ % kFogNoiseSize;
            fogNoiseCounter_ = fogNoiseCounter_ >= kFogNoiseSize ? 0 : fogNoiseCounter_ + 1;
        }
    }

    [[nodiscard]] Kind kind() const { return kind_; }
    [[nodiscard]] const NetherField& nether() const { return nether_; }
    [[nodiscard]] float netherFade() const { return netherFade_; }
    /// Param 0: the seed's colour, 0xAARRGGBB as a material colour.
    [[nodiscard]] uint32_t fogColour() const { return static_cast<uint32_t>(params_[0]); }
    /// Param 1: the fog's alpha lost each frame as it spreads, of 255
    /// (0x007e9010 into FFXPropagateFog's constant).
    [[nodiscard]] float fogDecay() const { return static_cast<float>(params_[1]) * 0.003921569f; }
    [[nodiscard]] float fogDesaturate() const { return fogDesaturate_; }
    [[nodiscard]] float fogBrighten() const { return fogBrighten_; }
    [[nodiscard]] uint32_t fogNoiseRow() const { return fogNoiseRow_; }

private:
    Kind kind_ = Kind::Glow;
    uint32_t rowId_ = 0;
    int32_t params_[4] = {0, 0, 0, 0};
    NetherField nether_;
    float netherFade_ = 0.0f;
    float fogRamp_ = 0.0f;
    float fogDesaturate_ = 0.0f;
    float fogBrighten_ = 0.0f;
    uint32_t fogNoiseCounter_ = 0;
    uint32_t fogNoiseRow_ = 0;
};

/// The fog the nether world puts up (0x004f7020 into 0x007ed870): to 150
/// yards, from 0.7 of that, white while the full-screen effects are on and
/// (0x4c, 0x4c, 0x63) when they are off.
inline constexpr float kNetherFogEnd = 150.0f;
inline constexpr float kNetherFogStartScalar = 0.7f;

}  // namespace screen_effect
}  // namespace rendering
}  // namespace wowee

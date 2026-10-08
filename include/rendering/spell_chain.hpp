#pragma once

/// SpellChainEffects as the client draws them: the beams of Chain
/// Lightning, Drain Life, Mind Flay, Health Funnel and the like.
///
/// A kit's CharProc 0 or 12 (0x007265c0) names a SpellChainEffects row in
/// its ParamZero and hands it to 0x007fc5f0, which makes a LightningObject:
/// a list of nodes (the unit, then its targets) and a bolt from node to node,
/// each a CLightning (Common\Lightning.cpp, 0x009a8c00 to 0x009ab730) - a
/// strip of joints jittered about the line between its ends, waved, arced,
/// flickered and pulsed by the row, drawn facing the camera.
///
/// Pure arithmetic: SpellVisualSystem finds the ends and hands the strips to
/// the renderer.

#include "rendering/client_ribbon.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace wowee::rendering::spell_chain {

/// A SpellChainEffects row (0x008b73b0, 0xb4 bytes in memory).
struct ChainEffect {
    uint32_t id = 0;
    float avgSegLen = 0.0f;           ///< +0x04: yards a joint, without flag 1
    float width = 0.0f;               ///< +0x08: half the strip's width
    float noiseScale = 0.0f;          ///< +0x0c: joint radius per yard of length
    float texCoordScale = 0.0f;       ///< +0x10: the texture's scroll a second
    uint32_t segDuration = 0;         ///< +0x14: ms a bolt shows
    uint32_t segDelay = 0;            ///< +0x18: ms before the first bolt
    std::string texture;              ///< +0x1c
    uint32_t flags = 0;               ///< +0x20, see kFlag*
    uint32_t jointCount = 0;          ///< +0x24: joints, with flag 1
    float jointOffsetRadius = 0.0f;   ///< +0x28
    uint32_t jointsPerMinorJoint = 0;       ///< +0x2c
    uint32_t minorJointsPerMajorJoint = 0;  ///< +0x30
    float minorJointScale = 0.0f;     ///< +0x34
    float majorJointScale = 0.0f;     ///< +0x38
    float jointMoveSpeed = 0.0f;      ///< +0x3c
    float jointSmoothness = 0.0f;     ///< +0x40
    float minDurationBetweenJointJumps = 0.0f;  ///< +0x44 (seconds)
    float maxDurationBetweenJointJumps = 0.0f;  ///< +0x48
    float waveHeight = 0.0f;          ///< +0x4c
    float waveFreq = 0.0f;            ///< +0x50
    float waveSpeed = 0.0f;           ///< +0x54
    float minWaveAngle = 0.0f;        ///< +0x58
    float maxWaveAngle = 0.0f;        ///< +0x5c
    float minWaveSpin = 0.0f;         ///< +0x60
    float maxWaveSpin = 0.0f;         ///< +0x64
    float arcHeight = 0.0f;           ///< +0x68
    float minArcAngle = 0.0f;         ///< +0x6c
    float maxArcAngle = 0.0f;         ///< +0x70
    float minArcSpin = 0.0f;          ///< +0x74
    float maxArcSpin = 0.0f;          ///< +0x78
    float delayBetweenEffects = 0.0f; ///< +0x7c: ms from one bolt to the next
    float minFlickerOnDuration = 0.0f;   ///< +0x80
    float maxFlickerOnDuration = 0.0f;   ///< +0x84
    float minFlickerOffDuration = 0.0f;  ///< +0x88
    float maxFlickerOffDuration = 0.0f;  ///< +0x8c
    float pulseSpeed = 0.0f;          ///< +0x90
    float pulseOnLength = 0.0f;       ///< +0x94
    float pulseFadeLength = 0.0f;     ///< +0x98
    uint8_t alpha = 0;                ///< +0x9c
    uint8_t red = 0;                  ///< +0x9d
    uint8_t green = 0;                ///< +0x9e
    uint8_t blue = 0;                 ///< +0x9f
    uint8_t blendMode = 0;            ///< +0xa0: the Gx blend
    std::string combo;                ///< +0xa4
    /// The Combo string's bytes read four at a time, as 0x007fc5f0 does
    /// (past its end into the string block), each a SpellChainEffects row
    /// after 0x009a8ce0; see comboRow.
    std::array<uint32_t, 12> comboWords{};
    int32_t renderLayer = 0;          ///< +0xa8: 0 to 3, drawn in that order
    float textureLength = 0.0f;       ///< +0xac: yards a texture repeat, flag 0x100
    float wavePhase = 0.0f;           ///< +0xb0: the wave's phase, flag 0x400
};

/// The row's Flags as the lightning reads them.
inline constexpr uint32_t kFlagFixedJointCount = 0x1;    ///< JointCount joints (0x009ab2e0)
inline constexpr uint32_t kFlagMajorMinorJoints = 0x2;   ///< 0x009a9dc0 rather than 0x009a9ca0
inline constexpr uint32_t kFlagJointsJump = 0x4;         ///< joints jump on their timers (0x009ab3b0)
inline constexpr uint32_t kFlagSpiralWave = 0x8;         ///< the wave turns about the line (0x009a9b30)
inline constexpr uint32_t kFlagFlicker = 0x10;           ///< on and off by the flicker times
inline constexpr uint32_t kFlagPulse = 0x20;             ///< only a pulse along it shows (0x009a9490)
inline constexpr uint32_t kFlagPulseRepeats = 0x40;      ///< the pulse starts again at the end
inline constexpr uint32_t kFlagOneSide = 0x80;           ///< one side vector for the whole strip
inline constexpr uint32_t kFlagTextureLength = 0x100;    ///< the texture repeats every TextureLength
inline constexpr uint32_t kFlagTextureFromEnd = 0x200;   ///< and is laid from the far end
inline constexpr uint32_t kFlagFixedWavePhase = 0x400;   ///< the wave starts at WavePhase

namespace detail {
inline uint32_t u32At(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline float f32At(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}
}  // namespace detail

/// 0x009a8ce0: the SpellChainEffects id one of the Combo string's words
/// names - each byte's low bit dropped and the bits gathered as the client
/// gathers them; 0 for none.
constexpr uint32_t comboRow(uint32_t word) {
    const uint32_t v = word & 0xfefefefeu;
    uint32_t a = ((v >> 7) & 0x200000u) | (v & 0x20000000u);
    a = (a >> 3) | (v & 0x2000000u);
    a = (a >> 4) | (v & 0x40000000u);
    a = (a >> 3) | (v & 0x4000000u);
    const uint32_t d = (((v >> 4) & 0xff800000u) | v) & 0xffffffu;
    return (a >> 11) | d;
}
/// The twelve words from `bytes` (the string block from the Combo string's
/// offset; short is read as zeros).
inline std::array<uint32_t, 12> comboWordsAt(const uint8_t* bytes, size_t available) {
    std::array<uint32_t, 12> words{};
    for (size_t w = 0; w < words.size(); ++w) {
        uint32_t v = 0;
        for (size_t b = 0; b < 4; ++b) {
            const size_t at = w * 4 + b;
            if (bytes && at < available) v |= static_cast<uint32_t>(bytes[at]) << (8 * b);
        }
        words[w] = v;
    }
    return words;
}

/// 0x008b73b0: a row from its record. The first 39 columns are four bytes
/// each and the five colour and blend columns one byte, then the Combo
/// string and three more columns; a file that stores the bytes four wide
/// (192-byte records) is read the same way. `stringAt` turns a string
/// column's offset into its text.
template <class StringAt>
std::optional<ChainEffect> parseChainEffect(const uint8_t* rec, uint32_t recordSize, StringAt stringAt) {
    if (!rec || recordSize < 0x9c + 5 + 16) return std::nullopt;
    using detail::f32At;
    using detail::u32At;
    ChainEffect e;
    e.id = u32At(rec + 0x00);
    e.avgSegLen = f32At(rec + 0x04);
    e.width = f32At(rec + 0x08);
    e.noiseScale = f32At(rec + 0x0c);
    e.texCoordScale = f32At(rec + 0x10);
    e.segDuration = u32At(rec + 0x14);
    e.segDelay = u32At(rec + 0x18);
    e.texture = stringAt(u32At(rec + 0x1c));
    e.flags = u32At(rec + 0x20);
    e.jointCount = u32At(rec + 0x24);
    e.jointOffsetRadius = f32At(rec + 0x28);
    e.jointsPerMinorJoint = u32At(rec + 0x2c);
    e.minorJointsPerMajorJoint = u32At(rec + 0x30);
    e.minorJointScale = f32At(rec + 0x34);
    e.majorJointScale = f32At(rec + 0x38);
    e.jointMoveSpeed = f32At(rec + 0x3c);
    e.jointSmoothness = f32At(rec + 0x40);
    e.minDurationBetweenJointJumps = f32At(rec + 0x44);
    e.maxDurationBetweenJointJumps = f32At(rec + 0x48);
    e.waveHeight = f32At(rec + 0x4c);
    e.waveFreq = f32At(rec + 0x50);
    e.waveSpeed = f32At(rec + 0x54);
    e.minWaveAngle = f32At(rec + 0x58);
    e.maxWaveAngle = f32At(rec + 0x5c);
    e.minWaveSpin = f32At(rec + 0x60);
    e.maxWaveSpin = f32At(rec + 0x64);
    e.arcHeight = f32At(rec + 0x68);
    e.minArcAngle = f32At(rec + 0x6c);
    e.maxArcAngle = f32At(rec + 0x70);
    e.minArcSpin = f32At(rec + 0x74);
    e.maxArcSpin = f32At(rec + 0x78);
    e.delayBetweenEffects = f32At(rec + 0x7c);
    e.minFlickerOnDuration = f32At(rec + 0x80);
    e.maxFlickerOnDuration = f32At(rec + 0x84);
    e.minFlickerOffDuration = f32At(rec + 0x88);
    e.maxFlickerOffDuration = f32At(rec + 0x8c);
    e.pulseSpeed = f32At(rec + 0x90);
    e.pulseOnLength = f32At(rec + 0x94);
    e.pulseFadeLength = f32At(rec + 0x98);
    const uint32_t byteStride = recordSize >= 0x9c + 5 * 4 + 16 ? 4u : 1u;
    e.alpha = rec[0x9c + 0 * byteStride];
    e.red = rec[0x9c + 1 * byteStride];
    e.green = rec[0x9c + 2 * byteStride];
    e.blue = rec[0x9c + 3 * byteStride];
    e.blendMode = rec[0x9c + 4 * byteStride];
    e.combo = stringAt(u32At(rec + recordSize - 16));
    e.renderLayer = static_cast<int32_t>(u32At(rec + recordSize - 12));
    e.textureLength = f32At(rec + recordSize - 8);
    e.wavePhase = f32At(rec + recordSize - 4);
    return e;
}

/// The lightning's own random numbers (0x00464580 on 0x00dcecf8): 23 bits
/// of mantissa under the exponent of 1 make a float in [1, 2).
class Rng {
public:
    explicit Rng(uint32_t seed = 0x2545F491u) : state_(seed ? seed : 1u) {}
    uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    /// [0, 1) (0x009a8e80 and the reset's draws).
    float unit() { return mantissa(next()) - 1.0f; }
    /// [-1, 1]: the sign bit picks the half (0x009a90c0).
    float signedUnit() {
        const uint32_t bits = next();
        const float f = mantissa(bits);
        return static_cast<int32_t>(bits) < 0 ? 2.0f - f : f - 2.0f;
    }
    float range(float lo, float hi) { return lo + unit() * (hi - lo); }

private:
    static float mantissa(uint32_t bits) {
        const uint32_t v = (bits & 0x7fffffu) | 0x3f800000u;
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    uint32_t state_;
};

/// 0x005fe800 with the FPU set to chop: the whole part, one less for a value
/// not above 0, and what is left over.
inline void splitTurn(float u, float& frac, int32_t& whole) {
    whole = u > 0.0f ? static_cast<int32_t>(u) : static_cast<int32_t>(u) - 1;
    frac = u - static_cast<float>(whole);
}
/// 0x005fff80: the client's cubic cosine, by half turns.
inline float clientCos(float x) {
    float f;
    int32_t n;
    splitTurn(x * 0.31830987f, f, n);
    const float v = 1.0f - (6.0f - 4.0f * f) * f * f;
    return (n & 1) ? -v : v;
}
/// 0x006f7a10: the same a quarter turn on, a sine.
inline float clientSin(float x) {
    float f;
    int32_t n;
    splitTurn(x * 0.31830987f - 0.5f, f, n);
    const float v = 1.0f - (6.0f - 4.0f * f) * f * f;
    return (n & 1) ? -v : v;
}

/// 0x009a9260: two directions across `dir` - the first level, the second
/// the cross of it with `dir`, over its length.
inline void crossBasis(const glm::vec3& dir, glm::vec3& across, glm::vec3& up) {
    across = glm::vec3(dir.y, 0.0f, 0.0f);
    if (std::fabs(across.x) >= 2.3841858e-07f) {
        across.y = -dir.x;
        const float inv = 1.0f / std::sqrt(across.x * across.x + across.y * across.y);
        across.x *= inv;
        across.y *= inv;
    } else {
        across.y = dir.x <= 0.0f ? 1.0f : -1.0f;
    }
    up = glm::cross(across, dir) * (1.0f / (glm::length(dir) + 1e-8f));
}

/// The wave's and the arc's swell along the strip: none at the ends, all of
/// it in the middle (0x009a9b30, 0x009aa210).
inline float swell(float t) { return 1.0f - (t - 0.5f) * (t - 0.5f) * 4.0f; }

/// 0x009ab2e0: how many joints a strip of `length` has - JointCount with
/// flag 1, else the length over AvgSegLen (chopped) and two - and one more,
/// held to 2..1000.
inline int32_t jointCountFor(const ChainEffect& e, float length) {
    int32_t count;
    if (e.flags & kFlagFixedJointCount) {
        count = static_cast<int32_t>(e.jointCount);
    } else {
        const double q = static_cast<double>(length) / static_cast<double>(e.avgSegLen);
        // fistp qword of what does not fit gives 0x8000000000000000, its low
        // half 0.
        count = std::isfinite(q) && std::fabs(q) < 2147483647.0 ? static_cast<int32_t>(q) : 0;
        count += 2;
    }
    count += 1;
    if (count > 1000) return 1000;
    return count < 2 ? 2 : count;
}

/// 0x009a9490: which vertices a pulse shows and their alpha, `segment`
/// yards apart pair by pair, the pulse's head at `pulse`.
struct PulseSpan {
    uint32_t first = 0;
    uint32_t count = 0;
};
inline PulseSpan pulseAlphas(const ChainEffect& e, float pulse, float segment, uint32_t vertexCount,
                             std::vector<uint8_t>& alphas) {
    alphas.assign(vertexCount, e.alpha);
    if (!(e.flags & kFlagPulse)) return {0, vertexCount};
    const float fadeEnd = pulse - e.pulseFadeLength;
    const float onStart = fadeEnd - e.pulseOnLength;
    const float fadeStart = onStart - e.pulseFadeLength;
    const float a = static_cast<float>(e.alpha) * 0.003921569f;
    uint32_t first = 0;
    float along = 0.0f;
    for (uint32_t k = 0; k < vertexCount; k += 2) {
        uint8_t v;
        if (along < fadeStart) {
            v = 0;
            first = k;
        } else if (along < onStart) {
            v = static_cast<uint8_t>(static_cast<int32_t>((along - fadeStart) * a * (255.0f / (onStart - fadeStart))));
        } else if (along < fadeEnd) {
            v = e.alpha;
        } else if (along < pulse) {
            v = static_cast<uint8_t>(static_cast<int32_t>((pulse - along) * a * (255.0f / (pulse - fadeEnd))));
        } else {
            alphas[k] = 0;
            if (k + 1 < vertexCount) alphas[k + 1] = 0;
            return {first, k + 1 - first};
        }
        alphas[k] = v;
        if (k + 1 < vertexCount) alphas[k + 1] = v;
        along += segment;
    }
    return {first, vertexCount - first};
}

/// 0x009aa210's texture matrix: u times len / TextureLength with flag 0x100,
/// less the random start, the scroll and (flag 0x200 too) len / TextureLength.
struct TexU {
    float scale = 1.0f;
    float offset = 0.0f;
};
inline TexU textureU(const ChainEffect& e, float length, float randomStart, float scroll) {
    TexU t;
    t.offset = randomStart + scroll;
    if ((e.flags & kFlagTextureLength) && e.textureLength != 0.0f) {
        t.scale = length / e.textureLength;
        if (e.flags & kFlagTextureFromEnd) t.offset += t.scale;
    }
    return t;
}

/// The M2 blend the ribbons' pipelines take for the row's Gx blend: 0
/// opaque, 1 alpha key, 2 alpha, 3 add, 4 mod, 5 mod2x and 10 no-alpha add
/// have one; the rest are drawn as alpha.
inline uint16_t m2BlendForGx(uint8_t gx) {
    switch (gx) {
        case 0: return 0;
        case 1: return 1;
        case 3: return 4;
        case 4: return 5;
        case 5: return 6;
        case 10: return 3;
        default: return 2;
    }
}

/// 0x009ab070 and 0x009aa210's state: unfogged, unlit (its emissive is the
/// colour), both faces, tested, written only for Gx blend 2, alpha
/// referenced by the Gx blend (0x00ad8b7c).
inline client_ribbon::MaterialState materialFor(const ChainEffect& e) {
    client_ribbon::MaterialState m;
    m.blend = m2BlendForGx(e.blendMode);
    m.lit = false;
    m.fogged = false;
    m.cull = false;
    m.depthTest = true;
    m.depthWrite = e.blendMode == 2;
    switch (e.blendMode) {
        case 0:
        case 10: m.alphaRef = 0.0f; break;
        case 1: m.alphaRef = 224.0f / 255.0f; break;
        default: m.alphaRef = 1.0f / 255.0f; break;
    }
    return m;
}

/// A CLightning (0xb4 bytes, Common\Lightning.cpp).
class Lightning {
public:
    /// 0x009aafb0 and 0x009a9900: the row, its colour, hidden.
    void init(const ChainEffect* effect, Rng& rng) {
        e_ = effect;
        flags_ = 0;
        joints_.clear();
        reset(rng);
    }
    const ChainEffect* effect() const { return e_; }
    /// 0x009a96a0, 0x009a96c0, 0x009a9710.
    void setEnds(const glm::vec3& start, const glm::vec3& end) {
        start_ = start;
        end_ = end;
    }
    /// 0x009a9770: flag 2.
    void setVisible(bool visible) { flags_ = visible ? (flags_ | 2u) : (flags_ & ~2u); }
    bool visible() const { return (flags_ & 2u) != 0; }
    bool flickeredOff() const { return (flags_ & 4u) != 0; }
    size_t jointCount() const { return joints_.size(); }
    float pulse() const { return pulse_; }
    /// As its last build left them (+0x90 0x10, 0x20).
    bool pulseAtSource() const { return (flags_ & 0x10u) != 0; }
    bool pulseAtEnd() const { return (flags_ & 0x20u) != 0; }

    /// 0x009ab3b0: a frame of `dt` seconds.
    void update(float dt, Rng& rng) {
        if (!e_ || !(flags_ & 2u)) return;
        const ChainEffect& e = *e_;
        flickerTimer_ -= dt;
        if (!(flags_ & 4u)) {
            if ((e.flags & kFlagFlicker) && flickerTimer_ <= 0.0f) {
                flickerTimer_ = rng.range(e.minFlickerOffDuration, e.maxFlickerOffDuration);
                flags_ |= 4u;
            }
        } else {
            if (flickerTimer_ > 0.0f) return;
            reset(rng);
        }
        // 0x0088d93a: the scroll, wrapped.
        scroll_ = e.texCoordScale != 0.0f ? std::fmod(e.texCoordScale * dt + scroll_, 1.0f) : 0.0f;
        const float len = std::min(glm::length(end_ - start_), 10000.0f);
        const float radius = e.noiseScale * len + e.jointOffsetRadius;
        fitJoints(len, radius, rng);
        for (size_t j = 1; j < joints_.size(); ++j) {
            Joint& joint = joints_[j];
            joint.timer -= dt;
            if (!(e.flags & kFlagJointsJump) || joint.timer >= 0.0f) {
                joint.offset += dt * joint.velocity;
            } else {
                randomizeJoint(joint, radius, rng);
            }
        }
        waveAngle_ += waveSpin_ * dt;
        wavePhase_ -= e.waveSpeed * dt;
        arcAngle_ += arcSpin_ * dt;
        if (e.flags & kFlagPulse) {
            pulse_ += dt * e.pulseSpeed;
            if ((flags_ & 8u) || (e.flags & kFlagPulseRepeats)) {
                const float total = 2.0f * e.pulseFadeLength + e.pulseOnLength;
                if (len <= pulse_ - total) {
                    reset(rng);
                    flags_ &= ~8u;
                    return;
                }
                if (pulse_ < 0.0f) {
                    reset(rng);
                    flags_ &= ~8u;
                    pulse_ = total + len;
                }
            }
        }
    }

    /// 0x009aa210: the strip as it faces `camera` this frame, as vertices
    /// for a triangle strip; false when it shows nothing.
    bool build(const glm::vec3& camera, std::vector<client_ribbon::Vertex>& out) {
        out.clear();
        flags_ &= ~0x30u;
        if (!e_ || (flags_ & 6u) != 2u || joints_.size() < 2) return false;
        const ChainEffect& e = *e_;
        const size_t n = joints_.size();
        std::vector<glm::vec3> points(n);
        globalBasis();
        if ((e.flags & kFlagMajorMinorJoints) && e.jointsPerMinorJoint > 0 && e.minorJointsPerMajorJoint > 0)
            placeMajorMinor(points);
        else
            placeSimple(points);

        const bool waved = e.waveHeight != 0.0f;
        const bool arced = e.arcHeight != 0.0f;
        const float len = glm::length(end_ - start_);
        const float invLast = 1.0f / static_cast<float>(n - 1);
        std::vector<glm::vec3> verts(2 * n, glm::vec3(0.0f));
        auto shaped = [&](size_t i, float t) {
            glm::vec3 p = points[i];
            if (waved) p = wave(p, t, len);
            if (arced) p += arcVec_ * swell(t);
            return p;
        };
        if (!(e.flags & kFlagOneSide)) {
            glm::vec3 prev = points[0];
            float t = invLast;
            for (size_t i = 1; i < n; ++i) {
                const glm::vec3 p = shaped(i, t);
                const glm::vec3 d = p - prev;
                const glm::vec3 toEye = (p + prev) * 0.5f - camera;
                glm::vec3 side = glm::cross(d, toEye);
                const float sl = glm::length(side);
                if (sl > 0.001f) side /= sl;
                side *= e.width;
                verts[2 * (i - 1)] += (prev + side) * 0.5f;
                verts[2 * (i - 1) + 1] += (prev - side) * 0.5f;
                verts[2 * i] = (p + side) * 0.5f;
                verts[2 * i + 1] = (p - side) * 0.5f;
                prev = p;
                t += invLast;
            }
        } else {
            const glm::vec3 d = end_ - start_;
            glm::vec3 side = glm::cross(d, (start_ + end_) * 0.5f - camera);
            const float sl = glm::length(side);
            if (sl > 0.001f) side /= sl;
            side *= e.width;
            float t = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                const glm::vec3 p = shaped(i, t);
                verts[2 * i] = p + side;
                verts[2 * i + 1] = p - side;
                t += invLast;
            }
        }
        // The ends pinched to their points.
        verts[0] = verts[1] = points[0];
        verts[2 * n - 2] = verts[2 * n - 1] = points[n - 1];

        std::vector<uint8_t> alphas;
        const float segment = len / static_cast<float>(n - 1);
        const PulseSpan span = pulseAlphas(e, pulse_, segment, static_cast<uint32_t>(2 * n), alphas);
        // +0x90 0x10 and 0x20: the pulse at the first vertex, at the last.
        if (span.first == 0) flags_ |= 0x10u;
        if (span.first + span.count >= 2 * n) flags_ |= 0x20u;
        if (span.count <= 2) return false;
        const TexU tex = textureU(e, len, randomStart_, scroll_);
        const uint32_t rgb = (static_cast<uint32_t>(e.red) << 16) | (static_cast<uint32_t>(e.green) << 8) | e.blue;
        out.reserve(span.count);
        for (uint32_t k = span.first; k < span.first + span.count && k < 2 * n; ++k) {
            // 0x009ab3b0's coordinates: u along the strip, v across it, the
            // ends at the middle of v.
            const uint32_t pair = k / 2;
            float u = static_cast<float>(pair) / static_cast<float>(n - 1);
            float v = (k & 1u) ? 1.0f : 0.0f;
            if (pair == 0 || pair == n - 1) v = 0.5f;
            client_ribbon::Vertex vx;
            vx.position = verts[k];
            vx.color = (static_cast<uint32_t>(alphas[k]) << 24) | rgb;
            vx.uv = glm::vec2(u * tex.scale - tex.offset, v);
            out.push_back(vx);
        }
        return true;
    }

private:
    struct Joint {
        glm::vec3 offset{0.0f};
        glm::vec3 smoothed{0.0f};
        bool smoothedSet = false;
        glm::vec3 velocity{0.0f};
        float timer = 0.0f;
    };

    /// 0x009a8ec0: the wave, arc and flicker afresh, the joints to be laid
    /// again.
    void reset(Rng& rng) {
        const ChainEffect& e = *e_;
        flags_ &= ~5u;
        waveAngle_ = rng.range(e.minWaveAngle, e.maxWaveAngle);
        waveSpin_ = rng.range(e.minWaveSpin, e.maxWaveSpin);
        wavePhase_ = (e.flags & kFlagFixedWavePhase) ? e.wavePhase : rng.unit() * 6.2831855f;
        arcAngle_ = rng.range(e.minArcAngle, e.maxArcAngle);
        arcSpin_ = rng.range(e.minArcSpin, e.maxArcSpin);
        flickerTimer_ = rng.range(e.minFlickerOnDuration, e.maxFlickerOnDuration);
        pulse_ = 0.0f;
        if (e.pulseSpeed < 0.0f) flags_ |= 8u;
        randomStart_ = rng.unit();
    }

    /// 0x009a90c0: a joint somewhere in its radius, drifting.
    void randomizeJoint(Joint& joint, float radius, Rng& rng) {
        const ChainEffect& e = *e_;
        const float r1 = rng.signedUnit();
        const float r2 = rng.signedUnit();
        joint.offset = glm::vec3(r2 * radius, 0.0f, r1 * radius);
        const float r3 = rng.signedUnit();
        const float r4 = rng.signedUnit();
        joint.velocity = glm::vec3(r4 * e.jointMoveSpeed, 0.0f, r3 * e.jointMoveSpeed);
        joint.timer = (flags_ & 1u) ? rng.unit() * e.maxDurationBetweenJointJumps
                                    : rng.range(e.minDurationBetweenJointJumps, e.maxDurationBetweenJointJumps);
    }

    /// 0x009ab2e0: as many joints as the length wants, the new ones (all of
    /// them after a reset) laid afresh.
    void fitJoints(float len, float radius, Rng& rng) {
        const auto count = static_cast<size_t>(jointCountFor(*e_, len));
        const size_t from = (flags_ & 1u) ? joints_.size() : 0;
        joints_.resize(count);
        for (size_t j = from; j < count; ++j) {
            randomizeJoint(joints_[j], radius, rng);
            joints_[j].smoothedSet = false;
        }
        flags_ |= 1u;
    }

    /// 0x009a9980: the line's own directions, and the wave and arc across it.
    void globalBasis() {
        const ChainEffect& e = *e_;
        const glm::vec3 dir = end_ - start_;
        crossBasis(dir, across_, up_);
        waveVec_ = glm::vec3(0.0f);
        if (std::fabs(e.waveHeight) >= 2.3841858e-07f && !(e.flags & kFlagSpiralWave)) {
            waveVec_ = across_ * (clientSin(waveAngle_) * e.waveHeight) + up_ * (clientCos(waveAngle_) * e.waveHeight);
        }
        arcVec_ = glm::vec3(0.0f);
        if (std::fabs(e.arcHeight) >= 2.3841858e-07f) {
            arcVec_ = across_ * (clientSin(arcAngle_) * e.arcHeight) + up_ * (clientCos(arcAngle_) * e.arcHeight);
        }
    }

    /// 0x009a9350: a joint's point - `t` along `dir` from `base`, its offset
    /// (smoothed toward the last) across it at `scale`.
    glm::vec3 jointPoint(Joint& joint, const glm::vec3& base, const glm::vec3& dir, const glm::vec3& a,
                         const glm::vec3& b, float t, float scale) {
        const float s = e_->jointSmoothness;
        const glm::vec3 sm = joint.smoothedSet ? joint.offset * (1.0f - s) + joint.smoothed * s : joint.offset;
        joint.smoothed = sm;
        joint.smoothedSet = true;
        return base + (a * sm.x + b * sm.z) * scale + dir * t;
    }

    /// 0x009a9ca0: every joint about the one line.
    void placeSimple(std::vector<glm::vec3>& points) {
        const size_t last = points.size() - 1;
        points[0] = start_;
        points[last] = end_;
        const glm::vec3 dir = end_ - start_;
        const float inv = 1.0f / static_cast<float>(last);
        for (size_t i = 1; i < last; ++i)
            points[i] = jointPoint(joints_[i], start_, dir, across_, up_, static_cast<float>(i) * inv, 1.0f);
    }

    /// 0x009a9dc0: the major joints about the line at MajorJointScale, the
    /// minor ones about the line between majors at MinorJointScale, the rest
    /// about the line between minors.
    void placeMajorMinor(std::vector<glm::vec3>& points) {
        const ChainEffect& e = *e_;
        const int32_t last = static_cast<int32_t>(points.size()) - 1;
        points[0] = start_;
        points[static_cast<size_t>(last)] = end_;
        const int32_t minor = static_cast<int32_t>(e.jointsPerMinorJoint);
        const int32_t majorStep = static_cast<int32_t>(e.minorJointsPerMajorJoint) * minor;
        const glm::vec3 dirAll = end_ - start_;
        glm::vec3 a, b;
        crossBasis(dirAll, a, b);
        const float invLast = 1.0f / static_cast<float>(last);
        for (int32_t i = majorStep; i < last; i += majorStep)
            points[i] = jointPoint(joints_[i], start_, dirAll, a, b, static_cast<float>(i) * invLast, e.majorJointScale);

        // Between the majors.
        int32_t next = std::min(majorStep, last);
        glm::vec3 base = points[0];
        glm::vec3 dir = points[next] - base;
        crossBasis(dir, a, b);
        float step = static_cast<float>(minor) / static_cast<float>(next);
        float t = step;
        int32_t firstMinor = last;
        for (int32_t j = minor; j < last; j += minor) {
            if (j == next) {
                const int32_t after = std::min(j + majorStep, last);
                base = points[j];
                dir = points[after] - base;
                crossBasis(dir, a, b);
                step = static_cast<float>(minor) / static_cast<float>(after - j);
                t = step;
                next = after;
            } else {
                points[j] = jointPoint(joints_[j], base, dir, a, b, t, e.minorJointScale);
                t += step;
            }
            firstMinor = minor;
        }

        // Between the minors.
        next = firstMinor;
        base = points[0];
        dir = points[next] - base;
        crossBasis(dir, a, b);
        step = 1.0f / static_cast<float>(next);
        t = step;
        for (int32_t j = 1; j < last; ++j) {
            if (j == next) {
                const int32_t after = std::min(j + minor, last);
                base = points[j];
                dir = points[after] - base;
                crossBasis(dir, a, b);
                step = 1.0f / static_cast<float>(after - j);
                t = step;
                next = after;
            } else {
                points[j] = jointPoint(joints_[j], base, dir, a, b, t, 1.0f);
                t += step;
            }
        }
    }

    /// 0x009a9b30: the wave at `t` along a strip `len` long.
    glm::vec3 wave(const glm::vec3& p, float t, float len) const {
        const ChainEffect& e = *e_;
        const float env = swell(t);
        const float phase = e.waveFreq * t * len + wavePhase_;
        if (e.flags & kFlagSpiralWave) {
            const float amp = e.waveHeight * env;
            return p + across_ * (clientCos(phase) * amp) + up_ * (clientSin(phase) * amp);
        }
        return p + waveVec_ * (std::sin(phase) * env);
    }

    const ChainEffect* e_ = nullptr;
    uint32_t flags_ = 0;  ///< +0x90: 1 joints laid, 2 shown, 4 flickered off, 8 pulse backward,
                          ///< 0x10 pulse at the first vertex, 0x20 at the last
    glm::vec3 start_{0.0f};
    glm::vec3 end_{0.0f};
    std::vector<Joint> joints_;
    glm::vec3 across_{0.0f}, up_{0.0f}, waveVec_{0.0f}, arcVec_{0.0f};
    float scroll_ = 0.0f;        ///< +0x80
    float waveAngle_ = 0.0f;     ///< +0x94
    float waveSpin_ = 0.0f;      ///< +0x98
    float wavePhase_ = 0.0f;     ///< +0x9c
    float arcAngle_ = 0.0f;      ///< +0xa0
    float arcSpin_ = 0.0f;       ///< +0xa4
    float flickerTimer_ = 0.0f;  ///< +0xa8
    float pulse_ = 0.0f;         ///< +0xac
    float randomStart_ = 0.0f;   ///< +0xb0
};

/// A bolt of a LightningObject (0x14 bytes): from one node to another, shown
/// from `startMs` to `endMs` (0x007fa4d0).
struct Bolt {
    uint16_t from = 0;
    uint16_t to = 0;
    uint32_t startMs = 0;
    uint32_t endMs = 0;
    /// +0x10: the first bolt's carries the unit's chain counter (0x007265c0's
    /// +0xf58), which names the kits waiting at a place for its pulse; the
    /// rest -1.
    int32_t counter = -1;
};

/// 0x007fa4d0: node 0 is the unit and nodes 1.. its targets in order; a bolt
/// to each target from the one before it - Chain Lightning's hops - or, with
/// `fromFirst` (the kit's ParamThree), all from the unit. The first starts
/// SegDelay after `nowMs` and each DelayBetweenEffects (chopped) after the
/// one before, each showing SegDuration. `objectEndMs` is when the last ends.
inline std::vector<Bolt> planBolts(const ChainEffect& e, size_t targetCount, bool fromFirst, uint32_t nowMs,
                                   uint32_t& objectEndMs, int32_t counter = -1) {
    std::vector<Bolt> bolts;
    uint32_t at = nowMs + e.segDelay;
    objectEndMs = at;
    const auto gap = static_cast<uint32_t>(static_cast<int32_t>(e.delayBetweenEffects));
    for (size_t i = 0; i < targetCount; ++i) {
        Bolt b;
        b.from = fromFirst ? 0 : static_cast<uint16_t>(i);
        b.to = static_cast<uint16_t>(i + 1);
        b.startMs = at;
        b.endMs = at + e.segDuration;
        b.counter = i == 0 ? counter : -1;
        if (static_cast<int32_t>(b.endMs - objectEndMs) >= 0) objectEndMs = b.endMs;
        bolts.push_back(b);
        at += gap;
    }
    return bolts;
}

/// 0x007fae90 after a bolt's lightning was last laid out (0x009aa210 sets
/// +0x90 0x10 where the pulse shows the first vertex, 0x20 where it shows
/// the last): whose waiting kits for the spell play (the unit's virtual
/// 0xc0, 0x00722760) - the source's where the pulse is at it; the target's
/// where it has reached the far end, or, the far end a place, the source's
/// that wait on the bolt's counter.
struct PulseRelease {
    bool source = false;          ///< the source's, counter -1
    bool target = false;          ///< the target object's, counter -1
    bool sourceCounter = false;   ///< the source's, the bolt's counter
};
constexpr PulseRelease pulseRelease(bool pulseAtSource, bool pulseAtEnd, bool endIsObject) {
    PulseRelease r;
    r.source = pulseAtSource;
    r.target = pulseAtEnd && endIsObject;
    r.sourceCounter = pulseAtEnd && !endIsObject;
    return r;
}

/// 0x007fae90: whether a bolt shows at `nowMs` - always while its object is
/// held by its effect (ParamTwo), else from its start to its end.
constexpr bool boltShows(const Bolt& b, bool held, uint32_t nowMs) {
    return held || (static_cast<int32_t>(nowMs - b.startMs) >= 0 && static_cast<int32_t>(nowMs - b.endMs) < 0);
}

/// Where a kit's chain goes (0x007265c0 cases 0 and 12), in the client's
/// order of asking.
enum class ChainTargets {
    FromOther,  ///< kit flag 0x1000 with another unit named: from it to this one
    Place,      ///< the unit's cast at a place (+0xa30 0x800000): to the place
    Channel,    ///< its channel's object, for the channelled spell with at most one hit
    Hits,       ///< its last cast's hit targets (+0xb64)
    None,       ///< nothing to draw to
};
constexpr ChainTargets chainTargets(bool otherSource, bool hasPlace, bool channelMatches, size_t hitCount) {
    if (otherSource) return ChainTargets::FromOther;
    if (hasPlace) return ChainTargets::Place;
    if (channelMatches && hitCount <= 1) return ChainTargets::Channel;
    return hitCount != 0 ? ChainTargets::Hits : ChainTargets::None;
}

/// 0x007faa40 and 0x007fabf0: a unit's middle - its feet, raised three
/// quarters of its height.
inline glm::vec3 unitMiddle(const glm::vec3& feet, float height) {
    return feet + glm::vec3(0.0f, 0.0f, height * 0.75f);
}

}  // namespace wowee::rendering::spell_chain

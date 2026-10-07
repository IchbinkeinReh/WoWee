#pragma once

/// How the 3.3.5a client draws a liquid, as data and arithmetic.
///
/// The client's liquids are drawn by one of three material classes, picked by
/// the LiquidType row's LiquidMaterial id (0x008a1fa0): 1 water, 2 magma (lava
/// and slime), 3 procedural water. Each has a shader path (water 0x008a5590
/// with vsLiquidWater/psLiquidWater, magma 0x008a6090 with vsLiquidMagma/
/// psLiquidMagma, procedural water 0x008a48f0) and a fixed-function stand-in
/// (0x008a5c70, 0x008a6350, 0x008a5170) that binds the same slots, matrices
/// and textures. All three are drawn here by their .bls programs (see
/// liquid.vert.glsl and liquid_proc.frag.glsl, client_proc_water.hpp). What is
/// here is the setup both paths share and the data it reads: the LiquidType
/// row's six textures, eighteen floats and four ints (copied by 0x008a27c0),
/// the vertex a chunk or a WMO liquid builds (0x007ce390, 0x007a7b00), and the
/// procedural colour ramps built from the light (0x008a2bf0, 0x008a2ac0).
///
/// Nothing here touches a device, so it can be checked on its own.

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace wowee::rendering::client_liquid {

/// LiquidType.dbc's columns the liquid code reads (0x008a27c0): MaterialID at
/// +0x38, Texture[6] from +0x3c, Float[18] from +0x5c, Int[4] from +0xa4.
struct LiquidTypeRecord {
    uint32_t id = 0;
    uint32_t flags = 0;
    uint32_t materialId = 0;
    std::array<std::string, 6> textures{};
    std::array<float, 18> floats{};
    std::array<uint32_t, 4> ints{};
};

/// LiquidMaterial.dbc: the vertex format a liquid of this material carries
/// (0x0079b870 reads +0x4) and its flags (0x008a27c0 keeps +0x8 & 1).
struct LiquidMaterialRecord {
    uint32_t id = 0;
    int32_t lvf = 0;
    uint32_t flags = 0;
};

/// The material class a LiquidMaterial id picks (0x008a1fa0). Anything else
/// gets no material there, and nothing is drawn.
enum class MaterialKind : uint8_t { None, Water, Magma, ProcWater };

inline MaterialKind materialKind(uint32_t materialId) {
    switch (materialId) {
        case 1: return MaterialKind::Water;
        case 2: return MaterialKind::Magma;
        case 3: return MaterialKind::ProcWater;
        default: return MaterialKind::None;
    }
}

/// The period a texture slot not tied to a LiquidType int is animated over,
/// in milliseconds: the 0x4e2 each fixed-function material passes to 0x008a1d60.
inline constexpr uint32_t kFixedSlotPeriodMs = 1250;

/// The highest frame number a "%d" texture name is loaded with: frames 1 to
/// 30, the loop in 0x008a2450 running while the number is below 0x1f.
inline constexpr int kMaxAnimFrames = 30;

/// Which texture slots, ints and floats a water material reads.
///
/// Texture0 is the depth slot, sampled at the depth coordinate (attribute 7)
/// with its v scaled by `depthScale`, and modulated by the lit vertex colour.
/// Texture1 is the animated slot, sampled at the surface coordinate
/// (attribute 6) rotated and scaled, and added; the alpha is the depth
/// texture's. psLiquidWater adds the specular highlight on top, gated by the
/// animated texture's alpha; the stand-in's colour op 2 ADD and alpha op 3
/// SELECTARG2 (0x00a2f9cc) are the same sum without it.
struct WaterStages {
    int depthSlot;        ///< texture slot, animated over kFixedSlotPeriodMs
    int animSlot;         ///< texture slot, animated over ints[animPeriodInt] ms
    int animPeriodInt;
    int depthScaleFloat;  ///< floats[] index: the depth coordinate's v scale
    int scaleFloat;       ///< floats[] index: the surface coordinate's scale
    int rotationFloat;    ///< floats[] index: its rotation, times 57.29578
};

/// Water (0x008a5c70): slots 1 and 0, Int[1], Float[2], Float[0], Float[1].
inline constexpr WaterStages kWaterStages{1, 0, 1, 2, 0, 1};
/// Procedural water (0x008a5170): slots 4 and 5, Int[2], Float[8], Float[9], Float[10].
inline constexpr WaterStages kProcWaterStages{4, 5, 2, 8, 9, 10};

/// Magma (0x008a6350) reads slot 0, over the fixed period, its coordinate
/// scrolled by Float[0] and Float[1] cycles a second. It sets lighting off.
inline constexpr int kMagmaSlot = 0;

/// The frame of an animated slot to draw at `timeMs` (0x008a1d60): the
/// period's elapsed fraction times the frame count, less a half (0xb23f74),
/// rounded to nearest.
inline uint32_t animFrameIndex(uint32_t timeMs, uint32_t periodMs, uint32_t frameCount) {
    if (frameCount <= 1) return 0;
    if (periodMs == 0) periodMs = 1;
    const float phase = static_cast<float>(timeMs % periodMs) / static_cast<float>(periodMs);
    const float f = static_cast<float>(frameCount) * phase - 0.5f;
    long idx = std::lrint(f);
    if (idx < 0) idx = 0;
    if (idx >= static_cast<long>(frameCount)) idx = static_cast<long>(frameCount) - 1;
    return static_cast<uint32_t>(idx);
}

/// The depth coordinate a vertex's depth byte gives (0x0079e3c0 builds the two
/// tables, 0x0079b870 picks one by the LiquidType's Int[0]). Table 0 runs over
/// 42 depth units, d/9 x 0.21428572 to a cap of 1 past d/9 = 4.6666665; table 1
/// over all 255, d x 0.5833333 / (0.5833333 x 255) capped at 1.
inline float depthCoord(uint8_t depth, uint32_t table) {
    const float d = static_cast<float>(depth);
    if (table == 0) {
        const float ninths = d * 0.11111111f;
        return ninths <= 4.6666665f ? ninths * 0.21428572f : 1.0f;
    }
    if (table == 1) {
        constexpr float k = 0.5833333f;  // 0xadf800
        const float v = d * k * (1.0f / (k * 255.0f));
        return v >= 1.0f ? 1.0f : v;
    }
    return 0.0f;
}

/// Whether a liquid gets a depth coordinate at all: only when its material's
/// vertex format carries depth, LVF 0 or 2 (0x0079b870), and its Int[0] names
/// one of the two tables.
inline bool hasDepthCoord(int32_t lvf, uint32_t table) {
    return (lvf == 0 || lvf == 2) && table <= 1;
}

/// A chunk liquid's surface coordinate (0x007ce390): its world position times
/// 0.06 on the client's own axes - x north, y west. Render space has the two
/// the other way round.
inline glm::vec2 chunkSurfaceCoord(const glm::vec3& renderPos) {
    return glm::vec2(renderPos.y * 0.060000002f, renderPos.x * 0.060000002f);
}

/// A chunk liquid's stored coordinate, for a material whose vertex format is 1
/// (0x007ce390): the raw ushorts times 0.01171875.
inline glm::vec2 chunkStoredCoord(uint16_t s, uint16_t t) {
    return glm::vec2(static_cast<float>(s) * 0.01171875f, static_cast<float>(t) * 0.01171875f);
}

/// A WMO liquid's surface coordinate (0x007a7b00): its offset from the
/// liquid's corner in the group's own space, times 0.24 - one repeat a tile.
inline glm::vec2 wmoSurfaceCoord(int column, int row) {
    constexpr float kTile = 4.1666665f;
    return glm::vec2(static_cast<float>(column) * kTile * 0.24000001f,
                     static_cast<float>(row) * kTile * 0.24000001f);
}

/// A WMO liquid's stored coordinate, for vertex format 1 (0x007a7b00): the
/// vertex's two shorts over 256.
inline glm::vec2 wmoStoredCoord(int16_t s, int16_t t) {
    return glm::vec2(static_cast<float>(s) * 0.00390625f, static_cast<float>(t) * 0.00390625f);
}

/// The surface coordinate's texture matrix on the animated stage (0x004c3290
/// then 0x004c1bf0): a rotation by `angle` radians - which the client gets
/// by multiplying the float by 57.29578 - scaled by `scale`. Returned as the
/// 2x2 that takes (u, v) to (u', v'), the row vector times the matrix.
inline glm::mat2 animStageMatrix(float rotationFloat, float scaleFloat) {
    const float angle = rotationFloat * 57.29578f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    // Row-vector (u, v) x [[c, s], [-s, c]]: u' = u c - v s, v' = u s + v c,
    // which as a glm matrix times a column vector has columns (c, s), (-s, c).
    return glm::mat2(glm::vec2(c, s) * scaleFloat, glm::vec2(-s, c) * scaleFloat);
}

/// Magma's scroll at `timeMs` (0x008a34b0): each axis advances one whole
/// repeat every round(1000 / speed) milliseconds; 0 holds it still.
inline glm::vec2 magmaScroll(uint32_t timeMs, float speedU, float speedV) {
    auto axis = [timeMs](float speed) {
        if (speed == 0.0f) return 0.0f;
        long period = std::lrint(1000.0f / speed);
        if (period == 0) return 0.0f;
        const uint32_t p = static_cast<uint32_t>(period);
        return static_cast<float>(timeMs % p) / static_cast<float>(p);
    };
    return glm::vec2(axis(speedU), axis(speedV));
}

/// One ramp row, 0-255 a channel.
struct RampTexel {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const RampTexel&) const = default;
};

/// The rows of the procedural depth textures: 64, from the shallows at row 0
/// to the deep at 63 (0x008a2e20 makes them 64 high).
inline constexpr int kRampRows = 64;

namespace detail {
/// One channel of a ramp row, in the 8.8 steps 0x008a2bf0 accumulates:
/// the difference times 256, shifted down six - a sixty-fourth a row.
inline uint8_t rampChannel(uint8_t from, uint8_t to, int row) {
    const int step = ((static_cast<int>(to) - static_cast<int>(from)) * 0x100) >> 6;
    const int acc = (static_cast<int>(from) << 8) + row * step;
    return static_cast<uint8_t>((acc >> 8) & 0xff);
}
}  // namespace detail

/// A liquid colour, as the light hands it over: 0-1 floats, packed to bytes
/// the way the client keeps them.
inline uint8_t toByte(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(std::lrint(c * 255.0f));
}

/// proceduralRiverDepthTex or proceduralOceanDepthTex (0x008a2bf0): the light's
/// close colour and alpha at row 0 stepping to its far ones, a 64th a row. The
/// ocean's last row is its colour's value taken down a tenth (to HSV and back,
/// 0x00984f60 and 0x00985030) and fully opaque (0x009851a0 writes alpha 0xff).
inline std::array<RampTexel, kRampRows> depthRamp(const glm::vec3& close, float closeAlpha,
                                                  const glm::vec3& farColor, float farAlpha,
                                                  bool ocean) {
    std::array<RampTexel, kRampRows> rows{};
    const uint8_t c0[3] = {toByte(close.r), toByte(close.g), toByte(close.b)};
    const uint8_t c1[3] = {toByte(farColor.r), toByte(farColor.g), toByte(farColor.b)};
    const uint8_t a0 = toByte(closeAlpha);
    const uint8_t a1 = toByte(farAlpha);
    for (int row = 0; row < kRampRows; ++row) {
        RampTexel t;
        t.r = detail::rampChannel(c0[0], c1[0], row);
        t.g = detail::rampChannel(c0[1], c1[1], row);
        t.b = detail::rampChannel(c0[2], c1[2], row);
        t.a = detail::rampChannel(a0, a1, row);
        if (ocean && row == kRampRows - 1) {
            // Scaling HSV's value scales all three channels alike.
            auto dim = [](uint8_t c) {
                return static_cast<uint8_t>(
                    std::lrint(static_cast<float>(c) * 0.003921569f * 0.9f * 255.0f));
            };
            t.r = dim(t.r);
            t.g = dim(t.g);
            t.b = dim(t.b);
            t.a = 0xff;
        }
        rows[row] = t;
    }
    return rows;
}

/// proceduralWmoWaterTex (0x008a2ac0): eight texels a row, the first four the
/// river's far colour and the last four white, both at the river's alpha
/// stepping from shallow to deep. Returned per row as {coloured, white}.
inline std::array<std::array<RampTexel, 2>, kRampRows> wmoWaterRamp(const glm::vec3& riverFar,
                                                                    float shallowAlpha,
                                                                    float deepAlpha) {
    std::array<std::array<RampTexel, 2>, kRampRows> rows{};
    const uint8_t a0 = toByte(shallowAlpha);
    const uint8_t a1 = toByte(deepAlpha);
    for (int row = 0; row < kRampRows; ++row) {
        const uint8_t a = detail::rampChannel(a0, a1, row);
        rows[row][0] = RampTexel{toByte(riverFar.r), toByte(riverFar.g), toByte(riverFar.b), a};
        rows[row][1] = RampTexel{0xff, 0xff, 0xff, a};
    }
    return rows;
}

/// The procedural textures a LiquidType names instead of a file. A name with
/// "procedural" in it is looked up among these (0x008a2450, 0x004b6f30);
/// one that is none of them gets the client's 1x1 stand-in.
enum class ProceduralTex : uint8_t { None, River, Ocean, WmoWater, Unknown };

inline ProceduralTex proceduralTexFor(const std::string& name) {
    if (name.find("procedural") == std::string::npos) return ProceduralTex::None;
    if (name == "proceduralRiverDepthTex") return ProceduralTex::River;
    if (name == "proceduralOceanDepthTex") return ProceduralTex::Ocean;
    if (name == "proceduralWmoWaterTex") return ProceduralTex::WmoWater;
    return ProceduralTex::Unknown;
}

/// The stand-in a "procedural" name the client has not registered gets
/// (0x008a2450): one texel, bytes 0, ff, 0, ff as B, G, R, A - opaque green.
inline constexpr RampTexel kMissingProceduralTexel{0x00, 0xff, 0x00, 0xff};

/// Whether a texture name is a frame sequence: one with "%d" in it is loaded as
/// frames 1 to 30 (0x008a2450).
inline bool isFrameSequence(const std::string& name) {
    return name.find("%d") != std::string::npos;
}

/// Frame `n`'s file name of a sequence.
inline std::string frameName(const std::string& pattern, int n) {
    std::string out;
    const size_t at = pattern.find("%d");
    if (at == std::string::npos) return pattern;
    out = pattern.substr(0, at);
    out += std::to_string(n);
    out += pattern.substr(at + 2);
    return out;
}

/// A WMO liquid drawn the interior way (0x00793d20): when its group is neither
/// exterior nor exterior-lit (flags & 0x48 clear) and its LiquidType does not
/// have flag 0x200. It is then lit by a fixed white light (0x007d4f40's second
/// branch), its vertices take the colour of its MOMT material (+0x1c, the
/// diffuse colour), its depth coordinate's u is 1 - the white half of
/// proceduralWmoWaterTex - and a water of a basic type at or below 20 becomes
/// type 17.
inline bool wmoLiquidIsInterior(uint32_t groupFlags, uint32_t liquidTypeFlags) {
    return (groupFlags & 0x48) == 0 && (liquidTypeFlags & 0x200) == 0;
}

/// The type an interior WMO water is drawn as (0x00793d20): one of the water
/// types 1, 5, 9, 13, 17 becomes 17.
inline uint32_t wmoInteriorLiquidType(uint32_t liquidType) {
    if (liquidType != 0 && liquidType < 0x15 && ((liquidType - 1) & 3) == 0) return 0x11;
    return liquidType;
}

}  // namespace wowee::rendering::client_liquid

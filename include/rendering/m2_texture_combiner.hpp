#pragma once

/// How an M2 batch combines its textures, as the 3.3.5a client works it out.
///
/// A batch names up to two textures. The client folds its material's blend
/// mode, the model's combiner table and each texture's coordinate source into
/// one shader id per batch when the skin loads (0x00836980), and picks the
/// vertex and pixel programs by that id (0x00836600, 0x00836c90). The pixel
/// programs (Combiners_*.bls) apply each stage to the running colour, the
/// vertex colour first: Opaque multiplies the colour and keeps the alpha, Mod
/// and Mod2x multiply both, Add adds both, the NA forms keep the alpha, Decal
/// and Fade blend by the vertex alpha. That is the same as the exe's
/// fixed-function stand-in for each (0x008728c0, tables 0x00af5a08 colour and
/// 0x00af59e8 alpha), which is what the tables below hold. The three 0x8000
/// programs are not stage-by-stage and have modes of their own.

#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// Where a stage's texture coordinates come from.
enum class M2TexCoordSource : uint8_t {
    UV0 = 0,  ///< the vertex's first UV set ("T1")
    UV1 = 1,  ///< its second ("T2")
    Env = 2,  ///< the sphere/reflection map ("Env")
    /// The first UV set through no matrix: Diffuse_Env_T2's second stage
    /// reads attrib 6 through c6/c7, the slot the env stage left identity
    /// (0x0081f450 calls 0x00873550 for it).
    UV0Raw = 3,
};

/// A combiner mode, as a shader id's nibbles carry it (0x00836600).
enum M2CombinerMode : uint8_t {
    M2_COMBINE_OPAQUE = 0,
    M2_COMBINE_MOD = 1,
    M2_COMBINE_DECAL = 2,
    M2_COMBINE_ADD = 3,
    M2_COMBINE_MOD2X = 4,
    M2_COMBINE_FADE = 5,
    M2_COMBINE_MOD2X_NA = 6,
    M2_COMBINE_ADD_NA = 7,
    /// The second stage of the three 0x8000 ids (0x00836c90), each a pixel
    /// program of its own over Diffuse_T1_Env:
    M2_COMBINE_MOD2X_NA_ALPHA = 8,   ///< Combiners_Opaque_Mod2xNA_Alpha
    M2_COMBINE_ADD_ALPHA = 9,        ///< Combiners_Opaque_AddAlpha
    M2_COMBINE_ADD_ALPHA_ALPHA = 10, ///< Combiners_Opaque_AddAlpha_Alpha
};

/// The device's combine ops (0x00a2f9cc maps them to D3DTOP, with arguments
/// from 0x00a2f9e4): 0 MODULATE(tex, cur), 1 MODULATE2X(tex, cur),
/// 2 ADD(tex, cur), 3 SELECTARG2 (cur), 4 BLENDCURRENTALPHA(cur, tex),
/// 5 BLENDDIFFUSEALPHA(tex, cur).
enum M2CombineOp : uint8_t {
    M2_OP_MOD = 0,
    M2_OP_MOD2X = 1,
    M2_OP_ADD = 2,
    M2_OP_PASS = 3,
    M2_OP_DECAL = 4,
    M2_OP_FADE = 5,
};

/// Per combiner mode, the stage's colour op (0x00af5a08) and alpha op (0x00af59e8).
inline constexpr uint8_t kM2CombinerColorOp[8] = {0, 0, 4, 2, 1, 5, 1, 2};
inline constexpr uint8_t kM2CombinerAlphaOp[8] = {3, 0, 3, 2, 1, 3, 3, 3};

/// What a batch draws with.
struct M2BatchCombiner {
    bool drawn = true;   ///< false for shader id 0x8000, which gets no shader (0x00836c90)
    uint8_t stages = 1;  ///< 1 or 2 textures
    uint8_t mode[2] = {M2_COMBINE_OPAQUE, M2_COMBINE_OPAQUE};
    M2TexCoordSource source[2] = {M2TexCoordSource::UV0, M2TexCoordSource::UV1};
};

namespace detail {
inline uint16_t m2At(const std::vector<uint16_t>& v, uint32_t i) {
    return i < v.size() ? v[i] : 0;
}
}  // namespace detail

/// The shader id the client gives a batch when its skin loads (0x00836980).
/// `fileShader` is the batch's shader field as stored, `blendMode` its
/// material's, `coordComboIndex` its first entry in `coordCombos`.
inline uint16_t m2BatchShaderId(uint16_t fileShader, uint16_t blendMode, uint16_t textureCount,
                                uint16_t coordComboIndex, uint32_t globalFlags,
                                const std::vector<uint16_t>& coordCombos,
                                const std::vector<uint16_t>& combinerCombos) {
    if ((fileShader & 0x8000) != 0) return fileShader;
    const uint16_t blended = blendMode != 0 ? 1 : 0;
    if ((globalFlags & 0x8) == 0) {
        const uint16_t coord = detail::m2At(coordCombos, coordComboIndex);
        uint16_t v = blended;
        if (coord > 2) v |= 8;
        uint16_t id = static_cast<uint16_t>(v << 4);
        if (coord == 1) id |= 0x4000;
        return id;
    }
    uint16_t id = 0;
    uint16_t vals[2] = {0, 0};
    for (uint32_t i = 0; i < textureCount; ++i) {
        uint16_t v = detail::m2At(combinerCombos, fileShader + i);
        if (i == 0 && !blended) v = 0;
        const uint16_t coord = detail::m2At(coordCombos, coordComboIndex + i);
        if (coord > 2) v |= 8;
        if (coord == 1 && i + 1 == textureCount) id |= 0x4000;
        if (i < 2) vals[i] = v;
    }
    return static_cast<uint16_t>(id | (vals[0] << 4) | vals[1]);
}

/// The shader a batch's id selects (0x00836c90, 0x00836600), as stages.
/// `firstCoord` is the batch's first coordinate combo entry, which picks T1
/// or T2 for a single texture.
inline M2BatchCombiner m2ResolveCombiner(uint16_t shaderId, uint16_t textureCount,
                                         uint16_t firstCoord) {
    M2BatchCombiner c;
    if ((shaderId & 0x8000) != 0) {
        // 1..3 are Combiners_Opaque_Mod2xNA_Alpha, _AddAlpha and
        // _AddAlpha_Alpha on Diffuse_T1_Env: the first texture on the first
        // UV set, the second sphere-mapped. 0 gets no shader and the batch is
        // not drawn (0x00821e97 skips a null one).
        const uint16_t which = shaderId & 0x7FFF;
        c.drawn = which != 0;
        c.mode[0] = M2_COMBINE_OPAQUE;
        c.source[0] = M2TexCoordSource::UV0;
        c.stages = 1;
        if (which >= 1 && which <= 3 && textureCount >= 2) {
            c.stages = 2;
            c.mode[1] = static_cast<uint8_t>(M2_COMBINE_MOD2X_NA_ALPHA + (which - 1));
            c.source[1] = M2TexCoordSource::Env;
        }
        return c;
    }
    const uint8_t hi = static_cast<uint8_t>((shaderId >> 4) & 0xF);
    const uint8_t lo = static_cast<uint8_t>(shaderId & 0xF);
    if (textureCount <= 1) {
        c.stages = 1;
        c.source[0] = (hi & 8) ? M2TexCoordSource::Env
                    : (firstCoord == 0 ? M2TexCoordSource::UV0 : M2TexCoordSource::UV1);
        switch (hi & 7) {
            case 0: c.mode[0] = M2_COMBINE_OPAQUE; break;
            case 2: c.mode[0] = M2_COMBINE_DECAL; break;
            case 3: c.mode[0] = M2_COMBINE_ADD; break;
            case 4: c.mode[0] = M2_COMBINE_MOD2X; break;
            case 5: c.mode[0] = M2_COMBINE_FADE; break;
            default: c.mode[0] = M2_COMBINE_MOD; break;
        }
        return c;
    }
    c.stages = 2;
    // Diffuse_T1_T2, _T1_Env, _Env_T2 and _Env_Env (0x00836600). Env_T2's
    // second stage is the first UV set, untransformed (see UV0Raw).
    c.source[0] = (hi & 8) ? M2TexCoordSource::Env : M2TexCoordSource::UV0;
    c.source[1] = (lo & 8) ? M2TexCoordSource::Env
                : (hi & 8) ? M2TexCoordSource::UV0Raw : M2TexCoordSource::UV1;
    const uint8_t m0 = hi & 7, m1 = lo & 7;
    auto second = [](uint8_t m) -> uint8_t {
        switch (m) {
            case 0: return M2_COMBINE_OPAQUE;
            case 3: return M2_COMBINE_ADD;
            case 4: return M2_COMBINE_MOD2X;
            case 6: return M2_COMBINE_MOD2X_NA;
            case 7: return M2_COMBINE_ADD_NA;
            default: return M2_COMBINE_MOD;
        }
    };
    if (m0 == 0 || m0 == 1) {
        c.mode[0] = m0;
        c.mode[1] = second(m1);
        return c;
    }
    if (m0 == 3 && m1 == 1) {
        c.mode[0] = M2_COMBINE_ADD;
        c.mode[1] = M2_COMBINE_MOD;
        return c;
    }
    if (m0 == 4 && (m1 == 1 || m1 == 4)) {
        // Mod2x over Mod is drawn as Combiners_Mod_Mod2x.
        c.mode[0] = m1 == 1 ? M2_COMBINE_MOD : M2_COMBINE_MOD2X;
        c.mode[1] = M2_COMBINE_MOD2X;
        return c;
    }
    // No such pair: the client falls back to id 0x11, Combiners_Mod_Mod on
    // Diffuse_T1_T2 (0x00836dbf).
    c.mode[0] = M2_COMBINE_MOD;
    c.mode[1] = M2_COMBINE_MOD;
    c.source[0] = M2TexCoordSource::UV0;
    c.source[1] = M2TexCoordSource::UV1;
    return c;
}

/// The two packed for the shaders: stage count in bits 8-9, the stage modes
/// in bits 0-3 and 4-7 (M2Material.combiners).
inline int32_t m2PackCombinerModes(const M2BatchCombiner& c) {
    return static_cast<int32_t>(c.mode[0] | (c.mode[1] << 4) | (c.stages << 8));
}

/// The coordinate sources packed for the vertex shader: stage 0 in bits 0-1,
/// stage 1 in bits 2-3 (push texCoordSet).
inline int32_t m2PackCoordSources(const M2BatchCombiner& c) {
    return static_cast<int32_t>(static_cast<uint8_t>(c.source[0]) |
                                (static_cast<uint8_t>(c.source[1]) << 2));
}

}  // namespace wowee::rendering

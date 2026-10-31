#pragma once

#include <cstdint>
#include <vector>

namespace wowee::pipeline {

/// A texture the client makes itself - a character's composited skin - put
/// into the GPU's block formats, for the "Texture compression" setting.
///
/// BC1 (DXT1, 8 bytes a 4x4 block) where every texel is opaque, else BC3
/// (DXT5, 16 bytes: the alpha in eight steps between two values, the colour
/// as BC1). The same formats the game's own textures arrive in, at the same
/// cost: an eighth or a quarter of the RGBA8 bytes, and the same kind of
/// loss - the colours of a block lie on a line between two 5:6:5 ends.
///
/// The colour ends are taken along the block's principal axis and refined
/// once by least squares; each texel then takes the nearest of the four
/// colours. Not the best an offline encoder finds, but no further from the
/// source than the shipped textures are from theirs.
struct BCLevels {
    bool alpha = false;                      ///< BC3 when true, BC1 when not
    std::vector<std::vector<uint8_t>> mips;  ///< level 0 first, down to 1x1
};

/// Encodes width x height RGBA8 texels, and a box-filtered mip chain made
/// from them, down to 1x1. withAlpha picks BC3 over BC1.
BCLevels encodeBCWithMips(const uint8_t* rgba, int width, int height, bool withAlpha);

/// One level only.
std::vector<uint8_t> encodeBC(const uint8_t* rgba, int width, int height, bool withAlpha);

}  // namespace wowee::pipeline

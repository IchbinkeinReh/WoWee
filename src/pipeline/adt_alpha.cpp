#include "pipeline/adt_alpha.hpp"

#include <algorithm>

namespace wowee {
namespace pipeline {

namespace {

/// The last row and column of a four-bit map are not data.
///
/// A chunk's alpha map is stored 64 wide and only 63 of those are painted: the
/// file's last row and column carry whatever was left there, and the client
/// fills them from the row and column before. Without it every chunk ends in a
/// strip of something else, and since the strip is on two of the four sides,
/// the boundary between two chunks is a hard line - which is the ground
/// textures not quite lining up, in a grid across the whole world.
///
/// Only the four-bit form, and only for a chunk without flag 0x8000 (0x007b76f0
/// does it, 0x007b75b0 for a chunk with the flag does not). The eight-bit and
/// compressed maps are what a map with "big alpha" carries, and those are
/// painted to the edge.
void fixLastRowAndColumn(std::vector<uint8_t>& alpha) {
    if (alpha.size() < ALPHA_MAP_SIZE) return;
    constexpr size_t kLast = ALPHA_MAP_DIM - 1;
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        alpha[kLast * ALPHA_MAP_DIM + i] = alpha[(kLast - 1) * ALPHA_MAP_DIM + i];
    }
    // After the row, so the corner takes the value the row above it just did.
    for (size_t i = 0; i < ALPHA_MAP_DIM; ++i) {
        alpha[i * ALPHA_MAP_DIM + kLast] = alpha[i * ALPHA_MAP_DIM + kLast - 1];
    }
}

}  // namespace

bool decodeLayerAlpha(const MapChunk& chunk, size_t layerIdx,
                      std::vector<uint8_t>& outAlpha, uint8_t unsetFill) {
    outAlpha.assign(ALPHA_MAP_SIZE, unsetFill);

    if (layerIdx >= chunk.layers.size()) return false;
    const auto& layer = chunk.layers[layerIdx];
    if (!layer.useAlpha() || layer.offsetMCAL >= chunk.alphaMap.size()) return false;

    const size_t offset = layer.offsetMCAL;

    // How much of MCAL belongs to this layer, taken from where the next layer
    // with an alpha map starts rather than from what is left in the blob. The
    // difference decides between the 4-bit and 8-bit forms below, so reading
    // "everything remaining" would misidentify every layer but the last.
    size_t layerSize = chunk.alphaMap.size() - offset;
    for (size_t j = layerIdx + 1; j < chunk.layers.size(); ++j) {
        if (chunk.layers[j].useAlpha()) {
            layerSize = chunk.layers[j].offsetMCAL - offset;
            break;
        }
    }

    // The client decides by the map's flag alone (0x007b9890 picks genformat
    // 2 or 3 by MPHD 0x4); only a chunk whose map is unknown is guessed at.
    const bool eightBit = chunk.bigAlpha == 1;
    const bool fourBit = chunk.bigAlpha == 0;

    if (layer.compressedAlpha() && !fourBit) {
        // The client unpacks a row at a time (0x007b7420, called per row by
        // 0x007b88d0): commands until the row's 64 texels are written, a
        // command's count being its low seven bits as they are - not one
        // more. Reading count + 1 made every run a texel too long, so the
        // stream slid off its commands within a row or two and the rest of
        // the layer was its own command bytes read as alpha: paving that
        // never showed, and a dark net of whatever layer came next. A run
        // that overshoots its row is cut there; the client's next row
        // starts afresh at the next command.
        const auto& src = chunk.alphaMap;
        size_t readPos = offset;
        for (size_t row = 0; row < ALPHA_MAP_DIM && readPos < src.size(); ++row) {
            uint8_t* out = outAlpha.data() + row * ALPHA_MAP_DIM;
            size_t written = 0;
            while (written < ALPHA_MAP_DIM && readPos < src.size()) {
                const uint8_t cmd = src[readPos++];
                const size_t count = cmd & ALPHA_COUNT_MASK;
                if ((cmd & ALPHA_FILL_FLAG) != 0) {
                    if (readPos >= src.size()) break;
                    const uint8_t val = src[readPos++];
                    for (size_t i = 0; i < count && written < ALPHA_MAP_DIM; ++i) {
                        out[written++] = val;
                    }
                } else {
                    // Every byte of a copy is consumed, kept or not.
                    for (size_t i = 0; i < count && readPos < src.size(); ++i, ++readPos) {
                        if (written < ALPHA_MAP_DIM) out[written++] = src[readPos];
                    }
                }
            }
        }
        return true;
    }

    if (!fourBit && (eightBit || layerSize >= ALPHA_MAP_SIZE) &&
        offset + ALPHA_MAP_SIZE <= chunk.alphaMap.size()) {
        std::copy(chunk.alphaMap.begin() + static_cast<std::ptrdiff_t>(offset),
                  chunk.alphaMap.begin() + static_cast<std::ptrdiff_t>(offset + ALPHA_MAP_SIZE),
                  outAlpha.begin());
        return true;
    }

    if (!eightBit && (fourBit || layerSize >= ALPHA_MAP_PACKED) &&
        offset + ALPHA_MAP_PACKED <= chunk.alphaMap.size()) {
        // 4 bits per texel: low nibble first, scaled 0-15 to 0-255 by 17.
        for (size_t i = 0; i < ALPHA_MAP_PACKED; ++i) {
            const uint8_t v = chunk.alphaMap[offset + i];
            outAlpha[i * 2] = static_cast<uint8_t>((v & 0x0F) * 17);
            outAlpha[i * 2 + 1] = static_cast<uint8_t>((v >> 4) * 17);
        }
        // Only without MCNK flag 0x8000: with it the 64th row and column
        // are painted and read as they are (CMapChunk::CreateChunkLayerTex,
        // 0x007b9890, picks 0x007b75b0 over 0x007b76f0 by that flag).
        if ((chunk.flags & 0x8000u) == 0) fixLastRowAndColumn(outAlpha);
        return true;
    }

    return false;
}

size_t alphaTexelIndex(float u, float v) {
    const auto clamp01 = [](float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); };
    // The renderer stretches all 64 texels over the chunk (0x007d06b0), so a
    // point falls on the texel whose 1/64 of it holds the point.
    constexpr float kTexels = static_cast<float>(ALPHA_MAP_DIM);
    const auto x = std::min(static_cast<size_t>(clamp01(u) * kTexels), ALPHA_MAP_DIM - 1);
    const auto y = std::min(static_cast<size_t>(clamp01(v) * kTexels), ALPHA_MAP_DIM - 1);
    return y * ALPHA_MAP_DIM + x;
}

float sampleAlpha(const std::vector<uint8_t>& alpha, float u, float v) {
    if (alpha.size() < ALPHA_MAP_SIZE) return 0.0f;
    const auto clampTo = [](float t, float hi) { return t < 0.0f ? 0.0f : (t > hi ? hi : t); };

    // Texel centres at (i + 0.5) / 64, as the renderer's linear filter sees
    // them, clamped at the edges.
    constexpr float kLast = static_cast<float>(ALPHA_MAP_DIM - 1);
    const float fx = clampTo(u * static_cast<float>(ALPHA_MAP_DIM) - 0.5f, kLast);
    const float fy = clampTo(v * static_cast<float>(ALPHA_MAP_DIM) - 0.5f, kLast);
    const auto x0 = static_cast<size_t>(fx);
    const auto y0 = static_cast<size_t>(fy);
    const size_t x1 = std::min(x0 + 1, ALPHA_MAP_DIM - 1);
    const size_t y1 = std::min(y0 + 1, ALPHA_MAP_DIM - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const float a00 = static_cast<float>(alpha[y0 * ALPHA_MAP_DIM + x0]);
    const float a10 = static_cast<float>(alpha[y0 * ALPHA_MAP_DIM + x1]);
    const float a01 = static_cast<float>(alpha[y1 * ALPHA_MAP_DIM + x0]);
    const float a11 = static_cast<float>(alpha[y1 * ALPHA_MAP_DIM + x1]);

    const float top = a00 + (a10 - a00) * tx;
    const float bottom = a01 + (a11 - a01) * tx;
    return (top + (bottom - top) * ty) / 255.0f;
}

} // namespace pipeline
} // namespace wowee

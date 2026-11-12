#pragma once

// The map's low-detail heights, its .wdl: what the client draws past the far
// clip as the horizon (CMapLowDetail, MapLowDetail.cpp).
//
// 0x007cc310 loads "%s\%s.wdl": MVER, then MWMO/MWID/MODF (the low-detail
// buildings, none outside Northrend), then MAOF - 64x64 file offsets, row by
// row - each to an MARE chunk of 17x17 outer then 16x16 inner heights
// (int16), followed by an optional MAHO (16 hole masks).

#include <array>
#include <cstdint>
#include <vector>

namespace wowee {
namespace pipeline {

struct WdlTile {
    /// 17x17 outer heights, then 16x16 inner (cell centre) heights.
    std::array<int16_t, 17 * 17 + 16 * 16> heights{};
    /// MAHO's 16 masks, one per row of cells, bit per cell; all clear without one.
    std::array<uint16_t, 16> holes{};
    int16_t minHeight = 0;
    int16_t maxHeight = 0;
};

struct WdlData {
    /// MAOF's order: index row * 64 + col, row running along the world's X
    /// (north to south), col along its Y. -1 where the map has no tile.
    std::array<int32_t, 64 * 64> tileIndex{};
    std::vector<WdlTile> tiles;
    [[nodiscard]] bool empty() const { return tiles.empty(); }
};

/// Parse a .wdl as 0x007cc310 reads it. Empty on a malformed file.
WdlData parseWDL(const std::vector<uint8_t>& data);

namespace wdl {

inline constexpr int kVertices = 17 * 17 + 16 * 16;  // 545
inline constexpr int kIndices = 16 * 16 * 4 * 3;     // 3072, 0x007d5240
inline constexpr float kZeroPoint = 17066.666f;
inline constexpr float kTileSize = 533.3333f;
inline constexpr float kCellSize = 33.333332f;

/// A tile's vertices in world (render) coordinates, as 0x007d5150 fills them:
/// the outer grid from the tile's north-west corner in 33.3 yd steps, then
/// the inner one half a step in.
std::array<float, kVertices * 3> tileVertices(const WdlTile& tile, int row, int col);

/// The tile's triangles, as 0x007d5240 lists them: per cell four around its
/// centre (centre, b, a) over the cell's edges in order.
std::array<uint16_t, kIndices> tileIndices();

/// The rows and columns 0x007cc0b0 visits for a camera at (x, y): the cells
/// (33.3 yd) within scale x farclip of it, widened by two tiles, clamped to
/// the map.
struct TileRange {
    int rowMin, rowMax, colMin, colMax;
};
TileRange visibleTiles(float x, float y, float farClip, float scale);

/// horizonFarclipScale as 0x0078d7c0 keeps it: 3 to 6.
float clampHorizonScale(float scale);

/// The horizon's own depth planes (0x00791170): near the far clip less 50,
/// far the scale times the far clip.
inline float horizonNear(float farClip) { return farClip - 50.0f; }
inline float horizonFar(float farClip, float scale) { return scale * farClip; }

}  // namespace wdl

}  // namespace pipeline
}  // namespace wowee

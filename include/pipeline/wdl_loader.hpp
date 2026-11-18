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

/// How close in front of the camera a tile whose ground is not in is drawn
/// from.
inline constexpr float kNearWithoutGround = 1.0f;

/// The near plane a tile is drawn from: the far clip less 50 where its
/// ground (the ADT tile) is in, as the client draws it (0x00791170). Where
/// it is not - still streaming after a teleport, failed, or past the square
/// the ground is loaded in, which the corners of a wide view reach before
/// the far clip - the near plane would cut away what nothing else draws and
/// leave the sky showing through the land; such a tile is drawn from just in
/// front of the camera instead, the silhouette in the fog colour it is past
/// the far clip.
inline float tileNear(bool groundIn, float farClip) {
    const float n = horizonNear(farClip);
    return groundIn ? n : (n < kNearWithoutGround ? n : kNearWithoutGround);
}

/// Whether the ground draws a chunk: the client's test of it against the
/// cube of the far clip about the camera (0x00780860 builds it, 0x007b6110
/// tests each chunk), on the centre of the chunk's bounding sphere widened
/// by its radius. The radius is at least half the chunk's diagonal, so the
/// chunk of every point within the far clip of the camera along both axes is
/// drawn (groundDrawsAt).
inline bool groundChunkInReach(float dx, float dy, float radius, float farClip) {
    const float reach = farClip + radius;
    return !(dx > reach || dx < -reach || dy > reach || dy < -reach);
}

/// Whether a point (dx, dy from the camera) of a loaded tile lies where the
/// ground's chunks are surely drawn: within the far clip of the camera along
/// both axes (groundChunkInReach).
inline bool groundDrawsAt(float dx, float dy, float farClip) {
    return !(dx > farClip || dx < -farClip || dy > farClip || dy < -farClip);
}

/// How a horizon tile is drawn: from which view depth (fromDepth), and up to
/// which view depth it gives way to the ground where groundDrawsAt holds
/// (groundNear; 0: nowhere), which horizon.frag tests per point.
struct TileDraw {
    float fromDepth = 0.0f;
    float groundNear = 0.0f;
};

/// The horizon stands where the ground does not: past the far clip less 50,
/// as the client draws it (0x00791170), and wherever the ground is not drawn
/// in front of that.
///
/// The ground's cull is a square (the far clip's cube about the camera,
/// groundChunkInReach), the far clip a view depth. Looking near a diagonal
/// of the square, or with a wide view, the square's sides come in front of
/// the far clip less 50: a tile whose ground is in reaches out of the square
/// there and its ground past the side is not drawn. Cut at the far clip less
/// 50 the horizon left the sky showing through the land at the top corners
/// of the view, coming and going with the turn of the camera and the field
/// of view (user 175/176: fov 70, facing 3.8515, 4.3 degrees off the
/// square's diagonal).
///
/// - Ground not in (still streaming, failed, past the load square): drawn
///   from just in front of the camera (tileNear).
/// - Ground in, the tile wholly inside the square: the client's horizon,
///   from the far clip less 50.
/// - Ground in, the tile reaching out of the square: drawn from just in
///   front of the camera too, but given way to the ground (discarded) inside
///   the square before the far clip less 50 - where the ground is drawn the
///   horizon is the client's, past the square's side it stands in for the
///   ground the cube leaves out.
///
/// tileMin/Max: the tile's square in world X and Y; eye: the camera's.
inline TileDraw tileDraw(bool groundIn, float tileMinX, float tileMinY, float tileMaxX, float tileMaxY, float eyeX,
                         float eyeY, float farClip) {
    const float n = horizonNear(farClip);
    const float front = n < kNearWithoutGround ? n : kNearWithoutGround;
    if (!groundIn) return {front, 0.0f};
    const bool inside = groundDrawsAt(tileMinX - eyeX, tileMinY - eyeY, farClip) &&
                        groundDrawsAt(tileMaxX - eyeX, tileMaxY - eyeY, farClip);
    if (inside) return {n, 0.0f};
    return {front, n};
}

/// horizon.frag's test: whether the horizon gives a point at (dx, dy) from
/// the camera and view depth `depth` to the ground.
inline bool horizonYieldsAt(float dx, float dy, float depth, float groundNear, float farClip) {
    return depth < groundNear && groundDrawsAt(dx, dy, farClip);
}

/// Whether a point of a tile at view depth `depth` (dx, dy from the camera;
/// chunkDx, chunkDy its chunk's centre, groundRadius its radius) is drawn by
/// the ground (tile in, chunk in the cube, before the far clip) or by the
/// horizon (tileDraw, horizonYieldsAt, before the horizon's far plane).
inline bool groundOrHorizonAt(bool groundIn, const TileDraw& draw, float chunkDx, float chunkDy, float groundRadius,
                              float dx, float dy, float depth, float farClip, float horizonFarPlane) {
    if (groundIn && depth < farClip && groundChunkInReach(chunkDx, chunkDy, groundRadius, farClip)) return true;
    return depth >= draw.fromDepth && depth <= horizonFarPlane &&
           !horizonYieldsAt(dx, dy, depth, draw.groundNear, farClip);
}

}  // namespace wdl

}  // namespace pipeline
}  // namespace wowee

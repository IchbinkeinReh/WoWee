#pragma once

// Which ADT tiles of a map are streamed in about a place - by the place alone.
//
// The client keeps the areas (ADT tiles) of a square about the camera loaded
// (0x00780860 builds it, 0x007b5950 walks it): the far clip in chunks of
// 33.3 yards plus one, rounded out to even chunks, and two chunks more each
// way, never fewer than eight past the camera's chunk - the far clip's square
// about the camera and a little more. Nothing in it asks which way the camera
// looks. The areas in it are loaded nearest first (qsort by 0x007b4830's
// distance).
//
// WoWee loaded a circle of whole tiles about the tile the eye stood in, its
// radius the far clip in tiles plus one, so what was loaded depended on where
// in its tile the eye was, which an orbit of the camera moves, and on how the
// world was reached: a world entry loaded a circle of four and kept it, a
// teleport within the map a circle of three, and the far clip's square has
// corners outside a circle of three. Now it is the square about the point the
// camera turns about, wide enough for the eye's orbit - the same for every
// yaw and pitch, and however the place was reached.
//
// The view's far clip is a depth, not a distance, so at the edges of a wide
// view ground past this square is in front of the far clip; the horizon
// draws a tile whose ground is not in from just in front of the camera
// (pipeline::wdl::tileNear), so that is never a hole.

#include "core/coordinates.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace wowee::rendering::terrain_stream {

inline constexpr float kTileSize = core::coords::TILE_SIZE;

/// What the client's square adds past the far clip: a chunk for the "+1",
/// up to one for rounding to even chunks and two more (0x00780860), about
/// 133 yards.
inline constexpr float kClientMargin = 4.0f * (kTileSize / 16.0f);

/// How far the eye can sit from the point the camera turns about (the
/// character, a possessed unit): the client's camera reaches 50 yards out
/// at most. Streaming about the focus rather than the eye, with this much
/// more reach, keeps every tile the eye can see loaded however it orbits -
/// and makes what is loaded the same for every yaw and pitch.
inline constexpr float kMaxEyeOffset = 50.0f;

/// Past the load square by this much before a tile is let go: one tile, so
/// walking back and forth over a tile's edge does not reload it.
inline constexpr float kUnloadHysteresis = kTileSize;

/// The reach (half the square's side, in yards) streamed in about the focus
/// for a far clip.
inline float loadReach(float farClip) {
    return std::max(farClip, 0.0f) + kClientMargin + kMaxEyeOffset;
}

/// Whether a tile is still to come, where a unit stands: streamed at all (not
/// on a map of one building), not in, and not one the map does not have.
inline constexpr bool tileComing(bool streaming, bool loaded, bool missing) {
    return streaming && !loaded && !missing;
}

/// How many tiles read may wait to be put in the world. Each holds its models
/// and textures decoded, a few hundred megabytes in a city.
inline constexpr size_t kReadyTiles = 3;
/// Whether a worker reads another with this many waiting.
inline constexpr bool mayPrepare(size_t waiting) { return waiting < kReadyTiles; }
/// Whether the main thread takes the next of them: as it is done with the one
/// it is putting in, and not before. It took all there were, every frame, and
/// none ever waited: the workers read the whole square ahead of it - every
/// tile of it in memory at once, where the frames are few.
inline constexpr bool takesReady(size_t puttingIn) { return puttingIn == 0; }

/// The reach a loaded tile is kept to.
inline float unloadReach(float loadReachYards) { return loadReachYards + kUnloadHysteresis; }

/// A rectangle of tiles, by ADT file index (x from render Y, y from render
/// X, as core::coords::worldToTile counts them), both ends included.
struct TileRect {
    int xMin = 0, xMax = -1, yMin = 0, yMax = -1;
    [[nodiscard]] bool contains(int x, int y) const {
        return x >= xMin && x <= xMax && y >= yMin && y <= yMax;
    }
    [[nodiscard]] bool operator==(const TileRect& o) const {
        return xMin == o.xMin && xMax == o.xMax && yMin == o.yMin && yMax == o.yMax;
    }
};

namespace detail {
inline int tileIndex(float renderCoord) {
    const int i = static_cast<int>(std::floor(32.0f - renderCoord / kTileSize));
    return std::clamp(i, 0, 63);
}
}  // namespace detail

/// The tiles whose square meets the square of `reach` yards about the render
/// point (renderX, renderY).
inline TileRect tilesWithin(float renderX, float renderY, float reach) {
    TileRect r;
    // A larger render coordinate is a smaller tile index.
    r.xMin = detail::tileIndex(renderY + reach);
    r.xMax = detail::tileIndex(renderY - reach);
    r.yMin = detail::tileIndex(renderX + reach);
    r.yMax = detail::tileIndex(renderX - reach);
    return r;
}

/// How far the render point is from tile (x, y)'s square, on the ground.
inline float distanceToTile(int x, int y, float renderX, float renderY) {
    const float maxRenderX = (32.0f - static_cast<float>(y)) * kTileSize;
    const float maxRenderY = (32.0f - static_cast<float>(x)) * kTileSize;
    const float dx = std::max({maxRenderX - kTileSize - renderX, 0.0f, renderX - maxRenderX});
    const float dy = std::max({maxRenderY - kTileSize - renderY, 0.0f, renderY - maxRenderY});
    return std::sqrt(dx * dx + dy * dy);
}

/// The tiles to stream in about the point, nearest first (ties by index, so
/// the order is the same every time for the same point).
inline std::vector<std::pair<int, int>> loadOrder(float renderX, float renderY, float reach) {
    const TileRect r = tilesWithin(renderX, renderY, reach);
    struct Entry { float d; int x, y; };
    std::vector<Entry> e;
    for (int y = r.yMin; y <= r.yMax; ++y)
        for (int x = r.xMin; x <= r.xMax; ++x) e.push_back({distanceToTile(x, y, renderX, renderY), x, y});
    std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) {
        if (a.d != b.d) return a.d < b.d;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    std::vector<std::pair<int, int>> out;
    out.reserve(e.size());
    for (const Entry& v : e) out.emplace_back(v.x, v.y);
    return out;
}

}  // namespace wowee::rendering::terrain_stream

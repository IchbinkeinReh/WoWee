#include "pipeline/wdl_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace pipeline {

namespace {

constexpr uint32_t tag(char a, char b, char c, char d) {
    // Chunk ids are stored reversed: "MAOF" reads as 'F','O','A','M'.
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

bool readU32(const std::vector<uint8_t>& d, size_t at, uint32_t& out) {
    if (at + 4 > d.size()) return false;
    std::memcpy(&out, d.data() + at, 4);
    return true;
}

}  // namespace

WdlData parseWDL(const std::vector<uint8_t>& data) {
    WdlData out;
    out.tileIndex.fill(-1);
    // Walk the chunks up to MAOF. 0x007cc310 takes MVER's size and then
    // either MWMO, MWID, MODF and MAOF or MAOF straight away.
    size_t at = 0;
    size_t maof = 0;
    while (at + 8 <= data.size()) {
        uint32_t id = 0, size = 0;
        readU32(data, at, id);
        readU32(data, at + 4, size);
        if (id == tag('M', 'A', 'O', 'F')) {
            maof = at + 8;
            if (maof + 64 * 64 * 4 > data.size()) return {};
            break;
        }
        at += 8 + static_cast<size_t>(size);
    }
    if (maof == 0) return {};

    for (int i = 0; i < 64 * 64; ++i) {
        uint32_t offset = 0;
        readU32(data, maof + static_cast<size_t>(i) * 4, offset);
        if (offset == 0) continue;
        uint32_t id = 0, size = 0;
        if (!readU32(data, offset, id) || !readU32(data, offset + 4, size)) continue;
        constexpr size_t kHeightBytes = sizeof(int16_t) * wdl::kVertices;
        if (id != tag('M', 'A', 'R', 'E') || size < kHeightBytes ||
            offset + 8 + kHeightBytes > data.size())
            continue;
        WdlTile tile;
        std::memcpy(tile.heights.data(), data.data() + offset + 8, kHeightBytes);
        const auto [lo, hi] = std::minmax_element(tile.heights.begin(), tile.heights.end());
        tile.minHeight = *lo;
        tile.maxHeight = *hi;
        // MAHO right after MARE, if there is one.
        const size_t next = offset + 8 + static_cast<size_t>(size);
        uint32_t holeId = 0, holeSize = 0;
        if (readU32(data, next, holeId) && readU32(data, next + 4, holeSize) &&
            holeId == tag('M', 'A', 'H', 'O') && holeSize >= 32 && next + 8 + 32 <= data.size())
            std::memcpy(tile.holes.data(), data.data() + next + 8, 32);
        out.tileIndex[i] = static_cast<int32_t>(out.tiles.size());
        out.tiles.push_back(tile);
    }
    return out;
}

namespace wdl {

std::array<float, kVertices * 3> tileVertices(const WdlTile& tile, int row, int col) {
    std::array<float, kVertices * 3> v{};
    // 0x007cc310: the tile's corner is 17066.666 less 33.333 per cell of
    // 16 per tile, in X by the row and in Y by the column.
    const float maxX = kZeroPoint - static_cast<float>(row * 16) * kCellSize;
    const float maxY = kZeroPoint - static_cast<float>(col * 16) * kCellSize;
    int k = 0;
    for (int i = 0; i < 17; ++i)
        for (int j = 0; j < 17; ++j, ++k) {
            v[k * 3 + 0] = maxX - static_cast<float>(i) * kCellSize;
            v[k * 3 + 1] = maxY - static_cast<float>(j) * kCellSize;
            v[k * 3 + 2] = static_cast<float>(tile.heights[k]);
        }
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j, ++k) {
            v[k * 3 + 0] = maxX - kCellSize * 0.5f - static_cast<float>(i) * kCellSize;
            v[k * 3 + 1] = maxY - kCellSize * 0.5f - static_cast<float>(j) * kCellSize;
            v[k * 3 + 2] = static_cast<float>(tile.heights[k]);
        }
    return v;
}

std::array<uint16_t, kIndices> tileIndices() {
    std::array<uint16_t, kIndices> idx{};
    size_t n = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) {
            const auto c = static_cast<uint16_t>(17 * 17 + i * 16 + j);
            const auto a = static_cast<uint16_t>(i * 17 + j);  // the cell's first outer corner
            const uint16_t tri[12] = {c, static_cast<uint16_t>(a + 1), a,
                                      c, static_cast<uint16_t>(a + 18), static_cast<uint16_t>(a + 1),
                                      c, static_cast<uint16_t>(a + 17), static_cast<uint16_t>(a + 18),
                                      c, a, static_cast<uint16_t>(a + 17)};
            for (uint16_t t : tri) idx[n++] = t;
        }
    return idx;
}

TileRange visibleTiles(float x, float y, float farClip, float scale) {
    // 0x007cc0b0: the camera's cell (ROUND of 0.03 x the distance from the
    // map's corner, less a half), the reach in cells, >> 4 to tiles, and two
    // more tiles each way.
    const int cellRow = static_cast<int>(std::nearbyint(-(x - kZeroPoint) * 0.03f - 0.5f));
    const int cellCol = static_cast<int>(std::nearbyint(-(y - kZeroPoint) * 0.03f - 0.5f));
    const int reach = static_cast<int>(std::nearbyint(scale * farClip * 0.030000001f));
    TileRange r;
    r.rowMin = std::max(((cellRow - reach) >> 4) - 2, 0);
    r.rowMax = std::min(((cellRow + reach) >> 4) + 2, 63);
    r.colMin = std::max(((cellCol - reach) >> 4) - 2, 0);
    r.colMax = std::min(((cellCol + reach) >> 4) + 2, 63);
    return r;
}

float clampHorizonScale(float scale) {
    if (!(scale >= 3.0f)) return 3.0f;
    return std::min(scale, 6.0f);
}

}  // namespace wdl

}  // namespace pipeline
}  // namespace wowee

#pragma once

// WorldMapTransforms.dbc: places shown on another map than they are on.
//
// The blood elf and draenei starting zones lie on Outland's map (530) and are
// shown on the Eastern Kingdoms' and on Kalimdor's. 0x00543920 is the client's
// one reader of the table, for the world map and the flight map alike; its
// coordinates are the wire's.

#include <cstdint>
#include <vector>

namespace wowee::game {

struct WorldMapTransform {
    uint32_t mapId = 0;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    uint32_t shownMapId = 0;
    float offsetX = 0.0f, offsetY = 0.0f;

    /// Both ends belong to the region (0x00543973..0x0054399c).
    [[nodiscard]] bool holds(uint32_t map, float x, float y) const {
        return mapId == map && minX <= x && x <= maxX && minY <= y && y <= maxY;
    }
};

/// Where a place is shown.
struct ShownPlace {
    uint32_t mapId = 0;
    float x = 0.0f, y = 0.0f;
};

/// 0x00543920: the first row holding the place moves it; without one it stays.
inline ShownPlace shownPlace(const std::vector<WorldMapTransform>& transforms,
                             uint32_t mapId, float x, float y) {
    for (const WorldMapTransform& t : transforms) {
        if (t.holds(mapId, x, y)) return {t.shownMapId, x + t.offsetX, y + t.offsetY};
    }
    return {mapId, x, y};
}

/// The table's rows; none where the client has no such table.
template <class Dbc>
std::vector<WorldMapTransform> readWorldMapTransforms(const Dbc* dbc) {
    std::vector<WorldMapTransform> out;
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 9) return out;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        out.push_back({dbc->getUInt32(i, 1), dbc->getFloat(i, 2), dbc->getFloat(i, 3),
                       dbc->getFloat(i, 4), dbc->getFloat(i, 5), dbc->getUInt32(i, 6),
                       dbc->getFloat(i, 7), dbc->getFloat(i, 8)});
    }
    return out;
}

}  // namespace wowee::game

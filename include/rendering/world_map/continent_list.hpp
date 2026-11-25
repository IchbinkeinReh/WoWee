#pragma once

/// The continents and their zones as the interface counts them, whichever map
/// is shown: what GetMapContinents and GetMapZones name and SetMapZoom takes.

#include "game/client_compare.hpp"
#include "rendering/world_map/world_map_types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wowee::rendering::world_map {

/// A WorldMapArea row as the list reads it.
struct MapAreaRow {
    uint32_t id = 0;
    uint32_t mapId = 0;
    uint32_t areaId = 0;
    uint32_t shownMapId = 0;  ///< DisplayMapID, the row's own map where it has none
};

struct MapContinent {
    struct ZoneRow {
        uint32_t wmaId = 0;
        std::string name;
    };
    uint32_t mapId = 0;
    uint32_t wmaId = 0;
    std::string name;
    std::vector<ZoneRow> zones;
};

/// 0x00547eb0: a continent for each row of no area, in the file's order, by its
/// map's name (0x00544b20); its zones the rows with an area shown on that map,
/// by the area's (0x005437b0, 0x005438a0).
inline std::vector<MapContinent> mapContinents(const std::vector<MapAreaRow>& rows,
                                               const std::function<std::string(uint32_t)>& mapName,
                                               const std::function<std::string(uint32_t)>& areaName) {
    std::vector<MapContinent> out;
    for (const MapAreaRow& row : rows) {
        if (row.areaId != 0) continue;
        MapContinent c;
        c.mapId = row.mapId;
        c.wmaId = row.id;
        c.name = mapName(row.mapId);
        for (const MapAreaRow& zone : rows)
            if (zone.areaId != 0 && zone.shownMapId == row.mapId) c.zones.push_back({zone.id, areaName(zone.areaId)});
        std::stable_sort(c.zones.begin(), c.zones.end(),
                         [](const auto& a, const auto& b) { return game::compareCaseless(a.name, b.name) < 0; });
        out.push_back(std::move(c));
    }
    return out;
}

/// GetCurrentMapContinent (0x00544d40): the continent's number, 0 for Azeroth's
/// map, -1 for the cosmic one - and for a map of no continent, a battleground's
/// or an instance's.
inline int shownContinent(ViewLevel level, std::optional<uint32_t> mapId, const std::vector<MapContinent>& list) {
    if (level == ViewLevel::COSMIC) return -1;
    if (level == ViewLevel::WORLD) return 0;
    for (size_t i = 0; mapId && i < list.size(); ++i)
        if (list[i].mapId == *mapId) return static_cast<int>(i) + 1;
    return -1;
}

/// Whether a WorldMapArea row is a continent's own or one of its zones'.
[[nodiscard]] inline bool isContinentArea(const std::vector<MapContinent>& continents, uint32_t wmaId) {
    for (const MapContinent& c : continents) {
        if (c.wmaId == wmaId) return true;
        for (const MapContinent::ZoneRow& z : c.zones)
            if (z.wmaId == wmaId) return true;
    }
    return false;
}

}  // namespace wowee::rendering::world_map

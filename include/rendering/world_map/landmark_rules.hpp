#pragma once

// Which of AreaPOI's points a map shows, where and with what icon: the list
// 0x00546310 makes as the shown map changes, and GetMapLandmarkInfo
// (0x00545a30) reads.

#include <array>
#include <cstdint>
#include <optional>

namespace wowee {
namespace rendering {
namespace world_map {

/// What is shown: Azeroth as a whole (the client's continent -1), a continent
/// (zone -1), a zone of one, or a map that is on none - a battlefield, an
/// instance (-2).
enum class LandmarkMap : uint8_t { World, Continent, Zone, Other };

inline constexpr uint32_t kLandmarkOnZone = 0x4;
inline constexpr uint32_t kLandmarkOnContinent = 0x8;
inline constexpr uint32_t kLandmarkOnWorld = 0x10;
/// Its icon is drawn on a zone's map, where one without it is a bare point.
inline constexpr uint32_t kLandmarkZoneIcon = 0x80;
/// Only on the map of the area it names.
inline constexpr uint32_t kLandmarkOwnArea = 0x400;

/// 0x005445f0's tests of the flags: a zone's map or a map of its own takes
/// those with 0x4, a continent's those with 0x8, the world's those with both
/// 0x8 and 0x10. `ownArea`: the shown map is the one of the point's own area
/// on its own map (0x400 asks for it).
[[nodiscard]] constexpr bool landmarkOnMap(uint32_t flags, LandmarkMap map, bool ownArea) {
    if ((flags & kLandmarkOwnArea) != 0 && !ownArea) return false;
    switch (map) {
        case LandmarkMap::World:
            return (flags & (kLandmarkOnWorld | kLandmarkOnContinent)) == (kLandmarkOnWorld | kLandmarkOnContinent);
        case LandmarkMap::Continent: return (flags & kLandmarkOnContinent) != 0;
        case LandmarkMap::Zone:
        case LandmarkMap::Other: return (flags & kLandmarkOnZone) != 0;
    }
    return false;
}

/// Whether the player knows of it (0x00546310): a point that names an area
/// shows once the area is explored, unless the area's exploration level is
/// negative; an area there is no row of shows none.
[[nodiscard]] constexpr bool landmarkKnown(uint32_t areaId, std::optional<int32_t> explorationLevel, bool explored) {
    if (static_cast<int32_t>(areaId) < 1) return true;
    if (!explorationLevel) return false;
    return *explorationLevel < 0 || explored;
}

/// A point with a world state is in the list while the state is set
/// (0x00548d10).
[[nodiscard]] constexpr bool landmarkStateSet(uint32_t worldStateId, uint32_t worldStateValue) {
    return worldStateId == 0 || worldStateValue != 0;
}

/// Its icon, GetMapLandmarkInfo's third: Icon[state - 1] for a state of 1 to 9,
/// else Icon[0]; on a zone's map and any other but Azeroth and a continent's
/// only with 0x80, and 0 without.
[[nodiscard]] constexpr int32_t landmarkIcon(uint32_t flags, LandmarkMap map, const std::array<int32_t, 9>& icons,
                                             uint32_t worldStateId, uint32_t worldStateValue) {
    const bool zoneless = map == LandmarkMap::World || map == LandmarkMap::Continent;
    if ((flags & kLandmarkZoneIcon) == 0 && !zoneless) return 0;
    uint32_t i = 0;
    if (worldStateId != 0 && worldStateValue >= 1 && worldStateValue <= 9) i = worldStateValue - 1;
    return icons[i];
}

/// A place on a continent, in the wire's axes, in the Azeroth map's frame from
/// the top left (0x00544140): WorldMapContinent's ContinentOffset and Scale.
struct LandmarkPoint {
    float x = 0.0f, y = 0.0f;
};
[[nodiscard]] constexpr LandmarkPoint landmarkOnWorld(float offsetX, float offsetY, float scale, float wireX,
                                                      float wireY) {
    return {(offsetX * 0.015968064f + 0.5f) - wireY * 2.994012e-05f * scale,
            (offsetY * 0.023952097f + 0.5f) - wireX * 4.4910183e-05f * scale};
}

}  // namespace world_map
}  // namespace rendering
}  // namespace wowee

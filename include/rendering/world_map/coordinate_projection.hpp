// coordinate_projection.hpp - Pure coordinate math for world map UV projection.
// Extracted from WorldMap methods (Phase 2 of refactoring plan).
// All functions are stateless free functions - trivially testable.
#pragma once

#include "rendering/world_map/world_map_types.hpp"
#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {
namespace world_map {

/// Project render-space position to [0,1] UV on a zone or continent map.
glm::vec2 renderPosToMapUV(const glm::vec3& renderPos,
                            const ZoneBounds& bounds,
                            bool isContinent);

/// Derive effective projection bounds for a continent from its child zones.
/// Uses zoneBelongsToContinent() internally. Returns false if insufficient data.
bool getContinentProjectionBounds(const std::vector<Zone>& zones,
                                   int contIdx,
                                   float& left, float& right,
                                   float& top, float& bottom);

/// The bounds a zone is projected against, and whether it is a continent.
///
/// A continent is drawn against bounds derived from its child zones, not its
/// own: WorldMapArea gives a continent a box that does not match where its zones
/// actually are, so projecting against that box puts every marker in the wrong
/// place. A zone uses its own bounds.
///
/// Nine places worked this out for themselves - every marker layer and the
/// facade - which is nine chances for one of them to project against the wrong
/// rectangle and put its markers somewhere the others do not.
ZoneBounds projectionBoundsFor(const std::vector<Zone>& zones, int zoneIdx,
                               bool& isContinent);

/// Find the best-fit continent index for a player position.
/// Prefers the smallest containing continent; falls back to nearest center.
int findBestContinentForPlayer(const std::vector<Zone>& zones,
                                const glm::vec3& playerRenderPos);

/// Find the zone the player is in.
///
/// authoritativeZoneId is the zone the server says the player is in, from
/// SMSG_INIT_WORLD_STATES. When it names a zone on this map that is the answer,
/// full stop - no geometry involved.
///
/// The fallback below is a guess and reads like one. WorldMapArea bounds are
/// axis-aligned boxes around irregular zones, so neighbours overlap heavily and
/// the best it can do is pick the box the player sits deepest inside. That is
/// how the map came to open on a zone the player was merely near.
///
/// Pass 0 when the server has not said (before the first world-state packet, or
/// on expansions that do not send one) and the geometry is used alone.
int findZoneForPlayer(const std::vector<Zone>& zones,
                       const glm::vec3& playerRenderPos,
                       uint32_t authoritativeZoneId = 0);

/// Index of the zone with this AreaID, or -1. Exact, no geometry.
int findZoneByAreaId(const std::vector<Zone>& zones, uint32_t areaId);

/// Whether a zone is one of a continent's: shown on its map (0x00547eb0),
/// whatever parent it names - Dalaran's is the zone below it.
bool zoneBelongsToContinent(const std::vector<Zone>& zones,
                             int zoneIdx, int contIdx);

/// Check whether the zone at idx is a root continent (has leaf continents as children).
bool isRootContinent(const std::vector<Zone>& zones, int idx);

/// Check whether the zone at idx is a leaf continent (parentWorldMapID != 0, areaID == 0).
bool isLeafContinent(const std::vector<Zone>& zones, int idx);

/// A DungeonMap row's rectangle, its four columns in the file's order
/// (0x00544140: +0xc and +0x10 across the map, +0x14 and +0x18 down it).
inline ZoneBounds floorRectangle(float minY, float maxY, float minX, float maxX) {
    return {.locLeft = maxY, .locRight = minY, .locTop = maxX, .locBottom = minX};
}

/// Whether a unit is on what a zone's map shows: with a floor selected, on that
/// floor (0x00544140).
[[nodiscard]] constexpr bool onShownFloor(int selectedRow, int unitRow) {
    return selectedRow <= 0 || unitRow == selectedRow;
}

/// What a zone's map spans with a floor selected: the floor's rectangle, else
/// the zone's own (0x00544140).
inline const ZoneBounds& shownBounds(const Zone& zone, int floorRowId) {
    for (const DungeonFloor& floor : zone.dungeonFloors)
        if (floorRowId > 0 && static_cast<int>(floor.rowId) == floorRowId) return floor.bounds;
    return zone.bounds;
}

/// GetMapInfo's second value (0x00543cd0): the shown map's height in the
/// 1002-pixel-wide frame, round(across-down extent * 1002 / across extent),
/// 0 when either extent is 0. A zone or floor gives its rectangle's
/// (left - right, top - bottom); a continent WorldMapContinent's grid,
/// (right - left + 1, bottom - top + 1).
[[nodiscard]] inline uint32_t mapInfoHeight(float across, float down) {
    if (across == 0.0f || down == 0.0f) return 0;
    return static_cast<uint32_t>(static_cast<int64_t>(
        std::nearbyint(static_cast<double>(down) * 1002.0 / static_cast<double>(across))));
}

/// GetMapInfo's third value (0x00544ca0): the height with its low byte
/// raised to the next power of two (668 = 0x29c -> 0x200 + 0x100 = 768).
[[nodiscard]] constexpr uint32_t mapInfoPaddedHeight(uint32_t height) {
    uint32_t low = height & 0xffu;
    const uint32_t high = height - low;
    if ((low & (low - 1u)) != 0) {
        for (int bit = 7; bit > 0; --bit) {
            if (low & (1u << bit)) {
                low = 1u << (bit + 1);
                break;
            }
        }
    }
    return high + low;
}

/// What the shown map spans (0x00544140): a zone's rectangle on a zone's map,
/// the continent's on the continent's.
inline ZoneBounds mapBounds(const std::vector<Zone>& zones, int shownIdx, int continentIdx, int floorRowId) {
    const Zone& shown = zones[static_cast<size_t>(shownIdx)];
    if (shown.areaID != 0) return shownBounds(shown, floorRowId);
    bool isContinent = false;
    return projectionBoundsFor(zones, continentIdx, isContinent);
}

/// Where one piece of an explored-area overlay goes on the 1024 by 768 map,
/// and how much of its file it is.
///
/// What WorldMapFrame_Update does with GetMapOverlayInfo: an overlay of
/// textureWidth by textureHeight is cut into 256-pixel pieces, numbered across
/// then down from one, and piece k of row j sits at offsetX + 256 (k - 1),
/// offsetY + 256 (j - 1) from the map's top-left. Every piece is 256 across but
/// the last in a row or column, which is what is left of the overlay - and the
/// file holding that one is padded to the next power of two from 16, so only
/// part of it is the piece.
struct OverlayPiece {
    int x = 0, y = 0;          ///< top-left on the map, in map pixels
    int width = 0, height = 0; ///< size on the map; zero when there is no such piece
    float uMax = 1.0f, vMax = 1.0f;  ///< how much of the file to sample
};
OverlayPiece overlayPiece(const OverlayEntry& overlay, int pieceIndex);

} // namespace world_map
} // namespace rendering
} // namespace wowee

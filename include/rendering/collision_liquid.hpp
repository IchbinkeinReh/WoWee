#pragma once

/// Which liquid a unit is in, and how high it stands, as the client finds it
/// for its movement (0x007a1bc0): the liquid level a swimmer's depth is read
/// against (swim starts when the level is more than 0.75 of the body's
/// height over the feet, 0x0073ab20 -> 0x00730d10).
///
/// The client asks one place only, chosen by where the unit is:
///
///  - In a building's group (the world object's floor search 0x007c2f80
///    found one under it), that group's liquid (0x007a1a30 -> 0x007c8360)
///    and nothing else: the sea outside a harbour building does not count
///    inside it.
///  - Otherwise first the liquid of an exterior group (MOGI 0x2000 clear)
///    of any building whose box holds the point (0x007a09d0 -> 0x007aeb90),
///    then the ground's (0x007a0820): a chunk's liquid with a tile there,
///    the point below its surface plus 0.01, and the ground under the point
///    below the point plus 0.01 - liquid under the terrain is not there.
///
/// Either way the level counts only for a point under it.

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::collision {

/// A building liquid's tile, in the group's own space (0xaeee54 is its
/// inverse).
inline constexpr float kBuildingLiquidTile = 4.1666625f;
/// 0x007a0820: how far above a chunk liquid's surface, or below the ground
/// under it, still counts.
inline constexpr float kGroundLiquidSlack = 0.01f;

/// 0x007c8360: a building group's liquid level at a point in the group's own
/// space - `heights` (xTiles+1) by (yTiles+1) from `corner`, `tileFlags` one
/// a tile, a tile flagged 0x0F in its low four bits having none. The level
/// counts only when the point is under it.
inline std::optional<float> buildingLiquidLevel(const glm::vec3& local, const glm::vec2& corner,
                                                int xTiles, int yTiles,
                                                const std::vector<float>& heights,
                                                const std::vector<uint8_t>& tileFlags) {
    if (xTiles <= 0 || yTiles <= 0) return std::nullopt;
    const float fx = (local.x - corner.x) / kBuildingLiquidTile;
    const float fy = (local.y - corner.y) / kBuildingLiquidTile;
    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));
    if (ix < 0 || iy < 0 || ix >= xTiles || iy >= yTiles) return std::nullopt;
    const size_t tile = static_cast<size_t>(iy) * static_cast<size_t>(xTiles) + static_cast<size_t>(ix);
    if (tile >= tileFlags.size() || (tileFlags[tile] & 0x0Fu) == 0x0Fu) return std::nullopt;
    const size_t row = static_cast<size_t>(xTiles) + 1;
    const size_t i0 = static_cast<size_t>(iy) * row + static_cast<size_t>(ix);
    const size_t i1 = i0 + row;
    if (i1 + 1 >= heights.size()) return std::nullopt;
    const float tx = fx - static_cast<float>(ix);
    const float ty = fy - static_cast<float>(iy);
    const float rowA = heights[i0] + (heights[i0 + 1] - heights[i0]) * tx;
    const float rowB = heights[i1] + (heights[i1 + 1] - heights[i1]) * tx;
    const float level = rowA + (rowB - rowA) * ty;
    if (!(local.z < level)) return std::nullopt;
    return level;
}

/// 0x007a0820's two tests on a chunk's liquid at a point: under its surface
/// (with the slack), and the ground there under the point.
inline bool groundLiquidHolds(float pointZ, float level, const std::optional<float>& groundZ) {
    if (!(pointZ < level + kGroundLiquidSlack)) return false;
    return !groundZ || *groundZ < pointZ + kGroundLiquidSlack;
}

}  // namespace wowee::rendering::collision

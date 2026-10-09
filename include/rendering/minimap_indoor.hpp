#pragma once

/// The minimap indoors: the WMO groups' own minimap pictures, laid out the way
/// the client cuts them (Minimap.cpp, 0x007f5ba0; the WMO side, 0x007af8d0).
///
/// Every group of a building has its pictures in Textures\Minimap, named
/// "<wmo>_<group>_<x>_<y>" through md5translate. Each covers a tile of the
/// group's bounding box in the WMO's own coordinates, so they are laid out
/// from the box, and the box alone says how many there are and how big.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::minimap_indoor {

/// Half a yard to a pixel (0x00aee00c).
constexpr float kYardsPerPixel = 0.5f;
/// A whole picture is 256 pixels, 128 yards (0x009ce850).
constexpr float kTileYards = 256.0f * kYardsPerPixel;
/// A picture is 32 to 256 pixels on a side.
constexpr int kMinTilePixels = 32;
constexpr int kMaxTilePixels = 256;
/// The outermost pictures reach two pixels past the box (0x007af8d0).
constexpr float kEdgePad = 2.0f * kYardsPerPixel;

/// The side of a group's pictures in pixels for an `extent` of its box: the
/// power of two that holds the extent at two pixels a yard, 32 to 256.
inline int tilePixels(float extent) {
    const float wanted = extent / kTileYards * 256.0f;
    const int exponent = wanted > 1.0f ? static_cast<int>(std::ceil(std::log2(wanted))) : 0;
    const int pixels = exponent >= 31 ? kMaxTilePixels : (1 << exponent);
    return std::clamp(pixels, kMinTilePixels, kMaxTilePixels);
}

/// One picture of a group: its place in the group's grid, and the rectangle
/// it covers in the WMO's coordinates.
struct Tile {
    int x = 0;
    int y = 0;
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
};

/// The pictures of a group with bounding box `boxMin`..`boxMax` that reach
/// into `queryMin`..`queryMax` (both in the WMO's coordinates), as
/// 0x007af8d0 lays them out: from the box's low corner, ceil(extent / 128)
/// across and as many up, each tilePixels() / 2 yards, the outermost ones
/// stretched a yard past the box.
inline std::vector<Tile> groupTiles(const glm::vec3& boxMin, const glm::vec3& boxMax,
                                    const glm::vec2& queryMin, const glm::vec2& queryMax) {
    std::vector<Tile> out;
    const float extentX = boxMax.x - boxMin.x;
    const float extentY = boxMax.y - boxMin.y;
    const int across = static_cast<int>(std::ceil(extentX / kTileYards));
    const int up = static_cast<int>(std::ceil(extentY / kTileYards));
    const float w = static_cast<float>(tilePixels(extentX)) * kYardsPerPixel;
    const float h = static_cast<float>(tilePixels(extentY)) * kYardsPerPixel;
    for (int ty = 0; ty < up; ++ty) {
        for (int tx = 0; tx < across; ++tx) {
            Tile t;
            t.x = tx;
            t.y = ty;
            t.min = {boxMin.x + static_cast<float>(tx) * w, boxMin.y + static_cast<float>(ty) * h};
            t.max = t.min + glm::vec2(w, h);
            if (tx == 0) t.min.x -= kEdgePad;
            if (ty == 0) t.min.y -= kEdgePad;
            if (tx == across - 1) t.max.x += kEdgePad;
            if (ty == up - 1) t.max.y += kEdgePad;
            // Touching counts.
            if (queryMin.x <= t.max.x && queryMin.y <= t.max.y &&
                t.min.x <= queryMax.x && t.min.y <= queryMax.y) {
                out.push_back(t);
            }
        }
    }
    return out;
}

/// md5translate's name for a picture: "%s_%03d_%02d_%02d" (0x007f5070), the
/// WMO's path without its extension, the group and the tile.
inline std::string tileName(const std::string& wmoBase, int group, int x, int y) {
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "_%03d_%02d_%02d", group, x, y);
    return wmoBase + suffix;
}

/// The WMO's path as the pictures are named after it: the extension gone.
inline std::string wmoBaseName(const std::string& wmoPath) {
    const auto dot = wmoPath.find_last_of('.');
    const auto slash = wmoPath.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return wmoPath;
    return wmoPath.substr(0, dot);
}

/// The square of the world the indoor map is drawn from (0x007f5ba0): the
/// cell of the zoom's radius the player stands in, grown by that radius on
/// every side - three radii across, which is what the picture behind the
/// map covers. The cell moves only when the player leaves it.
struct Area {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
    [[nodiscard]] glm::vec2 center() const { return (min + max) * 0.5f; }
    [[nodiscard]] float span() const { return max.x - min.x; }
};

/// `pos` is in world axes - either order of them, the cell is square. The
/// client gives the area a height too, and then tests only across it
/// (0x007ae2b0, 0x007af8d0), so it has none here.
inline Area area(const glm::vec2& pos, float radius) {
    Area a;
    if (radius <= 0.0f) return a;
    const glm::vec2 cell(std::floor(pos.x / radius) * radius, std::floor(pos.y / radius) * radius);
    a.min = cell - glm::vec2(radius);
    a.max = cell + glm::vec2(2.0f * radius);
    return a;
}

/// Whether a group's box reaches into an area, across only and touching
/// counting (0x007ae2b0).
inline bool reaches(const glm::vec2& boxMin, const glm::vec2& boxMax,
                    const glm::vec2& areaMin, const glm::vec2& areaMax) {
    return areaMin.x <= boxMax.x && areaMin.y <= boxMax.y &&
           boxMin.x <= areaMax.x && boxMin.y <= areaMax.y;
}

/// The order the groups' pictures go down in (0x0057bd10 sorting on
/// 0x007f3ce0's key): the lowest first, by the height of the middle of the
/// group's box over the player's, and the player's own group last, on top.
inline float drawKey(float groupMidZ, float playerZ, bool playerGroup) {
    return playerGroup ? 3.4028235e38f : groupMidZ - playerZ;
}

/// MOGI's flags the indoor map reads.
constexpr uint32_t kGroupExterior = 0x8;
constexpr uint32_t kGroupExteriorLit = 0x40;
constexpr uint32_t kGroupUnreachable = 0x80;
constexpr uint32_t kGroupInterior = 0x2000;

/// A group as MOGI has it, in the WMO's coordinates.
struct GroupInfo {
    uint32_t flags = 0;
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

/// Whether a group is one the indoor map is drawn for: inside, and not open
/// to the sky.
constexpr bool isIndoorGroup(uint32_t flags) {
    return (flags & kGroupInterior) != 0 && (flags & kGroupExterior) == 0;
}

/// The groups whose pictures make up the map (0x007b00a0, 0x007afc70): from
/// the player's group through the portals, into every group lit the way it is
/// - (flags & 0x48) equal to the player's group's 0x40, so inside stays inside
/// - that reaches the area, and never into one marked unreachable (0x80).
/// `neighbours[g]` are the groups on the far side of g's portals.
inline std::vector<uint32_t> connectedGroups(const std::vector<GroupInfo>& groups,
                                             const std::vector<std::vector<uint32_t>>& neighbours,
                                             uint32_t start, const glm::vec2& areaMin,
                                             const glm::vec2& areaMax) {
    std::vector<uint32_t> out;
    if (start >= groups.size()) return out;
    const uint32_t lit = groups[start].flags & kGroupExteriorLit;
    std::vector<uint8_t> seen(groups.size(), 0);
    std::vector<uint32_t> stack{start};
    while (!stack.empty()) {
        const uint32_t g = stack.back();
        stack.pop_back();
        if (g >= groups.size() || seen[g]) continue;
        seen[g] = 1;
        const GroupInfo& info = groups[g];
        if ((info.flags & (kGroupExterior | kGroupExteriorLit)) != lit) continue;
        if (!reaches(glm::vec2(info.min), glm::vec2(info.max), areaMin, areaMax)) continue;
        out.push_back(g);
        if (g >= neighbours.size()) continue;
        for (uint32_t n : neighbours[g]) {
            if (n < groups.size() && !seen[n] && (groups[n].flags & kGroupUnreachable) == 0) {
                stack.push_back(n);
            }
        }
    }
    return out;
}

/// What the minimap needs of the building the player is inside.
struct Scene {
    /// The name the pictures are filed under (wmoBaseName).
    std::string wmoBase;
    /// The WMO's coordinates to the world's (render axes).
    glm::mat4 modelMatrix{1.0f};
    uint32_t instanceId = 0;
    uint32_t playerGroup = 0;
    /// The player's height in the WMO's coordinates.
    float playerLocalZ = 0.0f;
    /// The area, in render axes, and its bounds in the WMO's coordinates.
    Area area;
    glm::vec2 localMin{0.0f};
    glm::vec2 localMax{0.0f};
    struct Group {
        uint32_t index = 0;
        glm::vec3 min{0.0f};
        glm::vec3 max{0.0f};
    };
    std::vector<Group> groups;
};

}  // namespace wowee::rendering::minimap_indoor

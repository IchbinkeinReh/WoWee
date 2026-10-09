#pragma once

/// The minimap indoors: the WMO groups' own minimap pictures, laid out the way
/// the client cuts them (Minimap.cpp, 0x007f5ba0; the WMO side, 0x007af8d0).
///
/// Every group of a building has its pictures in Textures\Minimap, named
/// "<wmo>_<group>_<x>_<y>" through md5translate. Each covers a tile of the
/// group's bounding box in the WMO's own coordinates, so they are laid out
/// from the box, and the box alone says how many there are and how big.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
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

/// The WMO's path as the pictures are named after it (0x007f3a70): the
/// "World\\" in front gone - the client steps over its six characters - and
/// everything from the last dot. "World\\wmo\\Dungeon\\X\\X.wmo" is filed as
/// "wmo\\Dungeon\\X\\X".
inline std::string wmoBaseName(const std::string& wmoPath) {
    std::string base = wmoPath;
    constexpr const char kWorld[] = "world";
    if (base.size() > 6 && (base[5] == '\\' || base[5] == '/')) {
        bool world = true;
        for (int i = 0; i < 5; ++i) {
            if (std::tolower(static_cast<unsigned char>(base[i])) != kWorld[i]) world = false;
        }
        if (world) base.erase(0, 6);
    }
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.erase(dot);
    return base;
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

/// A group as MOGI has it, in the WMO's coordinates, and MOGP's WMOAreaTable
/// group id (the group object's +0x180).
struct GroupInfo {
    uint32_t flags = 0;
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    int32_t areaGroupId = 0;
};

/// WMOAreaTable's rows by WMO, name set and group (0x00990560 looks them up
/// exactly; the group -1 is the building's own row): their Flags.
using AreaFlags = std::unordered_map<uint64_t, uint32_t>;
inline uint64_t areaKey(uint32_t wmoId, uint32_t nameSet, int32_t groupId) {
    return (static_cast<uint64_t>(wmoId) << 40) ^ (static_cast<uint64_t>(nameSet & 0xFFu) << 32) ^
           static_cast<uint32_t>(groupId);
}
inline const uint32_t* areaFlags(const AreaFlags* table, uint32_t wmoId, uint32_t nameSet, int32_t groupId) {
    if (!table) return nullptr;
    const auto it = table->find(areaKey(wmoId, nameSet, groupId));
    return it == table->end() ? nullptr : &it->second;
}

/// Whether the player's group counts as inside to 0x007a1480: it is not
/// open to the sky.
constexpr bool isInteriorGroup(uint32_t flags) {
    return (flags & kGroupExterior) == 0;
}

/// Whether the minimap is the indoor one (0x007f5ba0, from 0x007a1640's two
/// WMOAreaTable rows): never where the group's own row has flag 0x20; else
/// where the building's row (group -1) has 0x1, 0x8 or 0x10, or the group is
/// an inside one. `groupRow` and `rootRow` are the rows' Flags, null for none.
inline bool isIndoors(uint32_t groupFlags, const uint32_t* groupRow, const uint32_t* rootRow) {
    if (groupRow && (*groupRow & 0x20u) != 0) return false;
    return (rootRow && (*rootRow & 0x19u) != 0) || isInteriorGroup(groupFlags);
}

/// With the building's row's 0x8 or 0x10 and the group exterior or lit as
/// one (0x48), the map is that group's own pictures alone (0x007a17e0 into
/// 0x007afe70) rather than the walk through the portals.
inline bool ownGroupOnly(uint32_t groupFlags, const uint32_t* rootRow) {
    return rootRow && (*rootRow & 0x18u) != 0 && (groupFlags & 0x48u) != 0;
}

/// A portal out of a group (MOPR): the group past it and the portal.
struct PortalLink {
    uint32_t group = 0;
    uint32_t portal = 0;
};

/// minimapPortalMax, "Max Number of Portals to traverse for minimap": 99
/// by default (0x00510000's registration).
constexpr uint32_t kPortalMax = 99;

/// The two WMOAreaTable group ids the walk goes neither on from nor into
/// (0x007afc70 tests 0x59e7 and 0x59e8).
constexpr bool walkStopsAt(int32_t areaGroupId) {
    return areaGroupId == 0x59e7 || areaGroupId == 0x59e8;
}

/// Which faces of a box a point is outside, a bit each (0x007ae1f0): 1, 2, 4
/// below its x, y, z; 8, 0x10, 0x20 above them.
inline uint32_t outcode(const glm::vec3& v, const glm::vec3& lo, const glm::vec3& hi) {
    return (v.x < lo.x ? 1u : 0u) | (v.y < lo.y ? 2u : 0u) | (v.z < lo.z ? 4u : 0u) |
           (v.x > hi.x ? 8u : 0u) | (v.y > hi.y ? 0x10u : 0u) | (v.z > hi.z ? 0x20u : 0u);
}

/// What the walk reads of a building.
struct WalkModel {
    const std::vector<GroupInfo>* groups = nullptr;
    /// Per group, its portals in MOPR's order.
    const std::vector<std::vector<PortalLink>>* links = nullptr;
    /// Per portal (MOPT), its vertices (MOPV), in the WMO's coordinates.
    const std::vector<std::vector<glm::vec3>>* portals = nullptr;
};

namespace detail {
inline void walk(const WalkModel& m, uint32_t g, uint32_t from, const glm::vec3& areaMin,
                 const glm::vec3& areaMax, bool interior, uint32_t mask, uint32_t depth,
                 uint32_t portalMax, std::vector<uint8_t>& seen, std::vector<uint32_t>& out) {
    const auto& groups = *m.groups;
    if (g >= groups.size()) return;
    const GroupInfo& info = groups[g];
    const uint32_t lit = interior ? (info.flags & (kGroupExterior | kGroupExteriorLit))
                                  : (info.flags & kGroupExterior);
    if (lit != mask || seen[g]) return;
    seen[g] = 1;
    if (!reaches(glm::vec2(info.min), glm::vec2(info.max), glm::vec2(areaMin), glm::vec2(areaMax))) return;
    out.push_back(g);
    if (walkStopsAt(info.areaGroupId) || !m.links || g >= m.links->size() || !(depth < portalMax)) return;
    for (const PortalLink& link : (*m.links)[g]) {
        const uint32_t t = link.group;
        if (t == 0xFFFFu || t == from || t >= groups.size()) continue;
        if ((groups[t].flags & kGroupUnreachable) != 0) continue;
        // Into the group only where the portal is not wholly beyond one face
        // of the area, its height included.
        if (!m.portals || link.portal >= m.portals->size()) continue;
        const auto& verts = (*m.portals)[link.portal];
        if (verts.empty()) continue;
        uint32_t code = 0xFFFFFFFFu;
        for (const glm::vec3& v : verts) code &= outcode(v, areaMin, areaMax);
        if (code != 0 || walkStopsAt(groups[t].areaGroupId)) continue;
        // The client counts each portal it goes through against the limit,
        // the siblings' included (its depth argument is incremented in place).
        ++depth;
        walk(m, t, g, areaMin, areaMax, interior, mask, depth, portalMax, seen, out);
    }
}
}  // namespace detail

/// The groups whose pictures make up the map (0x007b00a0, 0x007afc70), in the
/// order they are reached: from the player's group through the portals, into
/// groups lit as it is - inside, (flags & 0x48) equal to its 0x40, so inside
/// stays inside; not inside, every exterior group - that reach the area
/// across, never into one marked unreachable (0x80), only through a portal
/// that is not wholly outside the area's box (its height the player's, down
/// half the zoom radius), never past WMOAreaTable groups 0x59e7 and 0x59e8,
/// and no more than `portalMax` portals in.
inline std::vector<uint32_t> connectedGroups(const WalkModel& m, uint32_t start, const glm::vec3& areaMin,
                                             const glm::vec3& areaMax, bool interior,
                                             uint32_t portalMax = kPortalMax) {
    std::vector<uint32_t> out;
    if (!m.groups || start >= m.groups->size()) return out;
    const uint32_t mask = interior ? ((*m.groups)[start].flags & kGroupExteriorLit) : kGroupExterior;
    std::vector<uint8_t> seen(m.groups->size(), 0);
    detail::walk(m, start, start, areaMin, areaMax, interior, mask, 0, portalMax, seen, out);
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

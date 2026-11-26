#pragma once

/// What the camera inside a building sees of the outside: the client's walk
/// through the portals (0x007ad1f0, 0x007ac060).
///
/// The client draws nothing of the outside from a room but through its exits.
/// Wherever the walk meets a portal to a group of the outside (0x007ac060, MOGI
/// 0x10008) it adds that portal's outline on screen to one rectangle (0xadf58c:
/// 0x007a8f20 -> 0x00790ad0), and the whole outside - the ground, and every
/// exterior group of every building, linked by a portal or not - is then drawn
/// through that rectangle (0x0079a870, 0x0079a790, 0x0079a160, 0x007b3a10).
/// Where the walk meets none, none of it is drawn (0x00794250).
///
/// A portal to a group lit by the sky (0x50148), and a camera in one (0x40140,
/// 0x00795d40), add to another rectangle (0xadf570), which the sky is drawn
/// for: Stormwind's streets are rooms (0x2040) with the sky over them, and the
/// city's far shells, exterior groups no portal leads to, are not seen from
/// there.
///
/// So an exit never stands for the group behind it alone: Booty Bay is one
/// building of 46 groups, its decks and streets a dozen exterior groups no
/// portal joins, and a door opening onto one of them shows them all.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::wmo_portal_walk {

/// MOGI flags. Exterior groups (0x8) are drawn with the outside and never
/// walked into; 0x10000 neither, and they are drawn always (0x007ad1f0's last
/// loop).
constexpr uint32_t kExterior = 0x8u;
constexpr uint32_t kAlways = 0x10000u;
/// A portal to a group with any of these shows the outside (0x007ac060).
constexpr uint32_t kShowsOutside = kExterior | kAlways;
/// How deep the walk goes (0x00adfe40, read into 0x00d1bee4).
constexpr int kMaxDepth = 10;

/// MOPT: the corners, and the plane as the file has it, which is the one
/// 0x007ac060 tests the camera against.
struct Portal {
    uint32_t startVertex = 0;
    uint32_t vertexCount = 0;
    glm::vec3 normal{0.0f};
    float distance = 0.0f;
};

/// MOPR: the portal, the group past it, and the side of its plane the group
/// owning the reference is on (below 0 the back).
struct Ref {
    uint16_t portalIndex = 0;
    uint16_t groupIndex = 0;
    int16_t side = 0;
};

/// A building, every index the file's own.
struct Building {
    std::span<const glm::vec3> vertices;
    std::span<const Portal> portals;
    std::span<const Ref> refs;
    /// Per group: its first reference and how many (MOGP).
    std::span<const std::pair<uint16_t, uint16_t>> groupRefs;
    std::span<const uint32_t> flags;
};

/// Screen rectangles in NDC: x0, y0, x1, y1.
using Rect = glm::vec4;
constexpr Rect kWholeScreen{-1.0f, -1.0f, 1.0f, 1.0f};

inline float planeDistance(const Portal& p, const glm::vec3& at) {
    return glm::dot(p.normal, at) + p.distance;
}

inline bool usable(const Building& b, uint32_t portal) {
    return portal < b.portals.size() && b.portals[portal].vertexCount >= 3 &&
           static_cast<size_t>(b.portals[portal].startVertex) + b.portals[portal].vertexCount <= b.vertices.size();
}

/// Whether `at` lies over the portal's polygon, looked at along the axis its
/// plane faces most (0x009829b0, 0x009830d0).
inline bool overPolygon(const Building& b, const Portal& p, const glm::vec3& at) {
    const glm::vec3 n = glm::abs(p.normal);
    int u = 0, v = 1;
    if (n.x > n.y && n.x > n.z) {
        u = 1;
        v = 2;
    } else if (n.y >= n.x && n.y > n.z) {
        v = 2;
    }
    bool in = false;
    for (uint32_t i = 0, j = p.vertexCount - 1; i < p.vertexCount; j = i++) {
        const glm::vec3& a = b.vertices[p.startVertex + i];
        const glm::vec3& c = b.vertices[p.startVertex + j];
        if ((a[v] > at[v]) == (c[v] > at[v])) continue;
        if (at[u] < (c[u] - a[u]) * (at[v] - a[v]) / (c[v] - a[v]) + a[u]) in = !in;
    }
    return in;
}

/// The portal's outline on screen: its polygon cut to the view and measured
/// (0x007a85e0, 0x007a72a0, 0x007a6b90). Corners behind the eye are cut off,
/// not taken for "anywhere": a door behind the camera shows nothing, and one
/// it stands in shows what is ahead.
inline std::optional<Rect> screenRect(const Building& b, const Portal& p, const glm::mat4& modelViewProj) {
    std::vector<glm::vec4> poly(p.vertexCount), cut;
    for (uint32_t i = 0; i < p.vertexCount; ++i) {
        poly[i] = modelViewProj * glm::vec4(b.vertices[p.startVertex + i], 1.0f);
    }
    // The eye's own plane first, so that what is closer than the near plane
    // still counts; then the four sides.
    const std::array<glm::vec4, 5> planes{glm::vec4(0, 0, 0, 1), glm::vec4(1, 0, 0, 1), glm::vec4(-1, 0, 0, 1),
                                          glm::vec4(0, 1, 0, 1), glm::vec4(0, -1, 0, 1)};
    for (size_t k = 0; k < planes.size(); ++k) {
        const float bias = k == 0 ? -1e-3f : 0.0f;
        cut.clear();
        for (size_t i = 0; i < poly.size(); ++i) {
            const glm::vec4& a = poly[i];
            const glm::vec4& c = poly[(i + 1) % poly.size()];
            const float da = glm::dot(planes[k], a) + bias;
            const float dc = glm::dot(planes[k], c) + bias;
            if (da >= 0.0f) cut.push_back(a);
            if ((da >= 0.0f) != (dc >= 0.0f)) cut.push_back(a + (c - a) * (da / (da - dc)));
        }
        poly.swap(cut);
        if (poly.empty()) return std::nullopt;
    }
    Rect r(1e30f, 1e30f, -1e30f, -1e30f);
    for (const glm::vec4& c : poly) {
        const float w = std::max(c.w, 1e-4f);
        r.x = std::min(r.x, c.x / w);
        r.y = std::min(r.y, c.y / w);
        r.z = std::max(r.z, c.x / w);
        r.w = std::max(r.w, c.y / w);
    }
    return r;
}

/// The portal straight below `eye` (model space), no farther than `reach`,
/// and the groups on its two sides, the eye's first (0x007af280).
struct PortalBelow {
    float distance = 0.0f;
    uint32_t here = 0;
    uint32_t past = 0;
};
inline std::optional<PortalBelow> portalBelow(const Building& b, const glm::vec3& eye, float reach) {
    std::optional<PortalBelow> best;
    for (uint32_t g = 0; g < b.groupRefs.size(); ++g) {
        const auto [first, count] = b.groupRefs[g];
        for (uint32_t r = first; r < static_cast<uint32_t>(first) + count && r < b.refs.size(); ++r) {
            const Ref& ref = b.refs[r];
            if (!usable(b, ref.portalIndex) || ref.groupIndex >= b.flags.size()) continue;
            const Portal& p = b.portals[ref.portalIndex];
            if (std::abs(p.normal.z) < 1e-4f) continue;
            const float side = planeDistance(p, eye);
            const float t = side / p.normal.z;
            if (t < 0.0f || t > reach || (best && t >= best->distance)) continue;
            if (!overPolygon(b, p, eye - glm::vec3(0.0f, 0.0f, t))) continue;
            // The owner is on the plane's `side`; the eye with it, or not.
            const bool owner = (ref.side > 0) == (side > 0.0f);
            best = owner ? PortalBelow{t, g, ref.groupIndex} : PortalBelow{t, ref.groupIndex, g};
        }
    }
    return best;
}

/// The groups the camera is in (0x007d59b0): the one the first face below it
/// belongs to - `floorGroup`, `floorDistance` down - unless a portal lies
/// before that face, when they are the two it joins, the far one only where
/// it is a room. None when the camera's own is an exterior group: outside.
inline std::vector<uint32_t> cameraGroups(const Building& b, const glm::vec3& eye, uint32_t floorGroup,
                                          float floorDistance) {
    uint32_t here = floorGroup;
    std::optional<uint32_t> past;
    if (const auto p = portalBelow(b, eye, floorDistance)) {
        here = p->here;
        if (p->past < b.flags.size() && (b.flags[p->past] & kExterior) == 0) past = p->past;
    }
    if (here >= b.flags.size() || (b.flags[here] & kExterior) != 0) return {};
    std::vector<uint32_t> out{here};
    if (past && *past != here) out.push_back(*past);
    return out;
}

namespace detail {
struct Walk {
    const Building& b;
    const glm::vec3& eye;
    const glm::mat4& modelViewProj;
    std::vector<std::optional<std::optional<Rect>>> rects;
    std::optional<Rect> outside;
    int budget = 4096;
    /// Where given, every group the walk reaches and the part of the screen
    /// it is reached in, the rectangles a group is reached through joined.
    std::vector<std::optional<Rect>>* reached = nullptr;

    void addOutside(const Rect& r) {
        outside = outside ? Rect(std::min(outside->x, r.x), std::min(outside->y, r.y), std::max(outside->z, r.z),
                                 std::max(outside->w, r.w))
                          : r;
    }

    /// Where the camera sees the portal from its owner's side, if it does.
    std::optional<Rect> seen(const Ref& ref) {
        const Portal& p = b.portals[ref.portalIndex];
        const float plane = planeDistance(p, eye);
        const float side = ref.side < 0 ? -plane : plane;
        // At a portal the whole view lies through it: within 0.01 of its
        // plane (0x007a7210), and up to a quarter yard past it, where the
        // face below is still the room's and nothing of the door is ahead.
        if (side < 0.01f && side > -0.25f && overPolygon(b, p, eye)) return kWholeScreen;
        if (side < 0.0f) return std::nullopt;
        auto& known = rects[ref.portalIndex];
        if (!known) known = screenRect(b, p, modelViewProj);
        return *known;
    }

    void from(uint32_t group, uint32_t parent, const Rect& view, int depth) {
        if (depth > kMaxDepth || group >= b.groupRefs.size()) return;
        if (reached && group < reached->size()) {
            auto& r = (*reached)[group];
            r = r ? Rect(std::min(r->x, view.x), std::min(r->y, view.y), std::max(r->z, view.z), std::max(r->w, view.w))
                  : view;
        }
        if ((b.flags[group] & kAlways) != 0) return;
        const auto [first, count] = b.groupRefs[group];
        for (uint32_t r = first; r < static_cast<uint32_t>(first) + count && r < b.refs.size(); ++r) {
            const Ref& ref = b.refs[r];
            if (ref.groupIndex == parent || ref.groupIndex >= b.flags.size() || !usable(b, ref.portalIndex)) continue;
            // A tangle of portals: taken for open rather than lose a group.
            if (--budget < 0) {
                addOutside(kWholeScreen);
                return;
            }
            const auto rect = seen(ref);
            if (!rect) continue;
            const Rect through(std::max(rect->x, view.x), std::max(rect->y, view.y), std::min(rect->z, view.z),
                               std::min(rect->w, view.w));
            constexpr float kNone = 0.001f;  // 0x3a83126f
            if (through.z - through.x <= kNone || through.w - through.y <= kNone) continue;
            const uint32_t flags = b.flags[ref.groupIndex];
            // The portal's own outline, not what is left of it in the view
            // through the ones before it (0x007a8f20).
            if ((flags & kShowsOutside) != 0) addOutside(*rect);
            if ((flags & (kExterior | kAlways)) == 0) from(ref.groupIndex, group, through, depth + 1);
        }
    }
};
}  // namespace detail

/// The part of the screen the outside shows in, from a camera at `eye` (model
/// space) in `groups`: nothing when no exit is in sight.
inline std::optional<Rect> outsideRect(const Building& b, std::span<const uint32_t> groups, const glm::vec3& eye,
                                       const glm::mat4& modelViewProj) {
    detail::Walk walk{b, eye, modelViewProj, {}, {}};
    walk.rects.resize(b.portals.size());
    for (uint32_t g : groups) {
        if (g >= b.flags.size()) continue;
        walk.from(g, 0xffffffffu, kWholeScreen, 0);
    }
    return walk.outside;
}

/// The groups a camera at `eye` (model space) sees, walking from `starts`,
/// each with the part of the screen it is seen in, none for one not seen.
/// From the camera's rooms (0x007ad1f0), or, for a camera outside the
/// building, from its exterior groups in view (0x007ad350 walks in from
/// each): a room is seen only through portals in sight.
inline std::vector<std::optional<Rect>> groupsSeen(const Building& b, std::span<const uint32_t> starts,
                                                   const glm::vec3& eye, const glm::mat4& modelViewProj) {
    std::vector<std::optional<Rect>> seen(b.flags.size());
    detail::Walk walk{b, eye, modelViewProj, {}, {}};
    walk.rects.resize(b.portals.size());
    walk.reached = &seen;
    for (uint32_t g : starts) {
        if (g >= b.flags.size()) continue;
        walk.from(g, 0xffffffffu, kWholeScreen, 0);
    }
    return seen;
}

/// Whether a building's doodad is drawn: the client draws a doodad with the
/// groups whose MODR lists it (0x007ac060 walks a group, its doodads with
/// it), so a room's doodad only where that room is seen and, within the
/// part of the screen the room is seen in (`inRect(group, rect)`), only there. The
/// outside's groups (0x8) and the always drawn ones (0x10000) are drawn as
/// the outside is; a doodad no group lists, or one of a group the file
/// does not have, is drawn as before.
template <class InRect>
bool doodadSeen(std::span<const uint32_t> groups, std::span<const uint32_t> flags,
                const std::vector<std::optional<Rect>>& seen, InRect&& inRect) {
    if (groups.empty()) return true;
    for (uint32_t g : groups) {
        if (g >= flags.size() || (flags[g] & kShowsOutside) != 0) return true;
        if (g < seen.size() && seen[g] && inRect(g, *seen[g])) return true;
    }
    return false;
}

/// The view-projection that sees `r` of the screen and nothing past it
/// (0x00790e20's frustum).
inline glm::mat4 throughRect(const Rect& r, const glm::mat4& viewProj) {
    const float w = std::max(r.z - r.x, 1e-4f);
    const float h = std::max(r.w - r.y, 1e-4f);
    // x' = (2x - (x0 + x1) w) / (x1 - x0), and y the same: the offset goes
    // with the clip w.
    glm::mat4 cut(1.0f);
    cut[0][0] = 2.0f / w;
    cut[1][1] = 2.0f / h;
    cut[3][0] = -(r.x + r.z) / w;
    cut[3][1] = -(r.y + r.w) / h;
    return cut * viewProj;
}

}  // namespace wowee::rendering::wmo_portal_walk

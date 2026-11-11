#pragma once

// The loading screen of a ship or zeppelin that crosses to another continent
// (LoadingScreen.cpp, 0x0040ad50 from SMSG_TRANSFER_PENDING's transport
// branch, 0x0040ae30 from the transport's own leg change): instead of the
// map's picture and the bar, the Azeroth map's twelve tiles
// (Interface\WorldMap\World\World1..12) and on them the crossing - a dot
// trail along a Catmull-Rom spline from the dock left to the dock arrived
// at, through the LoadingScreenTaxiSplines row's points, filling in with the
// load's progress, a marker at each end (0x00409880, 0x00408e10).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace wowee::rendering::loading_screen_route {

/// A point in the 1002x668 Azeroth map frame as 0..1, y up.
struct Point {
    float x = 0.0f, y = 0.0f;
};

/// A TaxiPathNode row as the leg table reads it (+0, +4, +0xc, +0x10,
/// +0x14, +0x1c), in file order.
struct PathNode {
    uint32_t id = 0;
    uint32_t pathId = 0;
    uint32_t mapId = 0;
    float x = 0.0f, y = 0.0f;   // wire X and Y
    uint32_t flags = 0;
};

/// A path's leg: its first and last stop node's id (0 where it had none).
struct Leg {
    uint32_t firstStop = 0;
    uint32_t lastStop = 0;
};

/// 0x0040b0b0 for one path: walking every node in file order, a stop node
/// (flag 2) opens a leg or extends it; a teleport node (flag 1), a map change
/// within the path and the next path's first node close it. The last path in
/// the file keeps its open leg.
inline std::vector<Leg> legsOf(const std::vector<PathNode>& nodes, uint32_t pathId) {
    std::vector<Leg> legs;
    const PathNode* prev = nullptr;
    uint32_t first = 0, last = 0;
    for (const PathNode& n : nodes) {
        if (prev && prev->pathId != n.pathId) {
            if (first > 0 && prev->pathId == pathId) legs.push_back({first, last});
            first = 0;
        }
        if (n.flags & 1u) {
            if (n.pathId == pathId) legs.push_back({first, last});
            first = 0;
        } else if (prev && prev->pathId == n.pathId && prev->mapId != n.mapId) {
            if (n.pathId == pathId) legs.push_back({first, last});
            first = 0;
        } else if (n.flags & 2u) {
            last = n.id;
            if (first == 0) first = n.id;
        }
        prev = &n;
    }
    return legs;
}

/// The WorldMapContinent row's ContinentOffset and Scale (+0x18, +0x1c, +0x20).
struct Continent {
    float offsetX = 0.0f, offsetY = 0.0f, scale = 0.0f;
};

/// 0x005440e0: a place on a continent in the Azeroth map frame.
inline Point mapPoint(const Continent& c, float wireX, float wireY) {
    Point p;
    p.x = (c.offsetX * (16.0f / 1002.0f) + 0.5f) - wireY * (0.03f / 1002.0f) * c.scale;
    p.y = 1.0f - ((c.offsetY * (16.0f / 668.0f) + 0.5f) - wireX * (0.03f / 668.0f) * c.scale);
    return p;
}

/// A LoadingScreenTaxiSplines row: up to eight points between the docks,
/// ended by the first with neither coordinate above 0.
struct SplineRow {
    uint32_t pathId = 0;
    std::array<float, 8> x{}, y{};
    uint32_t legIndex = 0;
};

/// The Catmull-Rom spline the trail is laid along (0x004c3830 with the
/// curved flag 0x00409740 sets): points ends doubled, each segment's
/// length summed over twenty steps (0x004c3b10), positions found by the
/// share of the whole length (0x004c3870 mode 1, 0x004c3bd0).
class Spline {
public:
    explicit Spline(std::vector<Point> points) : p_(std::move(points)) {
        if (p_.size() < 4) return;
        for (size_t s = 0; s + 3 < p_.size(); ++s) {
            Point a = at(s, 0.0f);
            float len = 0.0f;
            for (int i = 1; i <= 20; ++i) {
                const Point b = at(s, 0.05f * static_cast<float>(i));
                len += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                a = b;
            }
            seg_.push_back(len);
            length_ += len;
        }
    }
    float length() const { return length_; }
    /// The place `t` of the way along.
    Point eval(float t) const {
        if (p_.empty()) return {};
        if (!(t > 0.0f)) return p_.front();
        if (t >= 1.0f || seg_.empty()) return p_.back();
        const float dist = length_ * t;
        size_t s = 0;
        float acc = 0.0f;
        if (seg_.size() >= 2) {
            for (;;) {
                const float next = acc + seg_[s];
                if (next > dist) break;
                acc = next;
                ++s;
                if (s + 1 >= seg_.size()) break;
            }
        }
        const float u = seg_[s] > 0.0f ? (dist - acc) / seg_[s] : 0.0f;
        return at(s, u);
    }

private:
    Point at(size_t s, float t) const {
        // The basis at 0x00ac37b8, a row per control point.
        static constexpr float m[4][4] = {{-0.5f, 1.0f, -0.5f, 0.0f},
                                          {1.5f, -2.5f, 0.0f, 1.0f},
                                          {-1.5f, 2.0f, 0.5f, 0.0f},
                                          {0.5f, -0.5f, 0.0f, 0.0f}};
        Point r;
        for (int j = 0; j < 4; ++j) {
            const float w = ((m[j][0] * t + m[j][1]) * t + m[j][2]) * t + m[j][3];
            r.x += w * p_[s + static_cast<size_t>(j)].x;
            r.y += w * p_[s + static_cast<size_t>(j)].y;
        }
        return r;
    }
    std::vector<Point> p_;
    std::vector<float> seg_;
    float length_ = 0.0f;
};

/// What the screen draws: the docks and the trail's dots, the k-th of the n+1
/// shown once the progress reaches k/n (0x00408e10).
struct Route {
    Point start, end;
    std::vector<Point> dots;
    size_t shownDots(float progress) const {
        if (dots.empty()) return 0;
        const size_t n = dots.size() - 1;
        if (n == 0) return 1;
        size_t k = 0;
        while (k < dots.size() && static_cast<float>(k) / static_cast<float>(n) <= progress) ++k;
        return k;
    }
};

/// 0x00409880: the crossing of path `pathId` from map `fromMap` - the first
/// leg whose first stop is on that map (0x00408cd0), from its last stop to
/// the next leg's first stop, through the row for that path and leg. Nothing
/// where any piece is missing: then the screen is the map's own.
///
/// `leftFrom`, where given (wire X and Y): the leg crossed from is the one
/// whose last stop is nearest it, on that map. A route that stays on one
/// map - the Grom'gol zeppelin, Tirisfal to Stranglethorn - has its every
/// leg's first stop there, and the client names the leg it left instead
/// (0x007134a0 hands 0x0040ae30 the transport's leg before the change).
inline std::optional<Route> build(const std::vector<PathNode>& nodes,
                                  const std::vector<SplineRow>& rows, uint32_t pathId,
                                  uint32_t fromMap,
                                  const std::function<std::optional<Continent>(uint32_t)>& continentOf,
                                  std::optional<Point> leftFrom = std::nullopt) {
    const std::vector<Leg> legs = legsOf(nodes, pathId);
    if (legs.empty()) return std::nullopt;
    auto node = [&](uint32_t id) -> const PathNode* {
        if (id == 0) return nullptr;
        for (const PathNode& n : nodes)
            if (n.id == id) return &n;
        return nullptr;
    };
    size_t leg = legs.size();
    float nearest = std::numeric_limits<float>::max();
    for (size_t i = 0; i < legs.size(); ++i) {
        if (legs[i].firstStop == 0 || legs[i].lastStop == 0) return std::nullopt;
        if (leftFrom) {
            const PathNode* last = node(legs[i].lastStop);
            if (!last || last->mapId != fromMap) continue;
            const float dx = last->x - leftFrom->x, dy = last->y - leftFrom->y;
            const float d = dx * dx + dy * dy;
            if (d < nearest) { nearest = d; leg = i; }
            continue;
        }
        const PathNode* n = node(legs[i].firstStop);
        if (n && n->mapId == fromMap) { leg = i; break; }
    }
    if (leg == legs.size()) return std::nullopt;
    const SplineRow* row = nullptr;
    for (const SplineRow& r : rows) {
        if (r.pathId == pathId && r.legIndex == leg) { row = &r; break; }
    }
    if (!row) return std::nullopt;
    const Leg& next = legs[(leg + 1) % legs.size()];
    const PathNode* from = node(legs[leg].lastStop);
    const PathNode* to = node(next.firstStop);
    if (!from || !to || next.lastStop == 0) return std::nullopt;
    const auto fromContinent = continentOf ? continentOf(from->mapId) : std::nullopt;
    const auto toContinent = continentOf ? continentOf(to->mapId) : std::nullopt;
    if (!fromContinent || !toContinent) return std::nullopt;

    Route route;
    route.start = mapPoint(*fromContinent, from->x, from->y);
    route.end = mapPoint(*toContinent, to->x, to->y);
    std::vector<Point> points{route.start, route.start};
    for (size_t i = 0; i < row->x.size(); ++i) {
        if (row->x[i] <= 0.0f && row->y[i] <= 0.0f) break;
        points.push_back({row->x[i], row->y[i]});
    }
    points.push_back(route.end);
    points.push_back(route.end);
    const Spline spline(std::move(points));
    long n = std::lround(spline.length() * 66.66667f);
    if (n < 1) n = 1;
    route.dots.reserve(static_cast<size_t>(n) + 1);
    const float step = 1.0f / static_cast<float>(n);
    float t = 0.0f;
    for (long k = 0; k <= n; ++k) {
        route.dots.push_back(spline.eval(t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t)));
        t += step;
    }
    return route;
}

/// The trail's pieces in DynamicElements: a quarter each, and their half
/// sizes in the screen's 0..1 (0x00ab6420, 0x00408690).
struct Sprite {
    float u0, v0, u1, v1, halfW, halfH;
};
inline constexpr Sprite kDot{0.0f, 0.5f, 0.5f, 1.0f, 0.01f, 0.015f};
inline constexpr Sprite kStart{0.5f, 0.0f, 1.0f, 0.5f, 0.02f, 0.0265f};
inline constexpr Sprite kEnd{0.0f, 0.0f, 0.5f, 0.5f, 0.02f, 0.0265f};
inline constexpr const char* kDynamicElements = "Interface\\Glues\\LoadingScreens\\DynamicElements";

/// One of the twelve tiles (0x00407c50): 256 pixels of the 1002x668 frame,
/// the last column and row cut where the frame ends.
struct Tile {
    float x0, x1, yTop, yBottom, uMax, vMax;
};
inline Tile tile(int col, int row) {
    Tile t;
    const int right = std::min((col + 1) * 256, 1002);
    const int bottom = std::min((row + 1) * 256, 668);
    t.x0 = static_cast<float>(col * 256) / 1002.0f;
    t.x1 = static_cast<float>(right) / 1002.0f;
    t.yTop = 1.0f - static_cast<float>(row * 256) / 668.0f;
    t.yBottom = 1.0f - static_cast<float>(bottom) / 668.0f;
    t.uMax = col == 3 ? 0.9140625f : 1.0f;
    t.vMax = row == 2 ? 0.609375f : 1.0f;
    return t;
}

}  // namespace wowee::rendering::loading_screen_route

#include "mcp/walk_planner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <queue>

namespace wowee::mcp {

namespace {

/// cos(50): steeper is slid down, not walked up.
constexpr float kWalkable = 0.6427876f;
constexpr float kDegrees = 57.29578f;
constexpr float kKnee = 0.9f;
constexpr float kChest = 1.7f;

int cellOf(float v, float cell) {
    return static_cast<int>(std::floor(v / cell));
}

/// A place on the grid: the cell, and which of the floors over it.
struct NodeKey {
    int x = 0;
    int y = 0;
    int floor = 0;
    bool operator==(const NodeKey&) const = default;
};

struct NodeHash {
    size_t operator()(const NodeKey& k) const {
        const auto mix = static_cast<uint64_t>(static_cast<uint32_t>(k.x)) * 0x9E3779B97F4A7C15ULL ^
                         static_cast<uint64_t>(static_cast<uint32_t>(k.y)) * 0xC2B2AE3D27D4EB4FULL ^
                         static_cast<uint64_t>(static_cast<uint32_t>(k.floor)) * 0x165667B19E3779F9ULL;
        return static_cast<size_t>(mix ^ (mix >> 29));
    }
};

struct Node {
    glm::vec3 at{0.0f};
    float cost = 0.0f;
    NodeKey from;
    bool hasFrom = false;
    bool closed = false;
};

float flat(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a) - glm::vec2(b));
}

/// How far a place is from the goal, its height counted less: a goal's own is
/// often not known to the yard.
float leftTo(const glm::vec3& at, const glm::vec3& goal) {
    const float dz = at.z - goal.z;
    return std::sqrt(flat(at, goal) * flat(at, goal) + 0.25f * dz * dz);
}

/// Whether a step of the body from `a` to the ground at `b` can be taken.
bool stepOpen(const FacetIndex& world, const glm::vec3& a, const glm::vec2& b, const WalkLimits& limits,
              glm::vec3& landed) {
    const auto ground = world.groundAt(b.x, b.y, a.z, limits.climb);
    if (!ground) return false;
    const float rise = ground->z - a.z;
    if (rise > limits.climb || -rise > limits.drop) return false;
    if (rise > 0.3f && std::cos(ground->slopeDegrees / kDegrees) < kWalkable) return false;
    // At the higher of the two: walls are met where the body ends up.
    const glm::vec3 feet(a.x, a.y, std::max(a.z, ground->z - 0.2f));
    landed = glm::vec3(b, ground->z);
    for (const auto& p : limits.avoid) {
        // Out of one of them is let, into one is not.
        if (std::fabs(p.z - landed.z) < 3.0f && flat(p, landed) < limits.avoidRadius && flat(p, landed) < flat(p, a)) {
            return false;
        }
    }
    return world.clear(feet, glm::vec3(b, feet.z), limits.bodyRadius, limits.bodyHeight);
}

/// Whether the straight line can be walked, a yard at a time.
bool lineOpen(const FacetIndex& world, const glm::vec3& a, const glm::vec3& b, const WalkLimits& limits) {
    const float length = flat(a, b);
    const int steps = std::max(1, static_cast<int>(std::ceil(length)));
    glm::vec3 at = a;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const glm::vec2 next = glm::mix(glm::vec2(a), glm::vec2(b), t);
        glm::vec3 landed(0.0f);
        if (!stepOpen(world, at, next, limits, landed)) return false;
        // Off a ledge is not a line one would walk for a shorter way.
        if (at.z - landed.z > 1.6f) return false;
        at = landed;
    }
    return std::fabs(at.z - b.z) < 1.5f;
}

}  // namespace

FacetIndex::FacetIndex(std::vector<probe::Facet> facets, float cell) : facets_(std::move(facets)), cell_(cell) {
    for (uint32_t i = 0; i < facets_.size(); ++i) {
        const auto& f = facets_[i];
        const int x0 = cellOf(std::min({f.v[0].x, f.v[1].x, f.v[2].x}), cell_);
        const int x1 = cellOf(std::max({f.v[0].x, f.v[1].x, f.v[2].x}), cell_);
        const int y0 = cellOf(std::min({f.v[0].y, f.v[1].y, f.v[2].y}), cell_);
        const int y1 = cellOf(std::max({f.v[0].y, f.v[1].y, f.v[2].y}), cell_);
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) cells_[key(x, y)].push_back(i);
        }
    }
}

int64_t FacetIndex::key(int cx, int cy) const {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
}

void FacetIndex::gather(const glm::vec2& lo, const glm::vec2& hi, std::vector<probe::Facet>& out) const {
    out.clear();
    for (int x = cellOf(lo.x, cell_); x <= cellOf(hi.x, cell_); ++x) {
        for (int y = cellOf(lo.y, cell_); y <= cellOf(hi.y, cell_); ++y) {
            const auto it = cells_.find(key(x, y));
            if (it == cells_.end()) continue;
            for (const uint32_t i : it->second) out.push_back(facets_[i]);
        }
    }
}

std::optional<probe::Ground> FacetIndex::groundAt(float x, float y, float feet, float climb) const {
    thread_local std::vector<probe::Facet> some;
    gather({x, y}, {x, y}, some);
    return probe::groundAt(some, x, y, feet, climb);
}

bool FacetIndex::clear(const glm::vec3& feet, const glm::vec3& to, float bodyRadius, float bodyHeight) const {
    thread_local std::vector<probe::Facet> some;
    const glm::vec2 a(feet);
    const glm::vec2 b(to);
    const float length = glm::length(b - a);
    if (length < 1e-3f) return true;
    const glm::vec2 dir = (b - a) / length;
    const glm::vec2 side(-dir.y, dir.x);
    const glm::vec2 pad(bodyRadius);
    gather(glm::min(a, b) - pad, glm::max(a, b) + pad, some);
    const auto open = [&](float lateral, float height) {
        const glm::vec3 from(a + side * lateral, feet.z + height);
        const auto hit = probe::castRay(some, from, glm::vec3(dir, 0.0f), length + bodyRadius * 0.5f);
        return !hit || hit->normal.z >= kWalkable;
    };
    for (const float lateral : {0.0f, bodyRadius, -bodyRadius}) {
        if (!open(lateral, kKnee) || !open(lateral, kChest)) return false;
        // What leans over the way, or hangs into it.
        for (float height = kChest + 0.8f; height < bodyHeight; height += 0.8f) {
            if (!open(lateral, height)) return false;
        }
    }
    return true;
}

std::string FacetIndex::blockedBy(const glm::vec3& feet, const glm::vec3& to, float bodyRadius,
                                  float bodyHeight) const {
    thread_local std::vector<probe::Facet> some;
    const glm::vec2 a(feet);
    const glm::vec2 b(to);
    const float length = glm::length(b - a);
    if (length < 1e-3f) return {};
    const glm::vec2 dir = (b - a) / length;
    const glm::vec2 side(-dir.y, dir.x);
    gather(glm::min(a, b) - glm::vec2(bodyRadius), glm::max(a, b) + glm::vec2(bodyRadius), some);
    std::vector<float> heights{kKnee, kChest};
    for (float height = kChest + 0.8f; height < bodyHeight; height += 0.8f) heights.push_back(height);
    for (const float lateral : {0.0f, bodyRadius, -bodyRadius}) {
        for (const float height : heights) {
            const glm::vec3 from(a + side * lateral, feet.z + height);
            const auto hit = probe::castRay(some, from, glm::vec3(dir, 0.0f), length + bodyRadius * 0.5f);
            if (hit && hit->normal.z < kWalkable) {
                const char* what = hit->kind == probe::Facet::Kind::Ground     ? "ground"
                                   : hit->kind == probe::Facet::Kind::Building ? "building"
                                   : hit->kind == probe::Facet::Kind::Doodad   ? "doodad"
                                                                               : "face";
                char text[128];
                std::snprintf(text, sizeof text, "blocked by a %s (owner %u) at %.1f yd high, %.1f yd aside, %.2f yd ahead",
                              what, static_cast<unsigned>(hit->owner), height, lateral, hit->distance);
                return text;
            }
        }
    }
    return {};
}

std::string whyNoStep(const FacetIndex& world, const glm::vec3& a, const glm::vec2& b, const WalkLimits& limits) {
    char text[96];
    const auto ground = world.groundAt(b.x, b.y, a.z, limits.climb);
    if (!ground) return "no ground";
    const float rise = ground->z - a.z;
    if (rise > limits.climb) {
        std::snprintf(text, sizeof text, "rises %.1f yd", rise);
        return text;
    }
    if (-rise > limits.drop) {
        std::snprintf(text, sizeof text, "drops %.1f yd", -rise);
        return text;
    }
    if (rise > 0.3f && std::cos(ground->slopeDegrees / kDegrees) < kWalkable) {
        std::snprintf(text, sizeof text, "too steep (%.0f degrees)", ground->slopeDegrees);
        return text;
    }
    const glm::vec3 landed(b, ground->z);
    for (const auto& p : limits.avoid) {
        if (std::fabs(p.z - landed.z) < 3.0f && flat(p, landed) < limits.avoidRadius && flat(p, landed) < flat(p, a)) {
            return "a place stuck at before";
        }
    }
    const glm::vec3 feet(a.x, a.y, std::max(a.z, ground->z - 0.2f));
    return world.blockedBy(feet, glm::vec3(b, feet.z), limits.bodyRadius, limits.bodyHeight);
}

WalkPlan planWalk(const FacetIndex& world, const glm::vec3& from, const glm::vec3& to, const WalkLimits& limits) {
    WalkPlan plan;
    plan.left = leftTo(from, to);
    if (world.empty()) return plan;
    // Near enough in a straight line: no grid for it.
    if (lineOpen(world, from, to, limits)) {
        plan.points.push_back(to);
        plan.reaches = true;
        plan.left = 0.0f;
        return plan;
    }

    std::unordered_map<NodeKey, Node, NodeHash> nodes;
    using Open = std::pair<float, NodeKey>;
    const auto later = [](const Open& a, const Open& b) { return a.first > b.first; };
    std::priority_queue<Open, std::vector<Open>, decltype(later)> open(later);
    const auto floorOf = [](float z) { return static_cast<int>(std::floor(z / 2.0f)); };
    // The grid is laid on the start, so that its own cell's middle is under
    // the feet.
    const NodeKey start{.x = 0, .y = 0, .floor = floorOf(from.z)};
    nodes[start].at = from;
    open.emplace(leftTo(from, to), start);
    NodeKey best = start;
    float bestLeft = plan.left;
    while (!open.empty() && plan.visited < limits.nodes) {
        const NodeKey k = open.top().second;
        open.pop();
        Node& node = nodes[k];
        if (node.closed) continue;
        node.closed = true;
        ++plan.visited;
        const glm::vec3 at = node.at;
        const float cost = node.cost;
        const float left = leftTo(at, to);
        if (left < bestLeft) {
            bestLeft = left;
            best = k;
        }
        if (flat(at, to) <= limits.arrive && std::fabs(at.z - to.z) < 4.0f) {
            plan.reaches = true;
            best = k;
            break;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0) continue;
                const glm::vec2 p(from.x + static_cast<float>(k.x + dx) * limits.cell,
                                  from.y + static_cast<float>(k.y + dy) * limits.cell);
                glm::vec3 landed(0.0f);
                if (!stepOpen(world, at, p, limits, landed)) continue;
                const NodeKey next{.x = k.x + dx, .y = k.y + dy, .floor = floorOf(landed.z)};
                const float rise = landed.z - at.z;
                // A drop costs, for it cannot be gone back up.
                const float price = limits.cell * (dx != 0 && dy != 0 ? 1.41421f : 1.0f) +
                                    std::fabs(rise) * 0.5f + (rise < -1.6f ? 6.0f : 0.0f);
                const float total = cost + price;
                auto it = nodes.find(next);
                if (it != nodes.end() && (it->second.closed || it->second.cost <= total)) continue;
                nodes[next] = Node{.at = landed, .cost = total, .from = k, .hasFrom = true, .closed = false};
                open.emplace(total + leftTo(landed, to), next);
            }
        }
    }

    std::vector<glm::vec3> points;
    for (NodeKey k = best;;) {
        const Node& n = nodes[k];
        points.push_back(n.at);
        if (!n.hasFrom) break;
        k = n.from;
    }
    std::reverse(points.begin(), points.end());
    plan.left = plan.reaches ? 0.0f : bestLeft;
    // Corners only: from each point on to the last that can be walked to
    // straight.
    size_t i = 0;
    while (i + 1 < points.size()) {
        size_t j = std::min(points.size() - 1, i + 12);
        while (j > i + 1 && !lineOpen(world, points[i], points[j], limits)) --j;
        plan.points.push_back(points[j]);
        i = j;
    }
    if (plan.reaches) plan.points.push_back(to);
    return plan;
}

}  // namespace wowee::mcp

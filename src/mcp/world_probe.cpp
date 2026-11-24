#include "mcp/world_probe.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::mcp::probe {

namespace {

constexpr float kDegrees = 57.29578f;
/// cos(50): a face steeper than this is not walked up (movement_limits.hpp).
constexpr float kWalkable = 0.6427876f;

/// Where a vertical line through (x, y) meets the face, if it does.
std::optional<float> heightOn(const Facet& f, float x, float y) {
    // Nothing under a face seen edge on.
    if (std::fabs(f.n.z) < 1e-4f) return std::nullopt;
    const auto side = [&](const glm::vec3& a, const glm::vec3& b) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    const float s0 = side(f.v[0], f.v[1]);
    const float s1 = side(f.v[1], f.v[2]);
    const float s2 = side(f.v[2], f.v[0]);
    constexpr float kEdge = 1e-4f;
    const bool in = (s0 >= -kEdge && s1 >= -kEdge && s2 >= -kEdge) || (s0 <= kEdge && s1 <= kEdge && s2 <= kEdge);
    if (!in) return std::nullopt;
    return -(f.n.x * x + f.n.y * y + f.d) / f.n.z;
}

float slopeOf(const Facet& f) {
    return std::acos(std::clamp(f.n.z, -1.0f, 1.0f)) * kDegrees;
}

/// Whether `a` is the better one to stand on than `b` for feet at `feet`.
bool better(const Ground& a, const std::optional<Ground>& b, float feet, float climb) {
    if (!b) return true;
    const bool aUnder = a.z <= feet + climb;
    const bool bUnder = b->z <= feet + climb;
    if (aUnder != bUnder) return aUnder;
    return aUnder ? a.z > b->z : a.z < b->z;
}

}  // namespace

std::optional<Hit> castRay(const std::vector<Facet>& facets, const glm::vec3& origin, const glm::vec3& direction,
                           float reach) {
    std::optional<Hit> best;
    for (const auto& f : facets) {
        // Into its front only, as 0x0075f9d0 has it (facesMotion).
        if (!rendering::collision::facesMotion(f, direction)) continue;
        const glm::vec3 e1 = f.v[1] - f.v[0];
        const glm::vec3 e2 = f.v[2] - f.v[0];
        const glm::vec3 p = glm::cross(direction, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-8f) continue;
        const float inverse = 1.0f / det;
        const glm::vec3 s = origin - f.v[0];
        const float u = glm::dot(s, p) * inverse;
        if (u < -1e-5f || u > 1.0f + 1e-5f) continue;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(direction, q) * inverse;
        if (v < -1e-5f || u + v > 1.0f + 1e-5f) continue;
        const float t = glm::dot(e2, q) * inverse;
        if (t < 0.0f || t > reach || (best && t >= best->distance)) continue;
        best = Hit{.distance = t, .normal = f.n, .kind = f.kind};
    }
    return best;
}

std::optional<Ground> groundAt(const std::vector<Facet>& facets, float x, float y, float feet, float climb) {
    std::optional<Ground> best;
    for (const auto& f : facets) {
        if (f.n.z <= 0.05f) continue;
        const auto z = heightOn(f, x, y);
        if (!z) continue;
        const Ground g{.z = *z, .slopeDegrees = slopeOf(f), .kind = f.kind};
        if (better(g, best, feet, climb)) best = g;
    }
    return best;
}

Grid groundGrid(const std::vector<Facet>& facets, const glm::vec3& middle, float radius, float step, float climb) {
    Grid grid;
    const int half = std::max(1, static_cast<int>(std::floor(radius / step + 0.001f)));
    grid.side = half * 2 + 1;
    grid.step = step;
    grid.corner = glm::vec2(middle) + glm::vec2(static_cast<float>(half) * step);
    grid.points.resize(static_cast<size_t>(grid.side) * static_cast<size_t>(grid.side));
    for (const auto& f : facets) {
        if (f.n.z <= 0.05f) continue;
        const float loX = std::min({f.v[0].x, f.v[1].x, f.v[2].x});
        const float hiX = std::max({f.v[0].x, f.v[1].x, f.v[2].x});
        const float loY = std::min({f.v[0].y, f.v[1].y, f.v[2].y});
        const float hiY = std::max({f.v[0].y, f.v[1].y, f.v[2].y});
        // Rows run down in x and columns down in y.
        const int row0 = std::max(0, static_cast<int>(std::floor((grid.corner.x - hiX) / step)));
        const int row1 = std::min(grid.side - 1, static_cast<int>(std::ceil((grid.corner.x - loX) / step)));
        const int col0 = std::max(0, static_cast<int>(std::floor((grid.corner.y - hiY) / step)));
        const int col1 = std::min(grid.side - 1, static_cast<int>(std::ceil((grid.corner.y - loY) / step)));
        for (int row = row0; row <= row1; ++row) {
            for (int col = col0; col <= col1; ++col) {
                const glm::vec2 p = grid.place(row, col);
                const auto z = heightOn(f, p.x, p.y);
                if (!z) continue;
                const Ground g{.z = *z, .slopeDegrees = slopeOf(f), .kind = f.kind};
                auto& best = grid.points[static_cast<size_t>(row) * static_cast<size_t>(grid.side) +
                                         static_cast<size_t>(col)];
                if (better(g, best, middle.z, climb)) best = g;
            }
        }
    }
    return grid;
}

Way wayAlong(const std::vector<Facet>& facets, const glm::vec3& feet, const glm::vec2& direction, float reach,
             float drop) {
    constexpr float kStep = 1.0f;
    constexpr float kKnee = 0.9f;
    constexpr float kChest = 1.6f;
    constexpr float kClimb = 1.1f;  // what a step up reaches, and some
    const glm::vec3 along(direction, 0.0f);
    Way way;
    float z = feet.z;
    for (float gone = 0.0f; gone < reach; gone += kStep) {
        const glm::vec3 from = glm::vec3(feet.x, feet.y, z) + along * gone;
        for (const float height : {kKnee, kChest}) {
            const auto hit = castRay(facets, from + glm::vec3(0.0f, 0.0f, height), along, kStep);
            if (hit && hit->normal.z < kWalkable) {
                way.clear = gone + hit->distance;
                way.why = Way::Why::Wall;
                way.kind = hit->kind;
                return way;
            }
        }
        const glm::vec3 to = from + along * kStep;
        const auto ground = groundAt(facets, to.x, to.y, z, kClimb);
        if (!ground || z - ground->z > drop) {
            way.clear = gone;
            way.why = Way::Why::Drop;
            return way;
        }
        if (ground->z - z > kClimb) {
            way.clear = gone;
            way.why = Way::Why::Wall;
            way.kind = ground->kind;
            return way;
        }
        if (ground->z - z > 0.35f && std::cos(ground->slopeDegrees / kDegrees) < kWalkable) {
            way.clear = gone;
            way.why = Way::Why::Steep;
            way.kind = ground->kind;
            return way;
        }
        z = ground->z;
    }
    way.clear = reach;
    return way;
}

const char* kindName(Facet::Kind kind) {
    switch (kind) {
        case Facet::Kind::Ground: return "ground";
        case Facet::Kind::Building: return "building";
        case Facet::Kind::Doodad: return "doodad";
        case Facet::Kind::None: break;
    }
    return "?";
}

}  // namespace wowee::mcp::probe

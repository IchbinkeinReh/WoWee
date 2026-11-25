#pragma once

// Rooms the client lights as the open air: a group that is indoors (MOGP flag
// 0x2000) and lit by the zone (0x40), without vertex colours - Silvermoon's
// auction house, a third of the city's houses. Nothing is painted there, and
// what reaches it through the traced scene is what comes through its doors.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::pipeline {

[[nodiscard]] constexpr bool isOpenAirRoom(uint32_t groupFlags) {
    return (groupFlags & 0x2000u) != 0 && (groupFlags & 0x40u) != 0 && (groupFlags & 0x4u) == 0;
}

/// Which faces of such a group are inside: those that look down, and those
/// with more of the group over them. Kept as the height of what lies flat in
/// each square of a grid, so one pass over the triangles says it.
class RoofColumns {
public:
    RoofColumns(std::span<const glm::vec3> vertices, std::span<const uint16_t> indices) {
        if (vertices.empty() || indices.size() < 3) return;
        lo_ = hi_ = glm::vec2(vertices[0]);
        for (const glm::vec3& v : vertices) {
            lo_ = glm::min(lo_, glm::vec2(v));
            hi_ = glm::max(hi_, glm::vec2(v));
        }
        w_ = cells(hi_.x - lo_.x);
        h_ = cells(hi_.y - lo_.y);
        top_.assign(static_cast<size_t>(w_) * h_, std::numeric_limits<float>::lowest());
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3 &a = vertices[indices[i]], &b = vertices[indices[i + 1]], &c = vertices[indices[i + 2]];
            const glm::vec3 n = glm::cross(b - a, c - a);
            const float len = glm::length(n);
            if (!(len > 1e-6f) || std::fabs(n.z) < 0.3f * len) continue;
            const glm::vec3 lo = glm::min(a, glm::min(b, c)), hi = glm::max(a, glm::max(b, c));
            for (int y = cellY(lo.y); y <= cellY(hi.y); ++y)
                for (int x = cellX(lo.x); x <= cellX(hi.x); ++x) {
                    float& t = top_[static_cast<size_t>(y) * w_ + x];
                    t = std::max(t, hi.z);
                }
        }
    }
    [[nodiscard]] bool inside(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& normal) const {
        if (top_.empty()) return false;
        if (normal.z < -0.3f) return true;
        const glm::vec3 m = (a + b + c) / 3.0f;
        return top_[static_cast<size_t>(cellY(m.y)) * w_ + cellX(m.x)] > m.z + kRoofOver;
    }

private:
    static constexpr float kCell = 2.0f;       ///< yards
    static constexpr float kRoofOver = 2.0f;   ///< less is a step or a moulding
    static int cells(float extent) { return std::clamp(static_cast<int>(extent / kCell) + 1, 1, 1024); }
    [[nodiscard]] int cellX(float x) const { return std::clamp(static_cast<int>((x - lo_.x) / kCell), 0, w_ - 1); }
    [[nodiscard]] int cellY(float y) const { return std::clamp(static_cast<int>((y - lo_.y) / kCell), 0, h_ - 1); }
    glm::vec2 lo_{0.0f}, hi_{0.0f};
    int w_ = 0, h_ = 0;
    std::vector<float> top_;
};

}  // namespace wowee::pipeline

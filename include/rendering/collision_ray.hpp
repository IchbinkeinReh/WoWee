#pragma once

#include "rendering/collision_facets.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::collision {

/// How far under a point a yard over the feet the ground is (0x00714b60): a ray from
/// there to a yard under them, two yards where it meets nothing.
[[nodiscard]] inline float groundUnderFeet(const glm::vec3& feet, const std::vector<Facet>& faces) {
    constexpr float kReach = 1.0f;
    float highest = feet.z - kReach;
    for (const Facet& f : faces) {
        if (f.n.z <= kFacingEpsilon) continue;
        // Whether the point is over the triangle, seen from above.
        const auto side = [&feet](const glm::vec3& a, const glm::vec3& b) {
            return (b.x - a.x) * (feet.y - a.y) - (b.y - a.y) * (feet.x - a.x);
        };
        const float s0 = side(f.v[0], f.v[1]), s1 = side(f.v[1], f.v[2]), s2 = side(f.v[2], f.v[0]);
        if (!((s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f))) continue;
        const float z = -(f.d + f.n.x * feet.x + f.n.y * feet.y) / f.n.z;
        if (z <= feet.z + kReach) highest = std::max(highest, z);
    }
    return feet.z + kReach - highest;
}

/// 0x0073a890: one that may fly and falls takes to the air with no ground within half
/// a yard under its feet; a flyer lands where there is, two seconds after it took off.
inline constexpr float kFlightGround = 1.5f;

}  // namespace wowee::rendering::collision

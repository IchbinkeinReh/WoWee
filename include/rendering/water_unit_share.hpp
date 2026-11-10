#pragma once

// How the traced water's rings (swim_wave.hpp) and trail points
// (WaterRenderer::updateWake) are shared out among the other units in the
// water, so a crowd out to 150 yards all disturb it: the character keeps its
// own reserve; the rest is split among the units, nearest first, a nearer one
// weighing more than a far one, each at least a few - and in a crowd too big
// for that, each at least the one ring and the two trail points that still
// show it, the few the pool gives every unit: the farthest go without only
// once there are more units than that. Not the client's (it has
// no traced water); with the tracing off every unit has its sprite ripples
// (SwimEffects, 0x0071cba0).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wowee::rendering::water_share {

/// What one unit may have alive at once.
struct UnitShare {
    size_t rings = 0;
    size_t wakePoints = 0;
};

inline constexpr size_t kMinRings = 3;        // a unit's fewest rings while there is room
inline constexpr size_t kFewestRings = 1;     // and in a crowd too big for that
inline constexpr size_t kMostRings = 30;      // and its most (three limbs, as the character's)
inline constexpr size_t kMinWakePoints = 6;   // a unit's shortest trail while there is room
inline constexpr size_t kFewestWakePoints = 2; // and in a crowd too big for that (a trail's two ends)
inline constexpr size_t kMostWakePoints = 32; // and its longest (a swimmer's natural one)
inline constexpr float kFullWeightTo = 30.0f; // yards within which a unit weighs fully
inline constexpr float kWeightAt150 = 0.4f;   // and what it weighs at 150 yards

/// How much a unit `yards` away weighs in the sharing.
inline float weightAt(float yards) {
    if (yards <= kFullWeightTo) return 1.0f;
    const float t = std::min(1.0f, (yards - kFullWeightTo) / (150.0f - kFullWeightTo));
    return 1.0f + (kWeightAt150 - 1.0f) * t;
}

namespace detail {
/// Each unit's part of `pool`: its weight's fair share, held between `most`
/// and the least every unit can have - `least`, or in a crowd too big for
/// that what the pool gives each, never under `fewest` - and none taken that
/// the units after it need for their least. Past `pool / fewest` units the
/// farthest go without.
inline std::vector<size_t> split(const std::vector<float>& weights, size_t pool, size_t least, size_t fewest,
                                 size_t most) {
    std::vector<size_t> out(weights.size(), 0);
    if (weights.empty()) return out;
    const size_t floor = std::max(fewest, std::min(least, pool / weights.size()));
    float left = 0.0f;
    for (float w : weights) left += w;
    for (size_t i = 0; i < weights.size(); ++i) {
        const size_t fair =
            left > 0.0f ? static_cast<size_t>(static_cast<float>(pool) * weights[i] / left) : 0;
        size_t n = std::clamp(fair, floor, std::max(floor, most));
        // Kept for the units after this one, each its least, while the pool
        // holds that many.
        const size_t after = weights.size() - 1 - i;
        const size_t kept = std::min(pool, after * floor);
        n = std::min(n, std::max(pool - kept, std::min(floor, pool)));
        n = std::min(n, pool);  // the farthest go without once it is spent
        out[i] = n;
        pool -= n;
        left -= weights[i];
    }
    return out;
}
}  // namespace detail

/// The shares of units `yards` away (nearest first) out of `ringPool` rings
/// and `wakePool` trail points. Together they never exceed the pools.
inline std::vector<UnitShare> shareOut(const std::vector<float>& yards, size_t ringPool, size_t wakePool) {
    std::vector<float> w;
    w.reserve(yards.size());
    for (float d : yards) w.push_back(weightAt(d));
    const auto rings = detail::split(w, ringPool, kMinRings, kFewestRings, kMostRings);
    const auto points = detail::split(w, wakePool, kMinWakePoints, kFewestWakePoints, kMostWakePoints);
    std::vector<UnitShare> out(yards.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = {rings[i], points[i]};
    return out;
}

/// A group of rings or trail points of one owner, for the water shader: the
/// circle that holds their effect and where they begin in the array. The
/// shader looks at a group's points only where the circle reaches, so a crowd
/// spread over the sea costs a pixel only its near owners' points.
struct Group {
    float x = 0.0f, y = 0.0f, radius = 0.0f;
    size_t first = 0;
};

/// Groups `n` points, already ordered by owner (`ownerOf(i)`), at `posOf(i)`
/// (anything with .x and .y), each reaching `reach` yards; at most `maxGroups`,
/// the owners past that in the last group.
template <class OwnerOf, class PosOf>
std::vector<Group> groupByOwner(size_t n, size_t maxGroups, float reach, OwnerOf ownerOf, PosOf posOf) {
    std::vector<Group> groups;
    if (n == 0 || maxGroups == 0) return groups;
    std::vector<size_t> ends;
    for (size_t i = 0; i < n; ++i) {
        if (i == 0 || (ownerOf(i) != ownerOf(i - 1) && groups.size() < maxGroups)) {
            if (!groups.empty()) ends.push_back(i);
            Group g;
            g.first = i;
            groups.push_back(g);
        }
    }
    ends.push_back(n);
    for (size_t k = 0; k < groups.size(); ++k) {
        Group& g = groups[k];
        float cx = 0.0f, cy = 0.0f;
        for (size_t i = g.first; i < ends[k]; ++i) {
            cx += posOf(i).x;
            cy += posOf(i).y;
        }
        const float count = static_cast<float>(ends[k] - g.first);
        cx /= count;
        cy /= count;
        float r2 = 0.0f;
        for (size_t i = g.first; i < ends[k]; ++i) {
            const float dx = posOf(i).x - cx, dy = posOf(i).y - cy;
            r2 = std::max(r2, dx * dx + dy * dy);
        }
        g.x = cx;
        g.y = cy;
        g.radius = std::sqrt(r2) + reach;
    }
    return groups;
}

}  // namespace wowee::rendering::water_share

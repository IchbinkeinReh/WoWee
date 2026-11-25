#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// What the achievements window has in a category (0x005b38e0, 0x005b3fc0).
namespace wowee::game::achievement_category {

constexpr uint32_t kStatistic = 0x1, kHidden = 0x2, kOnlyEarned = 0x800;
/// Feats of Strength: the earned ones alone.
constexpr int32_t kFeats = 81;
/// Every achievement, and every statistic.
constexpr int32_t kAll = -1, kAllStatistics = -2;

struct Row {
    uint32_t id = 0;
    int32_t faction = -1;     ///< -1 either side's
    uint32_t supercedes = 0;  ///< the step before it
    int32_t category = 0;
    uint32_t flags = 0;
    uint32_t order = 0;
};
/// Earned ones by the time they were, a later one the larger.
using Earned = std::unordered_map<uint32_t, uint32_t>;
/// The player's side as Achievement.dbc has it, -1 without a player (either side's),
/// and a side that is neither for one that has none.
using Side = int32_t;

/// The steps before one that is earned (0x005b37c0 asks it of the next).
[[nodiscard]] inline std::unordered_set<uint32_t> passedSteps(const std::vector<Row>& rows, const Earned& earned) {
    std::unordered_set<uint32_t> passed;
    for (const Row& r : rows)
        if (r.supercedes != 0 && earned.count(r.id) > 0) passed.insert(r.supercedes);
    return passed;
}

struct Count {
    uint32_t total = 0, earned = 0;
};
/// GetCategoryNumAchievements(category, all) over the rows as the file has them.
[[nodiscard]] inline Count count(const std::vector<Row>& rows, const Earned& earned, int32_t category, bool all, Side side) {
    const auto passed = passedSteps(rows, earned);
    Count c;
    uint32_t last = 0;
    for (const Row& r : rows) {
        const bool statistic = (r.flags & kStatistic) != 0;
        const bool every = (category == kAll && !statistic) || (category == kAllStatistics && statistic);
        const bool whole = every || all;
        if ((r.category != category && !every) || (r.flags & kHidden)) continue;
        if (side != -1 && r.faction != -1 && r.faction != side) continue;
        const bool done = earned.count(r.id) > 0;
        if ((r.category == kFeats || (r.flags & kOnlyEarned)) && !done) continue;
        if (last != 0 && last == r.supercedes) {
            // The step after the one that is open: not yet.
            last = r.id;
            if (whole) ++c.total;
        } else if (!done) {
            last = r.id;
            ++c.total;
        } else if (whole || passed.count(r.id) == 0) {
            ++c.total;
            ++c.earned;
        }
    }
    if (category == kFeats) c.total = c.earned;
    return c;
}

/// GetComparisonCategoryNumAchievements (0x005b3b30): what the compared player has of
/// a category, without the steps the player's own chain has gone past.
[[nodiscard]] inline uint32_t comparedCount(const std::vector<Row>& rows, const Earned& own, const Earned& theirs,
                                            int32_t category, Side theirSide) {
    const auto passed = passedSteps(rows, own);
    uint32_t n = 0;
    for (const Row& r : rows) {
        const bool statistic = (r.flags & kStatistic) != 0;
        const bool every = (category == kAll && !statistic) || (category == kAllStatistics && statistic);
        if ((r.category != category && !every) || (r.flags & kHidden)) continue;
        if (theirSide != -1 && r.faction != -1 && r.faction != theirSide) continue;
        if (theirs.count(r.id) > 0 && (every || passed.count(r.id) == 0)) ++n;
    }
    return n;
}

/// Whether the interface is told of an achievement at all (0x005b4561).
[[nodiscard]] inline bool shown(uint32_t flags, bool earned) {
    return (flags & kHidden) == 0 && ((flags & kOnlyEarned) == 0 || earned);
}

/// GetAchievementInfo(category, index): the achievement's id, 0 for none. The
/// earned ones first, the latest of them before all - and of one time the one with
/// the larger order (0x005b28d0) -, then the others as the file has them. A hidden
/// one among a category's is counted and not told of.
[[nodiscard]] inline uint32_t at(const std::vector<Row>& rows, const Earned& earned, int32_t category, int index, Side side) {
    if (index < 1) return 0;
    const auto passed = passedSteps(rows, earned);
    std::vector<std::pair<const Row*, uint32_t>> done;
    for (const Row& r : rows)
        if (const auto it = earned.find(r.id); it != earned.end()) done.emplace_back(&r, it->second);
    std::stable_sort(done.begin(), done.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first->order > b.first->order;
    });
    for (const auto& [r, when] : done) {
        const bool statistic = (r->flags & kStatistic) != 0;
        const bool every = (category == kAll && !statistic) || (category == kAllStatistics && statistic);
        if (every ? (r->flags & kHidden) == 0 : r->category == category && passed.count(r->id) == 0)
            if (--index == 0) return shown(r->flags, true) ? r->id : 0;
    }
    if (category == kFeats) return 0;
    uint32_t last = 0;
    for (const Row& r : rows) {
        if (side != -1 && r.faction != -1 && r.faction != side) continue;
        const bool statistic = (r.flags & kStatistic) != 0;
        if (category == kAll || category == kAllStatistics) {
            if (statistic != (category == kAllStatistics) || r.category == kFeats || (r.flags & (kHidden | kOnlyEarned))) continue;
            if (category == kAll && earned.count(r.id) > 0) continue;
            if (--index == 0) return r.id;
            continue;
        }
        if (r.category != category) continue;
        if (last != 0 && last == r.supercedes) {
            last = r.id;
            continue;
        }
        if (earned.count(r.id) > 0 || (r.flags & kOnlyEarned)) continue;
        last = r.id;
        if (--index == 0) return shown(r.flags, false) ? r.id : 0;
    }
    return 0;
}

}  // namespace wowee::game::achievement_category

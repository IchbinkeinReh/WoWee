#pragma once

// What the achievements window lists of a category, and how many it says there
// are (0x005b38e0 GetCategoryNumAchievements, 0x005b3fc0 GetAchievementInfo of
// a category and an index, 0x005b3b30 GetComparisonCategoryNumAchievements).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee::game {

/// Achievement.dbc's row as the lists read it, in the file's order.
struct AchievementRow {
    uint32_t id = 0;
    int32_t faction = -1;    ///< -1 both, 0 Horde, 1 Alliance
    uint32_t previous = 0;   ///< field 3, the one it follows
    uint32_t category = 0;
    uint32_t flags = 0;      ///< 1 a statistic, 2 not shown, 0x800 only once earned
    uint32_t order = 0;      ///< +0x20
};

constexpr uint32_t kFeatsOfStrength = 0x51;

/// One earned, with its time as the server packs it (minute, hour, weekday, day, month and
/// year from the low bits up) and its +0x20.
struct EarnedAchievement {
    uint32_t id = 0;
    uint32_t time = 0;
    uint32_t order = 0;
};
/// The ids, the latest first, and of one time the one with the larger order (0x005b28d0,
/// 0x005b2ab0).
[[nodiscard]] inline std::vector<uint32_t> newestFirst(std::vector<EarnedAchievement> earned) {
    const auto when = [](uint32_t packed) {
        return ((packed >> 24) & 0x1Fu) << 21 | ((packed >> 20) & 0xFu) << 17 | ((packed >> 14) & 0x3Fu) << 11 |
               ((packed >> 6) & 0x1Fu) << 6 | (packed & 0x3Fu);
    };
    std::sort(earned.begin(), earned.end(), [&](const EarnedAchievement& a, const EarnedAchievement& b) {
        if (const uint32_t wa = when(a.time), wb = when(b.time); wa != wb) return wa > wb;
        return a.order != b.order ? a.order > b.order : a.id < b.id;
    });
    std::vector<uint32_t> ids;
    for (const EarnedAchievement& e : earned) ids.push_back(e.id);
    return ids;
}

/// Whether the interface is told of an achievement at all (0x005b4561).
[[nodiscard]] inline bool achievementTold(uint32_t flags, bool earned) {
    return (flags & 2u) == 0 && ((flags & 0x800u) == 0 || earned);
}

class AchievementList {
public:
    /// `earned` the player's, newest first; `compared` the compared player's,
    /// the same, empty for none. `side` the player's, none without one.
    AchievementList(const std::vector<AchievementRow>& rows, const std::vector<uint32_t>& earned,
                    const std::vector<uint32_t>& compared, std::optional<int32_t> side)
        : rows_(&rows), earnedOrder_(earned), comparedOrder_(compared), side_(side),
          earned_(earned.begin(), earned.end()), compared_(compared.begin(), compared.end()) {
        for (const AchievementRow& r : *rows_) {
            if (r.previous != 0 && earned_.count(r.id)) followed_.insert(r.previous);
        }
    }

    /// Total and earned. A category of -1 is every achievement, -2 every
    /// statistic; `all` counts each step of a chain.
    [[nodiscard]] std::pair<uint32_t, uint32_t> count(int32_t category, bool all) const {
        uint32_t total = 0, done = 0, theirs = 0, chain = 0;
        for (const AchievementRow& r : *rows_) {
            const bool every = (category == -1 && !(r.flags & 1u)) || (category == -2 && (r.flags & 1u));
            const bool steps = every || all;
            if (!(every || static_cast<int32_t>(r.category) == category) || (r.flags & 2u)) continue;
            const bool mine = earned_.count(r.id) != 0, them = compared_.count(r.id) != 0;
            if (side_ && r.faction != -1 && r.faction != *side_ && !them) continue;
            if ((r.category == kFeatsOfStrength || (r.flags & 0x800u)) && !mine && !them) continue;
            if (chain != 0 && chain == r.previous) {
                // Follows one just counted that is still to be earned.
                chain = r.id;
                if (steps) ++total;
                else if (them) ++theirs;
            } else if (mine) {
                // One with a later step earned is shown by that.
                if (steps || !followed_.count(r.id)) { ++done; ++total; }
            } else if (them) {
                ++theirs;
            } else {
                chain = r.id;
                ++total;
            }
        }
        if (category == static_cast<int32_t>(kFeatsOfStrength)) total = done;
        return {total + theirs, done};
    }

    /// The achievement at a place of the category, from one; 0 for none:
    /// earned by both, by the player, by the compared, and what is left in the
    /// file's order.
    [[nodiscard]] uint32_t at(int32_t category, int index) const {
        int left = index;
        const auto earnedOnes = [&](const std::vector<uint32_t>& list, const std::unordered_set<uint32_t>& other,
                                    bool inOther) -> uint32_t {
            for (const uint32_t id : list) {
                if ((other.count(id) != 0) != inOther) continue;
                const AchievementRow* r = row(id);
                if (!r) continue;
                if ((category == -1 && !(r->flags & 1u)) || (category == -2 && (r->flags & 1u))) {
                    if (!(r->flags & 2u)) --left;
                } else if (static_cast<int32_t>(r->category) == category && !followed_.count(id)) {
                    --left;
                } else {
                    continue;
                }
                if (left == 0) return id;
            }
            return 0;
        };
        if (left < 1) return 0;
        if (const uint32_t id = earnedOnes(earnedOrder_, compared_, true)) return id;
        if (const uint32_t id = earnedOnes(earnedOrder_, compared_, false)) return id;
        if (const uint32_t id = earnedOnes(comparedOrder_, earned_, false)) return id;
        if (category == static_cast<int32_t>(kFeatsOfStrength)) return 0;
        uint32_t chain = 0;
        for (const AchievementRow& r : *rows_) {
            if (side_ && r.faction != -1 && r.faction != *side_) continue;
            const bool either = earned_.count(r.id) != 0 || compared_.count(r.id) != 0;
            const bool every = r.category != kFeatsOfStrength &&
                               ((category == -1 && !(r.flags & 1u)) || (category == -2 && (r.flags & 1u)));
            if (every) {
                // Of all the statistics those there is something of as well.
                if ((either && category == -1) || (r.flags & 0x800u)) continue;
                if (!(r.flags & 2u) && --left == 0) return r.id;
            } else if (static_cast<int32_t>(r.category) == category) {
                if (chain != 0 && chain == r.previous) {
                    chain = r.id;
                } else if (!either && !(r.flags & 0x800u)) {
                    chain = r.id;
                    if (--left == 0) return r.id;
                }
            }
        }
        return 0;
    }

    /// What the compared player has of a category, without the steps the player's own chain
    /// has gone past; `theirSide` none where it is not known.
    [[nodiscard]] uint32_t comparedCount(int32_t category, std::optional<int32_t> theirSide) const {
        uint32_t n = 0;
        for (const AchievementRow& r : *rows_) {
            const bool every = (category == -1 && !(r.flags & 1u)) || (category == -2 && (r.flags & 1u));
            if (!(every || static_cast<int32_t>(r.category) == category) || (r.flags & 2u)) continue;
            if (theirSide && r.faction != -1 && r.faction != *theirSide) continue;
            if (compared_.count(r.id) != 0 && (every || followed_.count(r.id) == 0)) ++n;
        }
        return n;
    }

private:
    [[nodiscard]] const AchievementRow* row(uint32_t id) const {
        for (const AchievementRow& r : *rows_)
            if (r.id == id) return &r;
        return nullptr;
    }
    const std::vector<AchievementRow>* rows_;
    std::vector<uint32_t> earnedOrder_, comparedOrder_;
    std::optional<int32_t> side_;
    std::unordered_set<uint32_t> earned_, compared_;
    std::unordered_set<uint32_t> followed_;  ///< those with a following one earned (0x005b37c0)
};

}  // namespace wowee::game

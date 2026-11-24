#pragma once

// What the achievements window lists of a category, and how many it says there
// are (0x005b38e0 GetCategoryNumAchievements, 0x005b3fc0 GetAchievementInfo of
// a category and an index).

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
};

constexpr uint32_t kFeatsOfStrength = 0x51;

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
                if (either || (r.flags & 0x800u)) continue;
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

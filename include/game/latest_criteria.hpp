#pragma once

// The criteria most recently updated, newest first, that
// GetLatestUpdatedStats and GetLatestUpdatedComparisonStats answer
// (0x005b1c90, 0x005b1d60).
//
// The client keeps a list for the player (0x00acfda8) and one for the unit it
// compares with (0x00acfdcc). Each statistic criteria a criteria record
// carries goes on the front (0x005b2520 / 0x005b2cb0); then the five records
// behind it are walked for one it replaces - for the player the same
// criteria or any criteria of the same statistic, for the comparison the
// same criteria only - and the first such is dropped. With none found the
// sixth is dropped instead, so the list holds six and the Lua calls read five.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace wowee::game {

class LatestCriteriaList {
public:
    static constexpr int kWalk = 5;  ///< records looked at behind the new one
    static constexpr int kRead = 5;  ///< ids the Lua call answers

    /// The achievement a criteria belongs to (Achievement_Criteria +0x04),
    /// 0 when it has no row - a record without one stalls the walk, as the
    /// client's does.
    using AchievementOf = std::function<uint32_t(uint32_t criteriaId)>;

    /// The player's list: replaces the same criteria or the same statistic.
    void notePlayer(uint32_t criteriaId, const AchievementOf& achievementOf) {
        const uint32_t achievement = achievementOf ? achievementOf(criteriaId) : 0;
        // The walk is over the records already there, from the front.
        size_t pos = 0;
        size_t drop = ids_.size();
        for (int i = 0; i < kWalk && pos < ids_.size(); ++i) {
            const uint32_t held = ids_[pos];
            const uint32_t heldAchievement = achievementOf ? achievementOf(held) : 0;
            if (heldAchievement == 0) continue;
            if (held == criteriaId || heldAchievement == achievement) {
                drop = pos;
                pos = ids_.size();
                break;
            }
            ++pos;
        }
        if (drop == ids_.size() && pos < ids_.size()) drop = pos;
        pushFront(criteriaId, drop);
    }

    /// The comparison unit's list: replaces the same criteria only.
    void noteComparison(uint32_t criteriaId) {
        size_t pos = 0;
        size_t drop = ids_.size();
        for (int i = 0; i < kWalk && pos < ids_.size(); ++i) {
            if (ids_[pos] == criteriaId) {
                drop = pos;
                pos = ids_.size();
                break;
            }
            ++pos;
        }
        if (drop == ids_.size() && pos < ids_.size()) drop = pos;
        pushFront(criteriaId, drop);
    }

    /// What the Lua call returns: the first five.
    [[nodiscard]] std::vector<uint32_t> latest() const {
        const size_t n = ids_.size() < static_cast<size_t>(kRead) ? ids_.size()
                                                                  : static_cast<size_t>(kRead);
        return {ids_.begin(), ids_.begin() + static_cast<std::ptrdiff_t>(n)};
    }

    [[nodiscard]] const std::vector<uint32_t>& all() const { return ids_; }
    void clear() { ids_.clear(); }

private:
    /// The new id in front of the old list, without the old record at `drop`
    /// (none when it is the list's size).
    void pushFront(uint32_t id, size_t drop) {
        std::vector<uint32_t> next;
        next.reserve(ids_.size() + 1);
        next.push_back(id);
        for (size_t i = 0; i < ids_.size(); ++i) {
            if (i != drop) next.push_back(ids_[i]);
        }
        ids_ = std::move(next);
    }

    std::vector<uint32_t> ids_;
};

}  // namespace wowee::game

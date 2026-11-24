#pragma once

// A criterion's or a statistic's number as text (0x005b0ea0).

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game {

/// One criterion as its text needs it.
struct StatisticCriterion {
    bool counted = false;    ///< the server sent a counter
    uint64_t value = 0;
    uint32_t quantity = 0;
    uint32_t flags = 0;      ///< Achievement_Criteria field 26: 1 a bar, 0x20 money
    std::string description;
};

constexpr uint32_t kCriterionBar = 0x1;
constexpr uint32_t kCriterionMoney = 0x20;

/// A number of a criterion: money as coins (0x007e7e10, or 0x007e7ee0 with
/// colorblindMode), else the number.
template <typename Money>
std::string criterionNumber(uint64_t value, uint32_t flags, Money&& money) {
    return (flags & kCriterionMoney) ? money(value) : std::to_string(value);
}

/// Its text, with " / " and what it is of on a bar.
template <typename Money>
std::string criterionText(std::string text, const StatisticCriterion& c, Money&& money) {
    if (c.flags & kCriterionBar) text += " / " + criterionNumber(c.quantity, c.flags, money);
    return text;
}

/// GetStatistic's text (0x005b5620 -> 0x005b0ea0): the achievement's flags pick
/// a sum (0x8), the largest counter (0x10, with the criterion's name when the
/// achievement has flag 0x1), how many criteria reached their quantity (0x20),
/// or the last counted criterion's counter. With nothing counted the answer is
/// "--" for a flag-0x1 achievement and "0" otherwise. Not carried: 0x40, a
/// per-day average from the criterion's own date. A date (criterion flag 0x10)
/// is in no row of 3.3.5's table.
template <typename Money>
std::string statisticText(uint32_t aflags, const std::vector<StatisticCriterion>& list, Money&& money) {
    uint64_t sum = 0, best = 0, reached = 0;
    const StatisticCriterion *bestCriterion = nullptr, *last = nullptr;
    bool tie = false;
    const std::string none = (aflags & 0x1) ? "--" : "0";
    for (const StatisticCriterion& c : list) {
        if (!c.counted) continue;
        sum += c.value;
        if (c.value > best || !bestCriterion) { best = c.value; bestCriterion = &c; tie = false; }
        else if (c.value == best) tie = true;
        if (c.value >= c.quantity) ++reached;
        last = &c;
    }
    if (aflags & 0x8) {
        const uint32_t flags = last ? last->flags : list.empty() ? 0u : list.back().flags;
        return criterionNumber(sum, flags, money);
    }
    if (aflags & 0x10) {
        if (!last) return none;
        // A tie for the largest names no criterion on a flag-0x400 row.
        if ((aflags & 0x1) && !(tie && (aflags & 0x400)))
            return std::to_string(best) + " (" + bestCriterion->description + ")";
        return std::to_string(best);
    }
    if (aflags & 0x20) return reached == 0 ? none : std::to_string(reached);
    if (!last || last->value == 0) {
        // A bar of money says 0 in coins.
        const StatisticCriterion* c = last ? last : list.empty() ? nullptr : &list.back();
        if (!c) return none;
        const bool coins = (c->flags & kCriterionMoney) && (c->flags & kCriterionBar);
        return criterionText(coins ? money(0) : none, *c, money);
    }
    // Capped at the criterion's quantity unless the achievement has flag 0x1.
    uint64_t value = last->value;
    if (!(aflags & 0x1) && last->quantity != 0 && last->quantity <= value) value = last->quantity;
    return criterionText(criterionNumber(value, last->flags, money), *last, money);
}

}  // namespace wowee::game

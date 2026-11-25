#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

/// What an achievement's criteria come to (Achievement_C.cpp: 0x005b58b0, 0x005b5620
/// -> 0x005b0ea0), for GetAchievementCriteriaInfo and GetStatistic.
namespace wowee::game::achievement_progress {

/// Achievement.dbc's flags (+0x24).
constexpr uint32_t kStatistic = 0x1, kHidden = 0x2, kSum = 0x8, kLargest = 0x10, kReached = 0x20, kPerDay = 0x40,
                   kBar = 0x80, kTieUnnamed = 0x400;
/// Any of them makes a list of criteria one line.
constexpr uint32_t kOneLine = 0xb0;
/// Achievement_Criteria.dbc's (+0x28).
constexpr uint32_t kCriterionBar = 0x1, kCriterionHidden = 0x2, kCriterionDate = 0x10, kCriterionMoney = 0x20;

struct Criterion {
    uint32_t id = 0, quantity = 0, flags = 0;
    std::string description;
};
struct Progress {
    uint64_t counter = 0;
    uint32_t since = 0;  ///< the time the server last moved it (+0x4c)
};
using Find = std::function<const Progress*(uint32_t criterionId)>;

/// The criteria the interface's index counts: those that are not hidden, all of
/// them where the achievement makes one line of them.
[[nodiscard]] inline std::vector<size_t> shownCriteria(uint32_t achievementFlags, const std::vector<Criterion>& criteria) {
    std::vector<size_t> shown;
    for (size_t i = 0; i < criteria.size(); ++i)
        if ((criteria[i].flags & kCriterionHidden) == 0 || (achievementFlags & kOneLine) != 0) shown.push_back(i);
    return shown;
}

struct Totals {
    uint64_t sum = 0, largest = 0, reached = 0;
    const Criterion* largestOf = nullptr;
};
/// Over all of them, as the flags ask; `tieFlags` the flags of the achievement the
/// tie is read by.
[[nodiscard]] inline Totals totals(uint32_t flags, uint32_t tieFlags, const std::vector<Criterion>& criteria,
                                   const Find& find) {
    Totals t;
    bool tie = false;
    for (const Criterion& c : criteria) {
        const Progress* p = find(c.id);
        if (!p) continue;
        if (flags & kSum) t.sum += p->counter;
        if (flags & kLargest) {
            if (p->counter > t.largest) {
                t.largest = p->counter;
                t.largestOf = &c;
                tie = false;
            } else if (p->counter == t.largest) {
                tie = true;
            }
        }
        if ((flags & kReached) && p->counter >= c.quantity) ++t.reached;
    }
    if (tie && (tieFlags & kTieUnnamed)) t.largestOf = nullptr;
    return t;
}

struct Shown {
    uint64_t quantity = 0;
    std::string text;
};
struct Words {
    std::function<std::string(uint64_t copper)> money;
    std::function<std::string(uint64_t time)> date;
    uint32_t now = 0;
};
/// 0x005b0ea0. `flags` the achievement's and `minimum` its least number of criteria,
/// `counter` and `since` the criterion's progress, `criterion` null for none,
/// `criterionFlags` its, `required` what it says it wants where that is not its own
/// quantity (0).
[[nodiscard]] inline Shown show(uint32_t flags, uint32_t minimum, const Totals& t, uint64_t counter, uint32_t since,
                                const Criterion* criterion, uint32_t criterionFlags, bool earned, uint32_t required,
                                const Words& words) {
    Shown s;
    char buf[64];
    const auto number = [&](uint64_t v) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
        return std::string(buf);
    };
    const std::string none = (flags & kStatistic) ? "--" : "0";
    const bool money = (criterionFlags & kCriterionMoney) != 0;
    float average = 0.0f;
    const auto perDay = [&](uint64_t v) {
        const uint32_t days = (flags & kPerDay) && since != 0 && words.now > since ? (words.now - since) / 86400 : 0;
        if (days == 0) return v;
        average = static_cast<float>(v) / static_cast<float>(days);
        return static_cast<uint64_t>(average);
    };
    const auto averageText = [&] {
        std::snprintf(buf, sizeof(buf), "%0.2f", static_cast<double>(average));
        return std::string(buf);
    };
    if (flags & kSum) {
        s.quantity = perDay(t.sum);
        s.text = money                ? words.money(s.quantity)
                 : average != 0.0f    ? averageText()
                 : s.quantity != 0    ? number(s.quantity)
                                      : none;
    } else if (flags & kLargest) {
        s.quantity = t.largest;
        if (t.largestOf) {
            s.text = number(t.largest);
            if (flags & kStatistic) s.text += " (" + t.largestOf->description + ")";
        } else {
            s.text = t.largest != 0 ? number(t.largest) : none;
        }
    } else if (flags & kReached) {
        s.quantity = t.reached;
        s.text = t.reached != 0 ? number(t.reached) : none;
    } else if (counter == 0) {
        if (!earned || minimum != 0 || !criterion) {
            s.text = money && (criterionFlags & kCriterionBar) ? words.money(0) : none;
        } else {
            s.quantity = criterion->quantity;
            s.text = s.quantity == 0 ? none : money ? words.money(s.quantity) : number(s.quantity);
        }
    } else {
        uint64_t v = counter;
        if (criterion && (flags & kStatistic) == 0 && criterion->quantity <= v) v = criterion->quantity;
        s.quantity = v = perDay(v);
        if (criterionFlags & kCriterionDate) s.text = words.date(v);
        else if (money) s.text = words.money(v);
        else if ((flags & kStatistic) == 0) s.text = number(v);
        else s.text = average != 0.0f ? averageText() : v != 0 ? number(v) : none;
    }
    if ((criterionFlags & kCriterionBar) && criterion) {
        const uint32_t wanted = required != 0 ? required : criterion->quantity;
        s.text += " / " + (money ? words.money(wanted) : number(wanted));
    }
    return s;
}

}  // namespace wowee::game::achievement_progress

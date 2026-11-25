// What an achievement's criteria come to (game/achievement_progress.hpp).
#include <catch_amalgamated.hpp>

#include "game/achievement_progress.hpp"

#include <map>

using namespace wowee::game::achievement_progress;

namespace {
const std::vector<Criterion> kThree{{.id = 1, .quantity = 10, .description = "Wolf"},
                                    {.id = 2, .quantity = 20, .flags = kCriterionHidden, .description = "Bear"},
                                    {.id = 3, .quantity = 5, .description = "Boar"}};
const Words kWords{.money = [](uint64_t v) { return std::to_string(v) + "c"; },
                   .date = [](uint64_t v) { return "day" + std::to_string(v); },
                   .now = 1000000};
Totals of(uint32_t flags, const std::map<uint32_t, Progress>& progress, uint32_t tieFlags = 0) {
    return totals(flags, tieFlags, kThree, [&](uint32_t id) {
        const auto it = progress.find(id);
        return it != progress.end() ? &it->second : nullptr;
    });
}
std::string text(uint32_t flags, const Totals& t, uint64_t counter, const Criterion* c, uint32_t cflags, bool earned = false,
                 uint32_t required = 0, uint32_t since = 0, uint32_t minimum = 0) {
    return show(flags, minimum, t, counter, since, c, cflags, earned, required, kWords).text;
}
}  // namespace

// 0x005b58b0
TEST_CASE("the criteria the interface counts", "[achievement_progress]") {
    CHECK(shownCriteria(0, kThree) == std::vector<size_t>{0, 2});
    CHECK(shownCriteria(kBar, kThree) == std::vector<size_t>{0, 1, 2});
    CHECK(shownCriteria(kReached, kThree) == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("all of an achievement's criteria together", "[achievement_progress]") {
    const std::map<uint32_t, Progress> p{{1, {12, 0}}, {2, {7, 0}}, {3, {5, 0}}};
    CHECK(of(kSum, p).sum == 24);
    CHECK(of(kReached, p).reached == 2);
    CHECK(of(kLargest, p).largest == 12);
    CHECK(of(kLargest, p).largestOf == &kThree[0]);
    CHECK(of(0, p).sum == 0);
    const std::map<uint32_t, Progress> tie{{1, {7, 0}}, {2, {7, 0}}};
    CHECK(of(kLargest, tie).largestOf == &kThree[0]);
    CHECK(of(kLargest, tie, kTieUnnamed).largestOf == nullptr);
}

// 0x005b0ea0
TEST_CASE("how a criterion's number is shown", "[achievement_progress]") {
    const Totals none;
    const Criterion& c = kThree[0];
    CHECK(text(0, none, 3, &c, 0) == "3");
    CHECK(text(0, none, 3, &c, kCriterionBar) == "3 / 10");
    CHECK(text(0, none, 99, &c, kCriterionBar) == "10 / 10");
    CHECK(text(kStatistic, none, 99, &c, 0) == "99");
    CHECK(text(0, none, 0, &c, kCriterionBar) == "0 / 10");
    CHECK(text(0, none, 0, &c, kCriterionBar, true) == "10 / 10");
    CHECK(text(0, none, 0, &c, kCriterionBar, true, 0, 0, 2) == "0 / 10");
    CHECK(text(kStatistic, none, 0, &c, 0) == "--");
    CHECK(text(0, none, 3, &c, kCriterionMoney) == "3c");
    CHECK(text(0, none, 3, &c, kCriterionMoney | kCriterionBar) == "3c / 10c");
    CHECK(text(0, none, 0, &c, kCriterionMoney | kCriterionBar) == "0c / 10c");
    CHECK(text(0, none, 3, &c, kCriterionDate) == "day3");
    // By the day.
    CHECK(text(kStatistic | kPerDay, none, 7, nullptr, 0, false, 0, 1000000 - 2 * 86400) == "3.50");
    CHECK(text(kStatistic | kPerDay, none, 7, nullptr, 0, false, 0, 1000000 - 100) == "7");
    // Of all of them.
    Totals t;
    t.sum = 24;
    CHECK(text(kSum | kBar, t, 0, &c, kCriterionBar) == "24 / 10");
    CHECK(text(kSum | kStatistic, none, 0, &c, 0) == "--");
    t.largest = 12;
    t.largestOf = &c;
    CHECK(text(kLargest | kStatistic, t, 0, &c, 0) == "12 (Wolf)");
    CHECK(text(kLargest, t, 0, &c, 0) == "12");
    t.largestOf = nullptr;
    CHECK(text(kLargest | kStatistic, t, 0, &c, 0) == "12");
    t.reached = 2;
    CHECK(text(kReached | kBar, t, 0, &c, kCriterionBar, false, 25) == "2 / 25");
    CHECK(show(kReached, 0, t, 0, 0, &c, 0, false, 0, kWords).quantity == 2);
}

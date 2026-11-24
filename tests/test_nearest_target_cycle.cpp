// The order the target keys go through their targets in (0x00524440,
// 0x005131d0, 0x00524fc0).
#include <catch_amalgamated.hpp>

#include "game/nearest_target_cycle.hpp"

#include <cmath>
#include <set>

using wowee::game::NearestMode;
using wowee::game::NearestTarget;
using wowee::game::NearestTargetCycle;
using wowee::game::nearestTargetAt;

namespace {

std::optional<NearestTarget> at(uint64_t guid, float yards, float degrees, float facing = 0.0f) {
    const float a = facing + degrees * 3.14159265f / 180.0f;
    return nearestTargetAt(guid, std::cos(a) * yards, std::sin(a) * yards, 0.0f, facing);
}

}  // namespace

TEST_CASE("ahead is thirty degrees to 41 yards, round is ten yards", "[targeting]") {
    for (float facing : {0.0f, 1.5f, 3.1f, 6.2f, -2.0f}) {
        CAPTURE(facing);
        REQUIRE(at(1, 40.5f, 29.0f, facing));
        CHECK(at(1, 40.5f, 29.0f, facing)->ahead);
        CHECK(at(1, 40.5f, -29.0f, facing)->ahead);
        CHECK_FALSE(at(1, 41.5f, 0.0f, facing));
        CHECK_FALSE(at(1, 21.1f, -146.0f, facing));
        CHECK_FALSE(at(1, 10.5f, 31.0f, facing));
        REQUIRE(at(1, 9.5f, 180.0f, facing));
        CHECK_FALSE(at(1, 9.5f, 180.0f, facing)->ahead);
    }
    // Height counts for the distance.
    CHECK_FALSE(nearestTargetAt(1, 5.0f, 0.0f, 41.0f, 0.0f));
}

TEST_CASE("those ahead come first, the nearer first of each", "[targeting]") {
    NearestTargetCycle cycle;
    const auto collect = [] {
        return std::vector<NearestTarget>{{1, 25.0f, false}, {2, 900.0f, true}, {3, 400.0f, true}, {4, 16.0f, false}};
    };
    const auto always = [](uint64_t) { return true; };
    uint64_t target = 0;
    std::vector<uint64_t> order;
    for (int i = 0; i < 5; ++i) {
        target = cycle.next(NearestMode::Enemy, false, target, 1000 + i * 100, collect, always);
        order.push_back(target);
    }
    CHECK(order == std::vector<uint64_t>{3, 2, 4, 1, 3});
    // And backwards.
    target = cycle.next(NearestMode::Enemy, true, target, 1600, collect, always);
    CHECK(target == 1);
    // Another target in between: where one was, not on.
    CHECK(cycle.next(NearestMode::Enemy, false, 77, 1700, collect, always) == 1);
}

TEST_CASE("the list is kept for three seconds, and to its end for one", "[targeting]") {
    NearestTargetCycle cycle;
    int made = 0;
    const auto collect = [&] {
        ++made;
        return std::vector<NearestTarget>{{1, 1.0f, true}, {2, 2.0f, true}};
    };
    const auto always = [](uint64_t) { return true; };
    CHECK(cycle.next(NearestMode::Enemy, false, 0, 1000, collect, always) == 1);
    CHECK(cycle.next(NearestMode::Enemy, false, 1, 1200, collect, always) == 2);
    CHECK(cycle.next(NearestMode::Enemy, false, 2, 1400, collect, always) == 1);   // its end, not a second old
    CHECK(made == 1);
    CHECK(cycle.next(NearestMode::Enemy, false, 1, 2100, collect, always) == 2);
    CHECK(cycle.next(NearestMode::Enemy, false, 2, 2200, collect, always) == 1);   // and now it is
    CHECK(made == 2);
    CHECK(cycle.next(NearestMode::Enemy, false, 1, 5200, collect, always) == 1);   // three seconds on
    CHECK(made == 3);
    CHECK(cycle.next(NearestMode::Friend, false, 1, 5300, collect, always) == 1);  // another key
    CHECK(made == 4);
    cycle.targetCleared();
    CHECK(cycle.next(NearestMode::Friend, false, 0, 5400, collect, always) == 1);
    CHECK(made == 5);
}

TEST_CASE("one that is no candidate any more is passed over", "[targeting]") {
    NearestTargetCycle cycle;
    std::set<uint64_t> dead;
    const auto collect = [] {
        return std::vector<NearestTarget>{{1, 1.0f, true}, {2, 2.0f, true}, {3, 3.0f, true}};
    };
    const auto still = [&](uint64_t g) { return !dead.count(g); };
    CHECK(cycle.next(NearestMode::Enemy, false, 0, 1000, collect, still) == 1);
    dead = {2};
    CHECK(cycle.next(NearestMode::Enemy, false, 1, 1100, collect, still) == 3);
    dead = {1, 2, 3};
    CHECK(cycle.next(NearestMode::Enemy, false, 3, 1200, collect, still) == 0);
    CHECK(cycle.next(NearestMode::Enemy, false, 0, 1300, [] { return std::vector<NearestTarget>{}; }, still) == 0);
    // All of an old list gone: one fresh look, and an end of it.
    dead.clear();
    CHECK(cycle.next(NearestMode::Enemy, false, 0, 1400, collect, still) == 1);
    dead = {1, 2, 3};
    CHECK(cycle.next(NearestMode::Enemy, false, 1, 3000, collect, still) == 0);
}

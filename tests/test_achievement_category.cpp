// What the achievements window has in a category (game/achievement_category.hpp).
#include <catch_amalgamated.hpp>

#include "game/achievement_category.hpp"

using namespace wowee::game::achievement_category;

namespace {
// Level 10 to 40, one of each side, one hidden, one only when it is earned, a feat
// and a statistic.
const std::vector<Row> kRows{{.id = 10, .category = 92, .order = 1},
                             {.id = 20, .supercedes = 10, .category = 92, .order = 2},
                             {.id = 30, .supercedes = 20, .category = 92, .order = 3},
                             {.id = 40, .supercedes = 30, .category = 92, .order = 4},
                             {.id = 50, .faction = 1, .category = 92, .order = 5},
                             {.id = 51, .faction = 0, .category = 92, .order = 6},
                             {.id = 60, .category = 92, .flags = kHidden},
                             {.id = 70, .category = 92, .flags = kOnlyEarned, .order = 7},
                             {.id = 80, .category = kFeats},
                             {.id = 81, .category = kFeats},
                             {.id = 90, .category = 130, .flags = kStatistic}};
constexpr Side kHorde = 0;
std::vector<uint32_t> ids(const Earned& earned, int32_t category, Side side = kHorde) {
    std::vector<uint32_t> out;
    for (int i = 1; const uint32_t id = at(kRows, earned, category, i, side); ++i) out.push_back(id);
    return out;
}
using Ids = std::vector<uint32_t>;
}  // namespace

// 0x005b38e0, 0x005b3fc0
TEST_CASE("a category's achievements: the open steps, and those earned", "[achievement_category]") {
    Earned none;
    auto c = count(kRows, none, 92, false, kHorde);
    CHECK(c.total == 2);  // level 10, the Horde's
    CHECK(c.earned == 0);
    CHECK(ids(none, 92) == Ids{10, 51});
    CHECK(ids(none, 92, 1) == Ids{10, 50});
    CHECK(ids(none, 92, -1) == Ids{10, 50, 51});

    const Earned two{{10, 100}, {20, 200}};
    c = count(kRows, two, 92, false, kHorde);
    CHECK(c.total == 3);  // level 20 earned, 30 next, the Horde's
    CHECK(c.earned == 1);
    CHECK(ids(two, 92) == Ids{20, 30, 51});
    c = count(kRows, two, 92, true, kHorde);
    CHECK(c.total == 5);  // 10, 20, 30, 40, the Horde's
    CHECK(c.earned == 2);

    // The latest first, of one time the one with the larger order.
    const Earned some{{10, 300}, {51, 100}, {70, 300}};
    CHECK(ids(some, 92) == Ids{70, 10, 51, 20});
    CHECK(count(kRows, some, 92, false, kHorde).total == 4);
    CHECK(count(kRows, some, 92, false, kHorde).earned == 3);
}

TEST_CASE("feats of strength, and everything there is", "[achievement_category]") {
    const Earned one{{81, 5}};
    CHECK(count(kRows, one, kFeats, false, kHorde).total == 1);
    CHECK(count(kRows, one, kFeats, false, kHorde).earned == 1);
    CHECK(count(kRows, {}, kFeats, false, kHorde).total == 0);
    CHECK(ids(one, kFeats) == Ids{81});
    CHECK(count(kRows, {}, kAllStatistics, false, kHorde).total == 1);
    CHECK(ids({}, kAllStatistics) == Ids{90});
    CHECK(ids({}, kAll) == Ids{10, 20, 30, 40, 51});
    CHECK(count(kRows, {}, kAll, false, kHorde).total == 5);
    CHECK(at(kRows, {}, 92, 0, kHorde) == 0);
    CHECK(at(kRows, {}, 92, 99, kHorde) == 0);
    CHECK(at(kRows, {}, 92, 3, kHorde) == 0);  // the hidden one
    CHECK(shown(0, false));
    CHECK_FALSE(shown(kHidden, true));
    CHECK_FALSE(shown(kOnlyEarned, false));
    CHECK(shown(kOnlyEarned, true));
}

// 0x005b3b30
TEST_CASE("the compared player's of a category", "[achievement_category]") {
    const Earned own{{20, 1}}, theirs{{10, 1}, {20, 2}, {50, 3}, {60, 4}, {90, 5}};
    CHECK(comparedCount(kRows, own, theirs, 92, 1) == 2);       // 20 and the Alliance's; 10 is passed
    CHECK(comparedCount(kRows, {}, theirs, 92, 1) == 3);
    CHECK(comparedCount(kRows, {}, theirs, 92, kHorde) == 2);
    CHECK(comparedCount(kRows, {}, theirs, 92, -1) == 3);
    CHECK(comparedCount(kRows, {}, theirs, 92, -2) == 2);
    CHECK(comparedCount(kRows, own, theirs, kAll, 1) == 3);
    CHECK(comparedCount(kRows, own, theirs, kAllStatistics, 1) == 1);
}

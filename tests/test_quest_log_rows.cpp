// The quest log's rows (0x005e6940, 0x005e08f0): a header per zone, its quests
// by level and title, and those under a folded header behind all of it.
#include <catch_amalgamated.hpp>

#include "game/quest_log_rows.hpp"

#include <string>
#include <vector>

namespace {

struct Quest {
    uint32_t questId;
    std::string title;
    int32_t level;
    int32_t zoneOrSort;
};

std::string titleOf(int32_t group) {
    if (group == 10) return "Nagrand";
    if (group == 20) return "Hellfire Peninsula";
    return group < 0 ? "Epic" : "Miscellaneous";
}

const std::vector<Quest> kLog = {
    {1, "Zeta", 62, 10}, {2, "beta", 61, 20}, {3, "Alpha", 61, 20}, {4, "Gamma", -1, 20},
    {5, "Epic one", 60, -1}, {6, "Not known yet", 0, 0}, {0, "empty", 1, 10},
};

std::vector<int> ids(const wowee::game::QuestLogRows<Quest>& rows) {
    std::vector<int> out;
    for (const auto& r : rows.rows) out.push_back(r.isHeader ? -1 : static_cast<int>(r.quest->questId));
    return out;
}

}  // namespace

TEST_CASE("quests stand under their zone's header, by level and title", "[quest][log]") {
    const auto rows = wowee::game::questLogRows(kLog, {}, titleOf, 60);
    // The headers by name, zones and sorts alike (0x005dfdc0).
    CHECK(ids(rows) == std::vector<int>{-1, 5, -1, 4, 3, 2, -1, 1, -1, 6});
    CHECK(rows.visible == 10);
    CHECK(rows.rows[0].headerTitle == "Epic");
    CHECK(rows.rows[2].headerTitle == "Hellfire Peninsula");
    CHECK(rows.indexOf(1) == 8);
    CHECK(rows.indexOf(3) == 5);
    CHECK(rows.indexOf(0) == 0);
    CHECK(rows.indexOf(99) == 0);
}

TEST_CASE("the quests of a folded header keep a place behind the rest", "[quest][log]") {
    const auto rows = wowee::game::questLogRows(kLog, {20}, titleOf, 60);
    CHECK(ids(rows) == std::vector<int>{-1, 5, -1, -1, 1, -1, 6, 4, 3, 2});
    CHECK(rows.visible == 7);
    CHECK(rows.indexOf(2) == 10);
}

// 0x005e0da0
TEST_CASE("the items a quest's button may be of", "[quest][log]") {
    using Items = std::vector<uint32_t>;
    using wowee::game::questButtonItems;
    CHECK(questButtonItems(27479, 0, {1, 2}) == Items{27479});
    CHECK(questButtonItems(0, 0, {1, 2}).empty());
    CHECK(questButtonItems(27479, 0x20000, {1, 0, 3, 4, 5}) == Items{27479, 1, 3, 4});
    CHECK(questButtonItems(0, 0x20000, {7}) == Items{7});
}

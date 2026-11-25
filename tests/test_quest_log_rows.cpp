// The quest log's rows (game/quest_log_rows.hpp).
#include <catch_amalgamated.hpp>

#include "game/quest_log_rows.hpp"

using namespace wowee::game;

// 0x005e08f0
TEST_CASE("a heading's quests by level, then by title", "[quest_log_rows]") {
    CHECK(QuestRowOrder{10, "B"}.before({11, "A"}));
    CHECK_FALSE(QuestRowOrder{11, "A"}.before({10, "B"}));
    CHECK(QuestRowOrder{10, "apfel"}.before({10, "Birne"}));
    CHECK_FALSE(QuestRowOrder{10, "Birne"}.before({10, "apfel"}));
    CHECK_FALSE(QuestRowOrder{10, "Birne"}.before({10, "birne"}));
}

// 0x005e0da0
TEST_CASE("the items a quest's button may be of", "[quest_log_rows]") {
    using Items = std::vector<uint32_t>;
    CHECK(questButtonItems(27479, 0, {1, 2}) == Items{27479});
    CHECK(questButtonItems(0, 0, {1, 2}).empty());
    CHECK(questButtonItems(27479, 0x20000, {1, 0, 3, 4, 5}) == Items{27479, 1, 3, 4});
    CHECK(questButtonItems(0, 0x20000, {7}) == Items{7});
}

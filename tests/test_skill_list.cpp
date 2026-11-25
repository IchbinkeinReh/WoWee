// Which skills the skills list has (game/skill_ups.hpp).
#include <catch_amalgamated.hpp>

#include "game/skill_ups.hpp"

using Info = wowee::game::SkillUps::RaceClassInfo;

// 0x005cd9b0
TEST_CASE("a skill is listed by its row for the race and class", "[skill_list]") {
    // Mounts, companions (0x2), a race's own (0x92).
    CHECK_FALSE(Info{.flags = 0x2}.listed(400, 80));
    CHECK_FALSE(Info{.flags = 0x92}.listed(400, 80));
    CHECK_FALSE(Info{.flags = 0x3}.listed(400, 80));
    CHECK(Info{.flags = 0x0}.listed(400, 80));
    // Without a value: with flag 1, or with flag 4 from its level on.
    CHECK_FALSE(Info{.flags = 0x0}.listed(0, 80));
    CHECK(Info{.flags = 0x1}.listed(0, 80));
    CHECK(Info{.flags = 0x4, .minLevel = 20}.listed(0, 20));
    CHECK_FALSE(Info{.flags = 0x4, .minLevel = 20}.listed(0, 19));
    CHECK_FALSE(Info{.flags = 0x0, .minLevel = 20}.listed(0, 80));
}

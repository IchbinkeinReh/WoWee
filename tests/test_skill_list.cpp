// Which skills the skills list has (game/skill_ups.hpp).
#include <catch_amalgamated.hpp>

#include "game/skill_ups.hpp"

using wowee::game::SkillUps;
using Info = SkillUps::RaceClassInfo;

namespace {
bool listed(const Info& info, uint32_t rank, uint32_t level) { return SkillUps::listed(&info, rank, level); }
}  // namespace

// 0x005cd9b0
TEST_CASE("a skill is listed by its row for the race and class", "[skill_list]") {
    // Mounts, companions (0x2), a race's own (0x92).
    CHECK_FALSE(listed(Info{.flags = 0x2}, 400, 80));
    CHECK_FALSE(listed(Info{.flags = 0x92}, 400, 80));
    CHECK_FALSE(listed(Info{.flags = 0x3}, 400, 80));
    CHECK(listed(Info{.flags = 0x0}, 400, 80));
    // Without a value: with flag 1, or with flag 4 from its level on.
    CHECK_FALSE(listed(Info{.flags = 0x0}, 0, 80));
    CHECK(listed(Info{.flags = 0x1}, 0, 80));
    CHECK(listed(Info{.flags = 0x4, .minLevel = 20}, 0, 20));
    CHECK_FALSE(listed(Info{.flags = 0x4, .minLevel = 20}, 0, 19));
    CHECK_FALSE(listed(Info{.flags = 0x0, .minLevel = 20}, 0, 80));
    CHECK_FALSE(SkillUps::listed(nullptr, 400, 80));
}

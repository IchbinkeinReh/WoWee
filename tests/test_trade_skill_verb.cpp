// A recipe's verb (game/trade_skill_verb.hpp).
#include <catch_amalgamated.hpp>

#include "game/trade_skill_verb.hpp"

// 0x005db2a0
TEST_CASE("a recipe that makes no item goes by its skill's verb", "[trade_skill]") {
    using wowee::game::recipeHasSkillVerb;
    CHECK(recipeHasSkillVerb(53));   // ENCHANT_ITEM
    CHECK(recipeHasSkillVerb(0));
    CHECK_FALSE(recipeHasSkillVerb(24));
    CHECK_FALSE(recipeHasSkillVerb(59));
    CHECK_FALSE(recipeHasSkillVerb(157));
}

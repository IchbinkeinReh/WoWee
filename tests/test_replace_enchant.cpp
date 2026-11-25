// The enchant another takes the place of (game/replace_enchant.hpp).
#include <catch_amalgamated.hpp>

#include "game/replace_enchant.hpp"

using wowee::game::replacedEnchant;

TEST_CASE("an enchant takes the place of its own kind", "[replace_enchant]") {
    CHECK(replacedEnchant(53, 803, 803, 0).asks());
    CHECK(replacedEnchant(53, 803, 1900, 0).existing == 1900);
    CHECK(replacedEnchant(53, 803, 1900, 0).incoming == 803);
    CHECK_FALSE(replacedEnchant(53, 803, 0, 40).asks());
    CHECK(replacedEnchant(54, 13, 0, 40).existing == 40);
    CHECK_FALSE(replacedEnchant(54, 13, 803, 0).asks());
    CHECK_FALSE(replacedEnchant(53, 0, 803, 0).asks());
    CHECK_FALSE(replacedEnchant(6, 13, 803, 40).asks());
}

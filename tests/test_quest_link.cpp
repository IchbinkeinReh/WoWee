// A quest's chat link (game/quest_link.hpp).
#include <catch_amalgamated.hpp>

#include "game/quest_link.hpp"

using namespace wowee::game;

TEST_CASE("a quest's colour", "[quest_link]") {
    const std::string red = "|cffff2020", orange = "|cffff8040", yellow = "|cffffff00", green = "|cff40c040",
                      grey = "|cff808080";
    CHECK(questColour(85, 80) == red);
    CHECK(questColour(84, 80) == orange);
    CHECK(questColour(83, 80) == orange);
    CHECK(questColour(82, 80) == yellow);
    CHECK(questColour(78, 80) == yellow);
    CHECK(questColour(77, 80) == green);
    CHECK(questColour(72, 80) == green);
    CHECK(questColour(71, 80) == grey);
    CHECK(questColour(6, 10) == green);
    CHECK(questColour(5, 10) == green);
    CHECK(questColour(4, 10) == grey);
}

TEST_CASE("a quest's link", "[quest_link]") {
    CHECK(questChatLink(743, 8, "Windfuriengefahr", 80) == "|cff808080|Hquest:743:8|h[Windfuriengefahr]|h|r");
    CHECK(questChatLink(743, -1, "A", 80) == "|cffffff00|Hquest:743:-1|h[A]|h|r");
}

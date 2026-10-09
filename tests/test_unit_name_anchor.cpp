// Where the client puts a unit's name and plate, and which plates it shows
// and dims (0x0071fef0, 0x00715720, 0x0072b060, 0x0098e9f0).
#include <catch_amalgamated.hpp>

#include "rendering/unit_name_anchor.hpp"

namespace una = wowee::rendering::unit_name_anchor;

TEST_CASE("the name's attachments are PlayerName and PlayerNameMounted", "[unit_name_anchor]") {
    CHECK(una::kAttachmentPlayerName == 18);
    CHECK(una::kAttachmentPlayerNameMounted == 29);
}

TEST_CASE("without an attachment the name is 1.25 heights up", "[unit_name_anchor]") {
    const glm::vec3 p = una::fallbackNamePosition({1.0f, 2.0f, 3.0f}, 2.0f, 1.5f);
    CHECK(p.x == 1.0f);
    CHECK(p.y == 2.0f);
    CHECK(p.z == Catch::Approx(3.0f + 2.0f * 1.5f * 1.25f));
}

TEST_CASE("a plate stands two thirds of a yard over the name", "[unit_name_anchor]") {
    CHECK(una::kPlateLift == Catch::Approx(2.0f / 3.0f));
}

TEST_CASE("plates show within 41 yards of the player", "[unit_name_anchor]") {
    const glm::vec3 player(10.0f, 10.0f, 0.0f);
    CHECK(una::plateInRange(player, player + glm::vec3(41.0f, 0.0f, 0.0f)));
    CHECK_FALSE(una::plateInRange(player, player + glm::vec3(41.01f, 0.0f, 0.0f)));
    CHECK_FALSE(una::plateInRange(player, player + glm::vec3(30.0f, 30.0f, 0.0f)));
    CHECK(una::plateInRange(player, player + glm::vec3(0.0f, 20.0f, 30.0f)));
}

TEST_CASE("with a target the other plates are at half alpha", "[unit_name_anchor]") {
    CHECK(una::plateAlpha(false, false) == 1.0f);
    CHECK(una::plateAlpha(true, true) == 1.0f);
    CHECK(una::plateAlpha(true, false) == Catch::Approx(127.0f / 255.0f));
}

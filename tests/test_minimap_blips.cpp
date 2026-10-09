// The minimap's blips as the client draws them (0x005832f0, 0x0057dca0,
// 0x0057f7f0, 0x0057ff70, 0x00580ae0, 0x007f3f40).
#include <catch_amalgamated.hpp>

#include "rendering/minimap_blips.hpp"

#include <cmath>

namespace mb = wowee::rendering::minimap_blips;

TEST_CASE("sizes in Minimap.xml units", "[minimap_blips]") {
    CHECK(mb::kBlipSize == Catch::Approx(16.0f));
    CHECK(mb::kRimArrowSize == Catch::Approx(57.6f));
    CHECK(mb::kRimArrowDistance == Catch::Approx(56.32f));
    CHECK(mb::kFrameSize == 140.0f);
}

TEST_CASE("ObjectIcons is eight by two", "[minimap_blips]") {
    const auto first = mb::objectIconCell(0);
    CHECK(first.u0 == 0.0f);
    CHECK(first.v0 == 0.0f);
    CHECK(first.u1 == 0.125f);
    CHECK(first.v1 == 0.5f);
    // The ? is the third cell of the second row.
    const auto reward = mb::objectIconCell(mb::kQuestReward);
    CHECK(reward.u0 == 0.25f);
    CHECK(reward.u1 == 0.375f);
    CHECK(reward.v0 == 0.5f);
    CHECK(reward.v1 == 1.0f);
}

TEST_CASE("PartyRaidBlips is eight by four", "[minimap_blips]") {
    const auto c = mb::partyRaidCell(17);
    CHECK(c.u0 == 0.125f);
    CHECK(c.u1 == 0.25f);
    CHECK(c.v0 == 0.5f);
    CHECK(c.v1 == 0.75f);
}

TEST_CASE("quest givers: !, ? and blue !, and nothing else", "[minimap_blips]") {
    CHECK(mb::questGiverIcon(8, false) == 9);
    CHECK(mb::questGiverIcon(10, false) == 10);
    CHECK(mb::questGiverIcon(7, false) == 11);
    // Low level only with that tracking chosen.
    CHECK(mb::questGiverIcon(2, false) == -1);
    CHECK(mb::questGiverIcon(2, true) == 9);
    CHECK(mb::questGiverIcon(4, false) == -1);
    CHECK(mb::questGiverIcon(4, true) == 11);
    for (uint8_t s : {0, 1, 3, 5, 6, 9, 11}) {
        CHECK(mb::questGiverIcon(s, true) == -1);
    }
}

TEST_CASE("party by class in the top rows, the raid below", "[minimap_blips]") {
    CHECK(mb::partyRaidIcon(1, true) == 0);    // warrior
    CHECK(mb::partyRaidIcon(11, true) == 10);  // druid
    CHECK(mb::partyRaidIcon(1, false) == 16);
    CHECK(mb::partyRaidIcon(11, false) == 26);
    CHECK(mb::partyRaidIcon(0, true) == -1);
}

TEST_CASE("blips stop at 0.8 of the reach", "[minimap_blips]") {
    CHECK(mb::withinBlipReach(80.0f, 100.0f));
    CHECK_FALSE(mb::withinBlipReach(80.5f, 100.0f));
    CHECK_FALSE(mb::withinBlipReach(1.0f, 0.0f));
}

TEST_CASE("rim arrows sit on the bearing", "[minimap_blips]") {
    // North is up, west is left.
    const auto north = mb::rimArrowOffset(0.0f);
    CHECK(north[0] == Catch::Approx(0.0f).margin(1e-4));
    CHECK(north[1] == Catch::Approx(mb::kRimArrowDistance));
    const auto west = mb::rimArrowOffset(1.5707964f);
    CHECK(west[0] == Catch::Approx(-mb::kRimArrowDistance));
    CHECK(west[1] == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("rim arrow art turns about its middle", "[minimap_blips]") {
    for (float bearing : {0.0f, 0.7f, 2.0f, -1.3f}) {
        const auto uv = mb::rimArrowUVs(bearing);
        // Opposite corners mirror about the middle.
        CHECK(uv[0][0] + uv[3][0] == Catch::Approx(1.0f));
        CHECK(uv[0][1] + uv[3][1] == Catch::Approx(1.0f));
        CHECK(uv[1][0] + uv[2][0] == Catch::Approx(1.0f));
        CHECK(uv[1][1] + uv[2][1] == Catch::Approx(1.0f));
        // Every corner is the same distance out: a turned square.
        for (const auto& c : uv) {
            const float dx = c[0] - 0.5f, dy = c[1] - 0.5f;
            CHECK(std::sqrt(dx * dx + dy * dy) == Catch::Approx(1.0f));
        }
    }
}

TEST_CASE("rim arrow art lands where the corners' coordinates put it", "[minimap_blips]") {
    // The quad's corners, over the half size, y up, in rimArrowUVs's order.
    const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    const float h = mb::kRimArrowSize * 0.5f;
    for (float bearing : {0.0f, 0.4f, 1.9f, 3.0f, -2.2f}) {
        const auto uv = mb::rimArrowUVs(bearing);
        for (int i = 0; i < 4; ++i) {
            const auto p = mb::rimArrowArtPoint(bearing, uv[i][0], uv[i][1]);
            CHECK(p[0] == Catch::Approx(corners[i][0] * h).margin(1e-3));
            CHECK(p[1] == Catch::Approx(corners[i][1] * h).margin(1e-3));
        }
    }
}

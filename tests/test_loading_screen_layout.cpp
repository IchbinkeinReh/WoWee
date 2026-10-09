// The world loading screen as LoadingScreen.cpp lays it out: the picture in
// a viewport of its own shape (0x0040a270), its "Wide" version (0x00409ed0)
// and the bar's two pieces (0x004090c0).
#include <catch_amalgamated.hpp>

#include "rendering/loading_screen_layout.hpp"

namespace ls = wowee::rendering::loading_screen;

TEST_CASE("a screen wider than 4:3 takes a picture's Wide version (0x00409ed0)", "[loading_screen]") {
    CHECK(ls::wantsWidePicture(16.0f / 9.0f, true));
    CHECK_FALSE(ls::wantsWidePicture(16.0f / 9.0f, false));
    CHECK_FALSE(ls::wantsWidePicture(4.0f / 3.0f, true));
    CHECK(ls::widePicturePath("Interface\\Glues\\LoadingScreens\\LoadScreenNorthrend.blp") ==
          "Interface\\Glues\\LoadingScreens\\LoadScreenNorthrendWide.blp");
    CHECK(ls::widePicturePath("Interface\\Glues\\loading") == "Interface\\Glues\\loadingWide");
}

TEST_CASE("the picture keeps its shape in the middle of the screen (0x0040a270)", "[loading_screen]") {
    // 16:9 against a 4:3 picture: bars at the sides.
    const ls::Rect pillar = ls::pictureViewport(16.0f / 9.0f, false);
    CHECK(pillar.x1 - pillar.x0 == Catch::Approx(0.75f));
    CHECK(pillar.x0 == Catch::Approx(0.125f));
    CHECK(pillar.y0 == 0.0f);
    CHECK(pillar.y1 == 1.0f);
    // 16:10 against a Wide picture: all of it.
    const ls::Rect exact = ls::pictureViewport(1.6f, true);
    CHECK(exact.x0 == Catch::Approx(0.0f));
    CHECK(exact.x1 == Catch::Approx(1.0f));
    // 5:4 against a 4:3 picture: bars above and below.
    const ls::Rect letter = ls::pictureViewport(1.25f, false);
    CHECK(letter.y1 - letter.y0 == Catch::Approx(0.9375f));
    CHECK(letter.y0 == Catch::Approx(0.03125f));
    CHECK(letter.x0 == 0.0f);
}

TEST_CASE("the bar: its fill up to the progress, under its border (0x004090c0)", "[loading_screen]") {
    REQUIRE(ls::kBar[0].fill);
    REQUIRE_FALSE(ls::kBar[1].fill);
    const ls::Rect border = ls::barPieceRect(ls::kBar[1], 0.3f);
    CHECK(border.x0 == Catch::Approx(0.2f));
    CHECK(border.x1 == Catch::Approx(0.8f));
    CHECK(border.y0 == Catch::Approx(0.05f));
    CHECK(border.y1 == Catch::Approx(0.1f));
    const ls::Rect fill = ls::barPieceRect(ls::kBar[0], 0.5f);
    CHECK(fill.x0 == Catch::Approx(0.2375f));
    CHECK(fill.x1 == Catch::Approx(0.2375f + 0.2625f));
    CHECK(fill.y0 == Catch::Approx(0.0625f));
    CHECK(fill.y1 == Catch::Approx(0.0875f));
}

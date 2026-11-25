// The loading screen's tip (rendering/loading_tip.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/loading_tip.hpp"

namespace lt = wowee::rendering::loading_tip;

// 0x0040ab70
TEST_CASE("where the tip stands", "[loading_tip]") {
    const lt::Place four = lt::place(1024.0f, 768.0f);
    CHECK(four.width == Catch::Approx(515.0f));
    CHECK(four.left == Catch::Approx(254.5f));
    CHECK(four.foot == Catch::Approx(768.0f * 0.9f));
    CHECK(four.size == Catch::Approx(13.824f));
    CHECK(four.spacing == Catch::Approx(3.84f));
    // A wider screen: the column of the 4:3 in its middle.
    const lt::Place wide = lt::place(1365.3333f, 768.0f);
    CHECK(wide.width == Catch::Approx(515.0f));
    CHECK(wide.left == Catch::Approx((1365.3333f - 515.0f) / 2.0f));
    CHECK(wide.foot == Catch::Approx(768.0f * 0.9f));
    // A narrower one: the picture has black above and below.
    const lt::Place tall = lt::place(1024.0f, 1024.0f);
    CHECK(tall.foot == Catch::Approx(1024.0f * (0.9f - 0.125f)));
    CHECK(lt::place(0.0f, 0.0f).width == 0.0f);
}

// 0x004d9c00
TEST_CASE("which tip is the next", "[loading_tip]") {
    CHECK(lt::row(0, 142) == 0);
    CHECK(lt::row(141, 142) == 141);
    CHECK(lt::row(142, 142) == 0);
    CHECK(lt::row(-1, 142) == 0);
    CHECK(lt::row(0, 0) == -1);
    CHECK(lt::trimmed("Tipp\r\n") == "Tipp");
    CHECK(lt::trimmed("a\nb") == "a\nb");
}

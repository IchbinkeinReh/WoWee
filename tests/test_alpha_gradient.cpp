// SetAlphaGradient (ui/alpha_gradient.hpp).
#include <catch_amalgamated.hpp>

#include "ui/alpha_gradient.hpp"

using namespace wowee::ui;

// 0x00482230, 0x006c78f0
TEST_CASE("a text without a gradient's length is whole", "[alpha_gradient]") {
    CHECK_FALSE(alphaGradientOn(0, 0));
    CHECK_FALSE(alphaGradientOn(-1, 30));
    CHECK(alphaGradientOn(0, 30));
    CHECK(gradientAlpha(0, 0, 7) == 1.0f);
    CHECK(gradientAlpha(-1, 30, 7) == 1.0f);
}

TEST_CASE("the glyphs of a gradient", "[alpha_gradient]") {
    CHECK(gradientAlpha(5, 10, 4) == 1.0f);
    CHECK(gradientAlpha(5, 10, 5) == Catch::Approx(0.9f));
    CHECK(gradientAlpha(5, 10, 7) == Catch::Approx(0.5f));
    CHECK(gradientAlpha(5, 10, 12) == 0.0f);
    CHECK(gradientAlpha(5, 10, 15) == 0.0f);
}

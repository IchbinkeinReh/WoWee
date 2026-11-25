// Whether the mouse is in a region (ui/mouse_over.hpp).
#include <catch_amalgamated.hpp>

#include "ui/mouse_over.hpp"

using namespace wowee::ui;

// 0x00488570
TEST_CASE("the mouse in a region, and beside it", "[mouse_over]") {
    const auto over = [](float x, float y, MouseOverSides by = {}, float scale = 1.0f) {
        return mouseOver(100.0f, 50.0f, 40.0f, 20.0f, scale, x, y, by);
    };
    CHECK(over(120.0f, 60.0f));
    CHECK_FALSE(over(100.0f, 60.0f));
    CHECK_FALSE(over(120.0f, 70.0f));
    CHECK_FALSE(over(120.0f, 80.0f));
    CHECK(over(120.0f, 80.0f, {.top = 28.0f}));
    CHECK_FALSE(over(120.0f, 51.0f, {.bottom = 2.0f}));
    CHECK(over(120.0f, 49.0f, {.bottom = -2.0f}));
    CHECK(over(99.0f, 60.0f, {.left = -2.0f}));
    CHECK(over(141.0f, 60.0f, {.right = 2.0f}));
    // In its own units.
    CHECK(over(120.0f, 80.0f, {.top = 6.0f}, 2.0f));
    CHECK_FALSE(over(120.0f, 80.0f, {.top = 5.0f}, 2.0f));
}

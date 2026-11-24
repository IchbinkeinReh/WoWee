// How wide a tooltip is (ui/tooltip_layout.hpp, 0x0061caf0).
#include <catch_amalgamated.hpp>

#include "ui/tooltip_layout.hpp"

namespace tl = wowee::ui::tooltip_layout;

namespace {
const auto kWhole = [](size_t, float width) { return width; };
}

TEST_CASE("a tooltip is as wide as its widest line", "[tooltip_layout]") {
    CHECK(tl::widths({{.left = 28.0f}}, 0.0f, false, kWhole).lines == 28.0f);
    CHECK(tl::widths({{.left = 28.0f}, {.left = 90.0f}}, 0.0f, false, kWhole).lines == 90.0f);
    // Its two sides with room between; one that is hidden is none.
    CHECK(tl::widths({{.rightShown = true, .left = 10.0f, .right = 20.0f}}, 0.0f, false, kWhole).lines ==
          Catch::Approx(68.4f));
    CHECK(tl::widths({{.leftShown = false, .rightShown = true, .left = 10.0f, .right = 20.0f}}, 0.0f, false, kWhole).lines ==
          20.0f);
    CHECK(tl::widths({{.left = 10.0f, .texture = 15.0f}}, 0.0f, false, kWhole).lines == 25.0f);
    // No narrower than asked.
    CHECK(tl::widths({{.left = 28.0f}}, 100.0f, false, kWhole).lines == 100.0f);
    CHECK(tl::widths({}, 0.0f, false, kWhole).lines == 0.0f);
}

TEST_CASE("a line that wraps asks for 230.4 at most", "[tooltip_layout]") {
    // Broken there, the widest of its rows counts.
    const auto rows = [](size_t i, float width) {
        CHECK(i == 1);
        CHECK(width == tl::kWrap);
        return 221.0f;
    };
    const auto w = tl::widths({{.left = 50.0f}, {.wrap = true, .left = 900.0f}}, 0.0f, false, rows);
    CHECK(w.lines == 221.0f);
    CHECK(w.wrapAt == std::vector<float>{0.0f, 221.0f});
    // A short one as much as it needs, and none where another is wider.
    CHECK(tl::widths({{.wrap = true, .left = 60.0f}}, 0.0f, false, kWhole).lines == 60.0f);
    const auto wide = tl::widths({{.left = 320.0f}, {.wrap = true, .left = 900.0f}}, 0.0f, false,
                                 [](size_t, float) -> float { FAIL("not asked"); return 0.0f; });
    CHECK(wide.lines == 320.0f);
    CHECK(wide.wrapAt[1] == 320.0f);
}

TEST_CASE("a minimum that is forced wraps every line", "[tooltip_layout]") {
    const auto w = tl::widths({{.left = 500.0f}, {.left = 40.0f}}, 150.0f, true, [](size_t, float) { return 200.0f; });
    CHECK(w.lines == 200.0f);
    CHECK(w.wrapAt == std::vector<float>{200.0f, 200.0f});
}

TEST_CASE("a tooltip line without a colour is gold", "[tooltip_layout]") {
    using wowee::ui::tooltip_layout::lineColour;
    CHECK(lineColour(false, {0.0f, 1.0f, 1.0f}) == std::array<float, 3>{1.0f, 210.0f / 255.0f, 0.0f});
    CHECK(lineColour(true, {0.25f, 0.0f, 0.5f}) == std::array<float, 3>{0.25f, 0.0f, 0.5f});
    CHECK(lineColour(true, {-1.0f, 2.0f, 1.0f}) == std::array<float, 3>{0.0f, 1.0f, 1.0f});
}

TEST_CASE("a tooltip line's hidden side takes no room", "[tooltip_layout]") {
    using wowee::ui::tooltip_layout::Line;
    const auto none = [](size_t, float width) { return width; };
    const auto wide = [&](Line line) { return wowee::ui::tooltip_layout::widths({line}, 0.0f, false, none).lines; };
    CHECK(wide({.left = 20.0f, .right = 50.0f}) == 20.0f);
    CHECK(wide({.leftShown = false, .rightShown = true, .left = 20.0f, .right = 50.0f}) == 50.0f);
    CHECK(wide({.rightShown = true, .left = 20.0f, .right = 50.0f}) == 108.4f);
}

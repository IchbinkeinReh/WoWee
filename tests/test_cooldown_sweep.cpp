// A Cooldown frame's sweep and its flash (ui/cooldown_sweep.hpp), and that the
// frame is drawn while it has one.
#include <catch_amalgamated.hpp>

#include "ui/cooldown_sweep.hpp"
#include "ui/widget_tree.hpp"

namespace cd = wowee::ui::cooldown;
using wowee::ui::WidgetKind;
using wowee::ui::WidgetTree;

TEST_CASE("a cooldown is uncovered clockwise from twelve", "[cooldown]") {
    // The hand: an eighth of the time to each half side.
    CHECK(cd::hand(0.0f) == cd::Point{0.5f, 0.0f});
    CHECK(cd::hand(0.125f) == cd::Point{1.0f, 0.0f});
    CHECK(cd::hand(0.0625f) == cd::Point{0.75f, 0.0f});
    CHECK(cd::hand(0.25f) == cd::Point{1.0f, 0.5f});
    CHECK(cd::hand(0.5f) == cd::Point{0.5f, 1.0f});
    CHECK(cd::hand(0.75f) == cd::Point{0.0f, 0.5f});
    CHECK(cd::hand(1.0f) == cd::Point{0.5f, 0.0f});
    // All of it at first, nothing at the end.
    const auto all = cd::covered(0.0f, false);
    REQUIRE(all.size() == 9);
    CHECK(all.front() == cd::Point{0.5f, 0.0f});
    CHECK(all[1] == cd::Point{1.0f, 0.0f});
    CHECK(all.back() == cd::Point{0.5f, 0.0f});
    const auto none = cd::covered(1.0f, false);
    REQUIRE(none.size() == 2);
    CHECK(none[0] == none[1]);
    // Half of it: what is left is the left half.
    const auto half = cd::covered(0.5f, false);
    for (const auto& p : half) CHECK(p.x <= 0.5f);
    REQUIRE(half.size() == 5);
    CHECK(half.front() == cd::Point{0.5f, 1.0f});
}

TEST_CASE("reversed it covers what has passed", "[cooldown]") {
    const auto half = cd::covered(0.5f, true);
    for (const auto& p : half) CHECK(p.x >= 0.5f);
    CHECK(half.front() == cd::Point{0.5f, 0.0f});
    CHECK(half.back() == cd::Point{0.5f, 1.0f});
    const auto none = cd::covered(0.0f, true);
    CHECK(none.front() == none.back());
    CHECK(cd::covered(1.0f, true).size() == 9);
}

// 0x005ec8f0.
TEST_CASE("a cooldown flashes for a second and goes", "[cooldown]") {
    cd::Clock c{.start = 100.0, .duration = 10.0};
    CHECK(c.passed(105.0) == 0.5f);
    CHECK_FALSE(cd::advance(c, false, 109.9));
    CHECK_FALSE(c.flash);
    CHECK_FALSE(cd::advance(c, false, 110.2));
    CHECK(c.flash);
    CHECK(c.passed(110.2) == 0.0f);
    CHECK_FALSE(cd::advance(c, false, 111.1));
    CHECK(cd::advance(c, false, 111.2));
    CHECK_FALSE(c.runs());
    CHECK_FALSE(cd::advance(c, false, 300.0));
    // Reversed it stays, covered.
    cd::Clock r{.start = 100.0, .duration = 10.0};
    CHECK_FALSE(cd::advance(r, true, 200.0));
    CHECK_FALSE(r.flash);
    CHECK(r.passed(200.0) == 1.0f);
    // None, and one that has not begun.
    cd::Clock none;
    CHECK_FALSE(cd::advance(none, false, 5.0));
    CHECK_FALSE(none.runs());
    cd::Clock later{.start = 100.0, .duration = 10.0};
    CHECK_FALSE(cd::advance(later, false, 99.0));
    CHECK(later.passed(99.0) == 0.0f);
}

TEST_CASE("the flash swells, turns and fades in six steps", "[cooldown]") {
    CHECK(cd::flashAt(0.0f).alpha == 0.0f);
    CHECK(cd::flashAt(1.0f / 6.0f).alpha == Catch::Approx(0.5f).margin(1e-4));
    CHECK(cd::flashAt(1.0f / 3.0f).alpha == Catch::Approx(1.0f).margin(1e-4));
    CHECK(cd::flashAt(1.0f / 3.0f).extent == Catch::Approx(0.5405f).margin(1e-4));
    CHECK(cd::flashAt(0.25f).alpha == Catch::Approx(0.75f).margin(1e-4));
    CHECK(cd::flashAt(0.5f).turn == Catch::Approx(-0.3927f).margin(1e-4));
    CHECK(cd::flashAt(1.0f).alpha == 0.0f);
    CHECK(cd::flashAt(1.0f).turn == Catch::Approx(-0.7854f).margin(1e-4));
    const auto still = cd::turnedCorners(0.0f, 1.0f);
    CHECK(still[0] == cd::Point{0.0f, 0.0f});
    CHECK(still[1] == cd::Point{1.0f, 0.0f});
    CHECK(still[2] == cd::Point{0.0f, 1.0f});
    CHECK(still[3] == cd::Point{1.0f, 1.0f});
    // A quarter turn samples the bottom left where the top left was not.
    const auto quarter = cd::turnedCorners(-1.5707963f, 0.5f);
    CHECK(quarter[0].x == Catch::Approx(0.25f).margin(1e-4));
    CHECK(quarter[0].y == Catch::Approx(0.75f).margin(1e-4));
}

TEST_CASE("a cooldown that runs is drawn", "[cooldown]") {
    WidgetTree tree;
    const uint32_t id = tree.create(WidgetKind::Frame, tree.rootId(), "Sweep");
    tree.setWidth(id, 36.0f);
    tree.setHeight(id, 36.0f);
    wowee::ui::Anchor a;
    a.point = "CENTER";
    a.relativePoint = "CENTER";
    tree.addPoint(id, a);
    tree.get(id)->isCooldown = true;
    const auto drawn = [&] {
        tree.layout(800.0f, 600.0f);
        for (const auto* w : tree.drawOrder())
            if (w->id == id) return true;
        return false;
    };
    CHECK_FALSE(drawn());
    tree.get(id)->cooldownClock = {.start = 1.0, .duration = 10.0};
    CHECK(drawn());
    tree.get(id)->cooldownClock = {};
    CHECK_FALSE(drawn());
}

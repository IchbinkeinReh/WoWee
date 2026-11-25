// The WorldFrame and the mouse (ui/world_mouse.hpp).
#include <catch_amalgamated.hpp>

#include "ui/world_mouse.hpp"

using namespace wowee::ui;

TEST_CASE("over the world the mouse is on its frame", "[world_mouse]") {
    CHECK(mouseFocus(7, 3, true) == 7);
    CHECK(mouseFocus(0, 3, true) == 3);
    CHECK(mouseFocus(0, 3, false) == 0);
    CHECK(mouseFocus(0, 0, true) == 0);
}

TEST_CASE("a button is let go on what it was pressed on", "[world_mouse]") {
    WorldPresses<3> presses;
    CHECK_FALSE(presses.up(0));
    CHECK(presses.down(0, 0));
    CHECK_FALSE(presses.down(1, 5));
    CHECK_FALSE(presses.up(1));
    CHECK(presses.up(0));
    CHECK_FALSE(presses.up(0));
    CHECK(presses.down(2, 0));
    CHECK_FALSE(presses.down(2, 9));
    CHECK_FALSE(presses.up(2));
}

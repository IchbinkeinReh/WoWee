// The world state frame's lines as the client chooses them from
// WorldStateUI.dbc (0x005487d0) and writes their text (0x005770d0,
// 0x00576e50).
#include <catch_amalgamated.hpp>

#include <map>

#include "game/world_state_ui.hpp"

namespace ws = wowee::game::world_state_ui;

TEST_CASE("Rows are shown by map, area, phase and type", "[world_state_ui]") {
    std::vector<ws::Row> rows(6);
    rows[0].mapId = -1;                       // any map
    rows[1].mapId = 489;                      // Warsong Gulch
    rows[2].mapId = 0;   rows[2].areaId = 1519;  // Stormwind's zone
    rows[3].mapId = 0;   rows[3].phaseShift = 0x40;
    rows[4].mapId = 0;   rows[4].type = 2;    // not a frame line
    rows[5].mapId = 0;   rows[5].areaId = 5;
    ws::Place place{.mapId = 0, .zoneId = 1519, .areaId = 4411, .phaseMask = 1};
    CHECK(ws::shownRows(rows, place) == std::vector<size_t>{0, 2});
    place.phaseMask = 0x41;
    CHECK(ws::shownRows(rows, place) == std::vector<size_t>{0, 2, 3});
    place.areaId = 5;
    CHECK(ws::shownRows(rows, place) == std::vector<size_t>{0, 2, 3, 5});
    place.mapId = 489;
    CHECK(ws::shownRows(rows, place) == std::vector<size_t>{0, 1});
}

TEST_CASE("A line's text takes the world states it names", "[world_state_ui]") {
    const std::map<uint32_t, uint32_t> states = {{1581, 2}, {4248, 1000 + 125}};
    const ws::StateValue value = [&](uint32_t id) {
        const auto it = states.find(id);
        return it != states.end() ? it->second : 0u;
    };
    CHECK(ws::text("Alliance: %1581w/3", value, 0) == "Alliance: 2/3");
    CHECK(ws::text("%999W", value, 0) == "0");
    // A time to come, from now.
    CHECK(ws::text("Next: %4248k", value, 1000) == "Next: 02:05");
    CHECK(ws::timeLeft(1000 + 3725, 1000) == "1:02:05");
    CHECK(ws::timeLeft(10, 20) == "00:00");
    // Any other '%' stays, the digits after it dropped.
    CHECK(ws::text("100% sure, %5x", value, 0) == "100% sure, %x");
}

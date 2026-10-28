// /gps: the client's position as a line `.go xyz` takes back.
#include <catch_amalgamated.hpp>
#include "ui/chat/client_gps.hpp"

using namespace wowee::ui::client_gps;

TEST_CASE("/gps prints a .go xyz line in the server's order", "[client_gps]") {
    // The Undercity spot of the barber shadow report.
    CHECK(goXyzLine(0, 1647.834f, 201.512f, -62.18f, 1.046f) ==
          ".go xyz 1647.834 201.512 -62.180 0 1.0460");
    // Darnassus' temple terrace, map 1.
    CHECK(goXyzLine(1, 9949.56f, 2284.21f, 1341.394f, 4.8505588f) ==
          ".go xyz 9949.560 2284.210 1341.394 1 4.8506");
}

TEST_CASE("/gps gives the facing in [0, 2 pi)", "[client_gps]") {
    CHECK(wrapOrientation(-1.0f) == Catch::Approx(6.28318530718f - 1.0f));
    CHECK(wrapOrientation(7.0f) == Catch::Approx(7.0f - 6.28318530718f));
    CHECK(wrapOrientation(0.0f) == 0.0f);
    CHECK(goXyzLine(0, 0.0f, 0.0f, 0.0f, -0.5f) == ".go xyz 0.000 0.000 0.000 0 5.7832");
}

TEST_CASE("/gps names the zone and the subzone", "[client_gps]") {
    CHECK(zoneLine("Undercity", "Trade Quarter") == "Zone: Undercity - Trade Quarter");
    CHECK(zoneLine("Darnassus", "Darnassus") == "Zone: Darnassus");
    CHECK(zoneLine("Darnassus", "") == "Zone: Darnassus");
    CHECK(zoneLine("", "").empty());
}

TEST_CASE("/gps shows the sent position only where it parts from the drawn one", "[client_gps]") {
    CHECK_FALSE(differs(1.0f, 2.0f, 3.0f, 1.01f, 2.0f, 3.0f));
    CHECK(differs(1.0f, 2.0f, 3.0f, 1.0f, 2.0f, 15.5f));
}

TEST_CASE("/gps reports the camera as a line /cam takes back", "[client_gps]") {
    const CameraPose c{12.4f, 20.0f, -35.5f};
    CHECK(cameraLine(c) == "Camera: distance 12.40 pitch 20.00 yaw -35.50");
    const auto back = parseCamera(cameraLine(c));
    REQUIRE(back);
    CHECK(back->distance == Catch::Approx(12.4f));
    CHECK(back->pitch == Catch::Approx(20.0f));
    CHECK(back->yaw == Catch::Approx(-35.5f));
    // Or the three numbers alone.
    const auto bare = parseCamera("8 -10.5 90");
    REQUIRE(bare);
    CHECK(bare->distance == 8.0f);
    CHECK(bare->pitch == -10.5f);
    CHECK(bare->yaw == 90.0f);
    // Not a pose.
    CHECK_FALSE(parseCamera(""));
    CHECK_FALSE(parseCamera("8 10"));
    CHECK_FALSE(parseCamera("8 ten 0"));
    CHECK_FALSE(parseCamera("-1 0 0"));
    CHECK_FALSE(parseCamera("nan 0 0"));
}

TEST_CASE("/gps reports the time of day the light is drawn for", "[client_gps]") {
    CHECK(clockText(19.5f) == "19:30");
    CHECK(clockText(4.72f) == "04:43");
    CHECK(clockText(23.999f) == "00:00");
    CHECK(clockText(-0.5f) == "23:30");
    CHECK(timeLine(14.08f, -1.0f) == "Time: 14:05");
    // The Debug Time override, with the value /daytime takes back.
    CHECK(timeLine(19.5f, 19.5f) == "Time: 19:30 (Debug Time 19.50)");
}

TEST_CASE("/daytime takes hours, a clock, off, or the /gps line", "[client_gps]") {
    auto hours = [](const std::string& s) {
        const auto t = parseTime(s);
        REQUIRE(t);
        CHECK_FALSE(t->off);
        return t->hours;
    };
    CHECK(hours("19.5") == Catch::Approx(19.5f));
    CHECK(hours("19:30") == Catch::Approx(19.5f));
    CHECK(hours("4:43") == Catch::Approx(4.0f + 43.0f / 60.0f));
    CHECK(hours("Time: 14:05") == Catch::Approx(14.0f + 5.0f / 60.0f));
    // The override's own value wins over the rounded clock.
    CHECK(hours("Time: 04:43 (Debug Time 4.72)") == Catch::Approx(4.72f));
    const auto off = parseTime("off");
    REQUIRE(off);
    CHECK(off->off);
    CHECK_FALSE(parseTime(""));
    CHECK_FALSE(parseTime("noon"));
    CHECK_FALSE(parseTime("25"));
    CHECK_FALSE(parseTime("12:75"));
    CHECK_FALSE(parseTime("-1"));
}

// layout-local.txt (0x00517e00 writes, 0x005183a0 reads).
#include <catch_amalgamated.hpp>

#include "ui/layout_cache.hpp"

using namespace wowee::ui::layout_cache;

TEST_CASE("A frame is saved by the point nearest its parent's", "[layout_cache]") {
    // A 100x50 frame near the parent's top right corner.
    Saved s;
    s.name = "MyFrame";
    s.parentRect = {0.0f, 0.0f, 1024.0f, 768.0f};
    s.rect = {900.0f, 700.0f, 1000.0f, 750.0f};
    s.level = 5;
    const Entry e = entryFor(s);
    REQUIRE(std::string(kPoints[e.point]) == "TOPRIGHT");
    REQUIRE(e.x == -24);
    REQUIRE(e.y == -18);
    REQUIRE(e.w == 100);
    REQUIRE(e.h == 50);
    REQUIRE(e.level == 5);
}

TEST_CASE("Offsets and size are in the frame's own units", "[layout_cache]") {
    Saved s;
    s.parentRect = {0.0f, 0.0f, 1000.0f, 1000.0f};
    s.rect = {10.0f, 10.0f, 210.0f, 110.0f};
    s.effectiveScale = 2.0f;
    const Entry e = entryFor(s);
    REQUIRE(std::string(kPoints[e.point]) == "BOTTOMLEFT");
    REQUIRE(e.x == 5);
    REQUIRE(e.y == 5);
    REQUIRE(e.w == 100);
    REQUIRE(e.h == 50);
}

TEST_CASE("The first of equally near points wins", "[layout_cache]") {
    // The same rect as its parent: every point is at distance 0, TOPLEFT first.
    const Placement p = nearestPoint({0, 0, 10, 10}, {0, 0, 10, 10});
    REQUIRE(p.point == 0);
}

TEST_CASE("What is written is read back", "[layout_cache]") {
    std::vector<Entry> in(2);
    in[0] = {"PlayerFrame", 3, 0, -19, -4, 232, 100};
    in[1] = {"Bag1", -1, 8, -10, 50, 0, 0};
    const std::string text = serialize(in);
    REQUIRE(text.rfind("Version: 1\nFrame: PlayerFrame\nFrameLevel: 3\nAnchor: TOPLEFT\nX: -19\n", 0) == 0);
    const auto out = parse(text);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].name == "PlayerFrame");
    REQUIRE(out[0].level == 3);
    REQUIRE(out[0].x == -19);
    REQUIRE(out[0].h == 100);
    REQUIRE(out[1].name == "Bag1");
    REQUIRE(out[1].point == 8);
    REQUIRE(out[1].y == 50);
}

TEST_CASE("Keys are read with case ignored and CRLF lines", "[layout_cache]") {
    const auto out = parse("version: 1\r\nframe: A\r\nanchor: center\r\nx: 7\r\n");
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].name == "A");
    REQUIRE(out[0].point == 4);
    REQUIRE(out[0].x == 7);
    REQUIRE(out[0].level == -1);
}

TEST_CASE("Lines before any frame name nothing", "[layout_cache]") {
    REQUIRE(parse("Version: 1\nX: 4\n").empty());
    REQUIRE(pointIndex("NOWHERE") == -1);
}

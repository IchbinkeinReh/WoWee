// Which casts' delays and channels the client acts on (0x00801b80,
// 0x00801c90, 0x00801db0).
#include <catch_amalgamated.hpp>

#include "game/cast_event_gate.hpp"

using wowee::game::castEventShown;

TEST_CASE("an ordinary spell is shown for anyone", "[casteventgate]") {
    CHECK(castEventShown(0, 0, 0, true, false));
    CHECK(castEventShown(0, 0, 0, false, false));
    CHECK(castEventShown(0, 0, 0, false, true));
}

TEST_CASE("Attributes 0x2 hides it even for the player", "[casteventgate]") {
    CHECK_FALSE(castEventShown(0x2, 0, 0, true, false));
    CHECK_FALSE(castEventShown(0x2, 0, 0, false, true));
}

TEST_CASE("AttributesEx4 0x8000000 shows it whatever else", "[casteventgate]") {
    CHECK(castEventShown(0x2 | 0x180, 0x2000, 0x8000000 | 0x1, false, true));
}

TEST_CASE("other units' casts hide on 0x180 and Ex4 0x1", "[casteventgate]") {
    CHECK_FALSE(castEventShown(0x80, 0, 0, false, false));
    CHECK_FALSE(castEventShown(0x100, 0, 0, false, false));
    CHECK_FALSE(castEventShown(0, 0, 0x1, false, false));
    CHECK(castEventShown(0x180, 0, 0x1, true, false));
}

TEST_CASE("AttributesEx3 0x2000 hides only another unit's channel", "[casteventgate]") {
    CHECK_FALSE(castEventShown(0, 0x2000, 0, false, true));
    CHECK(castEventShown(0, 0x2000, 0, false, false));
    CHECK(castEventShown(0, 0x2000, 0, true, true));
}

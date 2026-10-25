// The cast a unit shows (game/unit_cast_visual.hpp): +0xa5c/+0xa60 and
// +0xa38's 0x400 and 0x200.
#include <catch_amalgamated.hpp>

#include "game/unit_cast_visual.hpp"

namespace cv = wowee::game::cast_visual;

namespace {
cv::Begin cast(uint8_t count, uint32_t spell, int32_t castTime, bool animates = true) {
    cv::Begin b;
    b.castCount = count;
    b.spellId = spell;
    b.castTimeMs = castTime;
    b.spell.precastKitAnimates = animates;
    return b;
}
}  // namespace

TEST_CASE("a started cast is shown until it ends", "[cast_visual]") {
    cv::Roster r;
    r.started(7, cast(3, 133, 2500));
    REQUIRE(r.state(7));
    CHECK(r.state(7)->spellId == 133);
    CHECK(r.state(7)->precastAnimating);
    // Another count of it ends nothing.
    r.ended(7, 4, 133);
    CHECK(r.state(7)->spellId == 133);
    r.ended(7, 3, 133);
    CHECK(r.state(7)->spellId == 0);
    CHECK_FALSE(r.state(7)->precastAnimating);
}

TEST_CASE("an instant shows only when nothing is shown", "[cast_visual]") {
    cv::Roster r;
    r.started(7, cast(1, 100, 2000));
    r.started(7, cast(2, 200, 0));
    CHECK(r.state(7)->spellId == 100);
    // A cast with a time takes the place of a different one.
    r.started(7, cast(3, 300, 1500, false));
    CHECK(r.state(7)->spellId == 300);
    CHECK_FALSE(r.state(7)->precastAnimating);
}

TEST_CASE("another's cast with a time but such attributes is not shown over one", "[cast_visual]") {
    cv::Roster r;
    r.started(7, cast(1, 100, 2000));
    auto b = cast(2, 200, 1500);
    b.spell.attributes = 0x100;
    r.started(7, b);
    CHECK(r.state(7)->spellId == 100);
    // The active player's is.
    b.casterIsActivePlayer = true;
    r.started(7, b);
    CHECK(r.state(7)->spellId == 200);
    // Attributes 0x2 never, unless AttributesEx4 0x8000000.
    auto c = cast(3, 300, 1500);
    c.spell.attributes = 0x2;
    c.casterIsActivePlayer = true;
    r.started(7, c);
    CHECK(r.state(7)->spellId == 200);
    c.spell.attributesEx4 = 0x8000000;
    r.started(7, c);
    CHECK(r.state(7)->spellId == 300);
}

TEST_CASE("the player's send is shown, kept by its own start, ended by another's", "[cast_visual]") {
    cv::Roster r;
    r.sent(1, cast(5, 400, 0));
    CHECK(r.state(1)->spellId == 400);
    // Its own start: nothing changes.
    r.started(1, cast(5, 400, 0));
    CHECK(r.state(1)->spellId == 400);
    // A second send, then a different cast started: the send shown goes.
    cv::Roster s;
    s.sent(1, cast(6, 500, 0));
    s.started(1, cast(9, 600, 0, false));
    CHECK(s.state(1)->spellId == 600);
}

TEST_CASE("an aimed cast object shows nothing when nothing is shown", "[cast_visual]") {
    cv::Roster r;
    auto b = cast(1, 700, 0);
    b.aimed = true;
    r.sent(1, b);
    CHECK(r.state(1)->spellId == 0);
}

TEST_CASE("an auto-repeat visual animates once until auto-repeat stops", "[cast_visual]") {
    cv::Roster r;
    auto shot = cast(1, 75, 500);
    shot.spell.attributesEx2 = cv::kAttrEx2AutoRepeat;
    r.started(1, shot);
    CHECK(r.state(1)->precastAnimating);
    CHECK(r.state(1)->autoRepeatShown);
    r.ended(1, 1, 75);
    shot.castCount = 2;
    r.started(1, shot);
    CHECK(r.state(1)->spellId == 75);
    CHECK_FALSE(r.state(1)->precastAnimating);
    r.ended(1, 2, 75);
    r.autoRepeatStopped(1);
    shot.castCount = 3;
    r.started(1, shot);
    CHECK(r.state(1)->precastAnimating);
    r.forget(1);
    CHECK(r.state(1) == nullptr);
}

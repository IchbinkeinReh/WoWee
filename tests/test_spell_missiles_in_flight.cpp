// The casts whose missile the client still has flying (0x00af5274), which
// hold a dynamic object's model hidden (0x00804cc0).
#include <catch_amalgamated.hpp>

#include "game/spell_missiles_in_flight.hpp"

using namespace wowee::game::spell_missiles;

TEST_CASE("a missile's flight is distance over speed, in ms", "[spell_missiles]") {
    // 0x0080e1b0: round(distance / speed * -1000), taken from now.
    CHECK(flightMs(30.0f, 20.0f) == 1500);
    CHECK(flightMs(0.0f, 20.0f) == 0);
    CHECK(flightMs(10.0f, 3.0f) == 3333);
}

TEST_CASE("a cast is in flight until its arrival", "[spell_missiles]") {
    InFlightList list;
    list.add({.caster = 0x42, .spellId = 42208, .castTime = 1000, .arrivesAt = 5000});
    // 0x00804cc0 matches the caster, the spell and the cast's time.
    CHECK(list.contains(0x42, 42208, 1000));
    CHECK_FALSE(list.contains(0x43, 42208, 1000));
    CHECK_FALSE(list.contains(0x42, 42209, 1000));
    CHECK_FALSE(list.contains(0x42, 42208, 1001));
    // 0x00804c10: kept before its time, dropped at it.
    list.expire(4999);
    CHECK(list.contains(0x42, 42208, 1000));
    list.expire(5000);
    CHECK_FALSE(list.contains(0x42, 42208, 1000));
    CHECK(list.size() == 0);
}

TEST_CASE("the list is kept in order of arrival", "[spell_missiles]") {
    InFlightList list;
    list.add({.caster = 1, .spellId = 1, .castTime = 1, .arrivesAt = 300});
    list.add({.caster = 2, .spellId = 2, .castTime = 2, .arrivesAt = 100});
    list.add({.caster = 3, .spellId = 3, .castTime = 3, .arrivesAt = 200});
    // 0x00804c10 stops at the first not yet arrived, so the order is what
    // lets each go at its time.
    list.expire(150);
    CHECK_FALSE(list.contains(2, 2, 2));
    CHECK(list.contains(3, 3, 3));
    CHECK(list.contains(1, 1, 1));
    list.expire(250);
    CHECK_FALSE(list.contains(3, 3, 3));
    CHECK(list.contains(1, 1, 1));
    // 0x008047a0, as the world goes.
    list.clear();
    CHECK(list.size() == 0);
}

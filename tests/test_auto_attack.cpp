// The player's own attack (game/auto_attack.hpp): asked for at any distance,
// on until the server ends it, too far when the server says so.
#include <catch_amalgamated.hpp>

#include "game/auto_attack.hpp"

using namespace wowee::game;
using Why = SwingRefusal::Why;

TEST_CASE("a swing is asked for once, until its stop is", "[auto_attack]") {
    AutoAttack a;
    CHECK(a.maySwing());
    CHECK_FALSE(a.mayStop());
    a.swingSent(7);
    CHECK(a.attacking());
    CHECK_FALSE(a.maySwing());
    CHECK(a.mayStop());
    a.started(7);
    CHECK(a.attacking());
    CHECK_FALSE(a.maySwing());
    // Stopped here it is on until the server's answer, and another may be
    // asked for meanwhile - a change of target does both.
    a.stopSent();
    CHECK(a.attacking());
    CHECK(a.maySwing());
    a.swingSent(8);
    a.stopped();
    CHECK_FALSE(a.attacking());
    CHECK(a.maySwing());
    a.started(8);
    CHECK(a.target == 8);
}

TEST_CASE("a refused swing is said at once and every four seconds", "[auto_attack]") {
    SwingRefusal r;
    CHECK(r.due(true, 1000) == Why::None);
    r.refused(Why::Distance, 1000);
    CHECK(r.due(true, 1000) == Why::Distance);
    CHECK(r.due(true, 1001) == Why::None);
    CHECK(r.due(true, 4999) == Why::None);
    CHECK(r.due(true, 5000) == Why::Distance);
    SECTION("until a swing comes off") {
        r.clear(5100);
        CHECK(r.due(true, 9100) == Why::None);
    }
    SECTION("or the attack is over") { CHECK(r.due(false, 9100) == Why::None); }
    SECTION("another reason is said at once") {
        r.refused(Why::Facing, 5100);
        CHECK(r.due(true, 5100) == Why::Facing);
    }
}

TEST_CASE("a swing that comes off four reaches away is the last", "[auto_attack]") {
    // Never under five yards of reach: twenty yards.
    CHECK_FALSE(swingLeftBehind(19.9f * 19.9f, 1.5f, 1.5f));
    CHECK(swingLeftBehind(20.0f * 20.0f, 1.5f, 1.5f));
    // A mammoth's eight yards of reach, at the edge of which it is hit.
    CHECK_FALSE(swingLeftBehind(40.0f * 40.0f, 1.5f, 8.0f));
    CHECK(swingLeftBehind(43.5f * 43.5f, 1.5f, 8.0f));
}

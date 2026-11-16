// Aura 48's client timers (0x0080df10, 0x0080ddd0, 0x0080dfe0).
#include <catch_amalgamated.hpp>

#include "game/client_periodic_triggers.hpp"

#include <utility>
#include <vector>

using wowee::game::ClientTriggerTimers;

TEST_CASE("a client trigger casts every period from when it was added", "[client_triggers]") {
    ClientTriggerTimers t;
    t.add(7, 100, 200, 1000, 5000);
    std::vector<std::pair<uint64_t, uint32_t>> cast;
    const auto record = [&](uint64_t u, uint32_t s) { cast.emplace_back(u, s); };
    t.fire(5999, record);
    CHECK(cast.empty());
    t.fire(6000, record);
    REQUIRE(cast.size() == 1);
    CHECK(cast[0] == std::pair<uint64_t, uint32_t>{7, 200});
    // Late: once, and due a period from then.
    t.fire(8500, record);
    CHECK(cast.size() == 2);
    t.fire(9499, record);
    CHECK(cast.size() == 2);
    t.fire(9500, record);
    CHECK(cast.size() == 3);
}

TEST_CASE("client triggers come off by spell or all of a unit's", "[client_triggers]") {
    ClientTriggerTimers t;
    t.add(1, 10, 11, 100, 0);
    t.add(1, 20, 21, 100, 0);
    t.add(2, 10, 11, 100, 0);
    t.remove(1, 10);
    CHECK(t.timers().size() == 2);
    t.remove(1, 0);
    REQUIRE(t.timers().size() == 1);
    CHECK(t.timers()[0].unit == 2);
}

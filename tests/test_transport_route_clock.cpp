// tests/test_transport_route_clock.cpp
// A transport's server path time on the client's steady clock: wrap-safe
// millisecond arithmetic, the path-time modulo, and the resync's slew and snap.
#include <catch_amalgamated.hpp>
#include "game/transport_route_clock.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace wowee::game;

namespace {
constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();
}  // namespace

TEST_CASE("Route clock: a 32-bit ms difference is right across the wrap", "[transport_route_clock]") {
    CHECK(wrapMsDiff(1500u, 500u) == 1000);
    CHECK(wrapMsDiff(500u, 1500u) == -1000);
    // 16 ms before the wrap to 5 ms after it: 21 ms on, not four billion back.
    CHECK(wrapMsDiff(5u, kU32Max - 15u) == 21);
    CHECK(wrapMsDiff(kU32Max - 15u, 5u) == -21);
    CHECK(wrapMsDiff(0u, kU32Max) == 1);
    // The largest step either way that still reads right.
    CHECK(wrapMsDiff(0x7FFFFFFFu, 0u) == std::numeric_limits<int32_t>::max());
    CHECK(wrapMsDiff(0x80000000u, 0u) == std::numeric_limits<int32_t>::min());
}

TEST_CASE("Route clock: the path-time modulo is never negative and never overflows",
          "[transport_route_clock]") {
    CHECK(positiveModMs(0, 4000) == 0u);
    CHECK(positiveModMs(4001, 4000) == 1u);
    CHECK(positiveModMs(-1, 4000) == 3999u);
    CHECK(positiveModMs(-4000, 4000) == 0u);
    CHECK(positiveModMs(3 * kDayMs + 17, 4000) == static_cast<uint32_t>((3 * kDayMs + 17) % 4000));
    CHECK(positiveModMs(std::numeric_limits<int64_t>::max(), 400000) < 400000u);
    CHECK(positiveModMs(std::numeric_limits<int64_t>::min(), 400000) < 400000u);
    CHECK(positiveModMs(12345, 0) == 0u);
}

TEST_CASE("Route clock: the server's progress wraps where its 32-bit counter does",
          "[transport_route_clock]") {
    // The server's progress counter is uint32 and the client adds its clock
    // to the offset in 32 bits (0x00710640), so a period that does not divide
    // 2^32 steps at the wrap on both sides alike - the client must follow it
    // there rather than run on smoothly into a phase the server has left.
    constexpr uint32_t kPeriod = 360000;
    TransportRouteClock clock;
    const int64_t t0 = 5000;
    REQUIRE(clock.apply(kU32Max - 999u, kPeriod, t0, true).outcome == TransportRouteClock::Outcome::Anchored);
    CHECK(clock.serverProgressAt(t0) == kU32Max - 999u);
    CHECK(clock.serverProgressAt(t0 + 999) == kU32Max);
    CHECK(clock.serverProgressAt(t0 + 1000) == 0u);
    CHECK(clock.serverProgressAt(t0 + 1500) == 500u);
    CHECK(clock.serverPathTimeAt(t0 + 1500) == 500u);
    CHECK(clock.serverPathTimeAt(t0) == (kU32Max - 999u) % kPeriod);
    CHECK(clock.pathTimeAt(t0 + 1500) == Catch::Approx(500.0));
}

TEST_CASE("Route clock: forty days of steady clock keep millisecond precision",
          "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 412345;
    const int64_t uptime = 40 * kDayMs;   // past what a float second count resolves to the ms
    TransportRouteClock clock;
    clock.apply(123456u, kPeriod, uptime, true);
    for (int64_t step : {0LL, 1LL, 7LL, 999LL, 86400000LL, 40 * kDayMs}) {
        const uint32_t expected =
            static_cast<uint32_t>(123456u + static_cast<uint32_t>(step)) % kPeriod;
        INFO("step " << step);
        CHECK(clock.serverPathTimeAt(uptime + step) == expected);
        CHECK(clock.pathTimeAt(uptime + step) == Catch::Approx(static_cast<double>(expected)).margin(1e-6));
    }
    // And a steady clock read 400 days into the machine's uptime.
    const int64_t longUptime = 400 * kDayMs;
    TransportRouteClock late;
    late.apply(0u, kPeriod, longUptime, true);
    CHECK(late.serverPathTimeAt(longUptime + 1) == 1u);
    CHECK(late.pathTimeAt(longUptime + kPeriod + 3) == Catch::Approx(3.0).margin(1e-6));
}

TEST_CASE("Route clock: a sample earlier than the anchor does not run the clock backwards",
          "[transport_route_clock]") {
    TransportRouteClock clock;
    clock.apply(1000u, 10000, 50000, true);
    CHECK(clock.serverProgressAt(49000) == 1000u);
}

TEST_CASE("Route clock: a small error is slewed out, not jumped", "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 300000;
    TransportRouteClock clock;
    const int64_t t0 = 1000000;
    clock.apply(10000u, kPeriod, t0, true);

    // Ten seconds on the client, and the server says it is 400 ms further on.
    const int64_t t1 = t0 + 10000;
    const double before = clock.pathTimeAt(t1);
    REQUIRE(before == Catch::Approx(20000.0));
    const auto r = clock.apply(20400u, kPeriod, t1, true);
    CHECK(r.outcome == TransportRouteClock::Outcome::Slewed);
    CHECK(r.errorMs == Catch::Approx(400.0));
    CHECK(r.oldPathMs == Catch::Approx(20000.0));
    CHECK(r.newPathMs == Catch::Approx(20400.0));
    CHECK(r.slewMs == 400 * TransportRouteClock::kSlewTimePerErrorMs);

    // Drawn where it was at the moment of the resync: no jump.
    CHECK(clock.pathTimeAt(t1) == Catch::Approx(before));
    // Catching up at no more than a quarter over the path's own rate, and
    // never going backwards.
    double last = clock.pathTimeAt(t1);
    for (int64_t dt = 10; dt <= r.slewMs; dt += 10) {
        const double now = clock.pathTimeAt(t1 + dt);
        const double rate = (now - last) / 10.0;
        CHECK(rate >= 1.0 - 1e-9);
        CHECK(rate <= 1.25 + 1e-9);
        last = now;
    }
    // And on the server's time once the slew has run.
    CHECK(clock.pathTimeAt(t1 + r.slewMs) == Catch::Approx(20400.0 + r.slewMs));
    CHECK(clock.pathTimeAt(t1 + r.slewMs + 5000) ==
          Catch::Approx(static_cast<double>(clock.serverPathTimeAt(t1 + r.slewMs + 5000))));
}

TEST_CASE("Route clock: a hull ahead of the server slows rather than reverses",
          "[transport_route_clock]") {
    TransportRouteClock clock;
    clock.apply(50000u, 300000, 0, true);
    const auto r = clock.apply(49700u, 300000, 0, true);   // 300 ms behind what is drawn
    REQUIRE(r.outcome == TransportRouteClock::Outcome::Slewed);
    CHECK(r.errorMs == Catch::Approx(-300.0));
    double last = clock.pathTimeAt(0);
    for (int64_t dt = 10; dt <= r.slewMs; dt += 10) {
        const double now = clock.pathTimeAt(dt);
        CHECK(now >= last);
        last = now;
    }
    CHECK(clock.pathTimeAt(r.slewMs) == Catch::Approx(49700.0 + r.slewMs));
}

TEST_CASE("Route clock: a large error snaps at once", "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 300000;
    TransportRouteClock clock;
    clock.apply(10000u, kPeriod, 0, true);
    // More than a second of path time apart: the forty seconds the
    // Thundercaller once ran behind.
    const auto r = clock.apply(50000u, kPeriod, 0, true);
    CHECK(r.outcome == TransportRouteClock::Outcome::Snapped);
    CHECK(r.errorMs == Catch::Approx(40000.0));
    CHECK(clock.pathTimeAt(0) == Catch::Approx(50000.0));
    CHECK(clock.slewRemainingMs(0) == 0.0);

    // Just over the threshold snaps; at it, slews.
    TransportRouteClock edge;
    edge.apply(0u, kPeriod, 0, true);
    CHECK(edge.apply(TransportRouteClock::kSnapThresholdMs, kPeriod, 0, true).outcome ==
          TransportRouteClock::Outcome::Slewed);
    TransportRouteClock over;
    over.apply(0u, kPeriod, 0, true);
    CHECK(over.apply(TransportRouteClock::kSnapThresholdMs + 1, kPeriod, 0, true).outcome ==
          TransportRouteClock::Outcome::Snapped);
}

TEST_CASE("Route clock: the error is taken the short way round the loop", "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 10000;
    TransportRouteClock clock;
    clock.apply(9990u, kPeriod, 0, true);
    // 20 ms on, past the end of the loop: a small step forward, not a jump of
    // nearly the whole period back.
    const auto r = clock.apply(10u, kPeriod, 0, true);
    CHECK(r.outcome == TransportRouteClock::Outcome::Slewed);
    CHECK(r.errorMs == Catch::Approx(20.0));
    CHECK(clock.pathTimeAt(0) == Catch::Approx(9990.0));
    CHECK(clock.pathTimeAt(r.slewMs) == Catch::Approx(static_cast<double>((10 + r.slewMs) % kPeriod)));
}

TEST_CASE("Route clock: a new period snaps", "[transport_route_clock]") {
    TransportRouteClock clock;
    clock.apply(1000u, 10000, 0, true);
    const auto r = clock.apply(1100u, 20000, 0, true);
    CHECK(r.outcome == TransportRouteClock::Outcome::Snapped);
    CHECK(r.periodChanged);
    CHECK(clock.periodMs() == 20000u);
    CHECK(clock.pathTimeAt(0) == Catch::Approx(1100.0));
}

TEST_CASE("Route clock: a weak sample only anchors a clock nothing better has",
          "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 300000;
    TransportRouteClock clock;
    // DYNAMIC alone: taken, and resynced by the next DYNAMIC.
    CHECK(clock.apply(1000u, kPeriod, 0, false).outcome == TransportRouteClock::Outcome::Anchored);
    CHECK_FALSE(clock.authoritative());
    CHECK(clock.apply(1200u, kPeriod, 0, false).outcome == TransportRouteClock::Outcome::Slewed);
    // The create or movement block's time: taken, and from now on the base.
    CHECK(clock.apply(90000u, kPeriod, 0, true).outcome == TransportRouteClock::Outcome::Snapped);
    CHECK(clock.authoritative());
    // DYNAMIC forty seconds off is compared, not believed.
    const auto r = clock.apply(50000u, kPeriod, 0, false);
    CHECK(r.outcome == TransportRouteClock::Outcome::Ignored);
    CHECK(r.errorMs == Catch::Approx(-40000.0));
    CHECK(clock.pathTimeAt(0) == Catch::Approx(90000.0));
}

TEST_CASE("Route clock: the periodic check snaps a drawn time that strays past its bound",
          "[transport_route_clock]") {
    constexpr uint32_t kPeriod = 300000;
    TransportRouteClock clock;
    clock.apply(10000u, kPeriod, 0, true);
    clock.apply(10800u, kPeriod, 0, true);   // 800 ms being slewed out
    double error = 0.0;
    // Inside the default bound: left to run out.
    CHECK_FALSE(clock.enforceBound(0, TransportRouteClock::kSanityBoundMs, &error));
    CHECK(error == Catch::Approx(-800.0));
    // A tighter bound than the slew: snapped to the server's time base.
    CHECK(clock.enforceBound(0, 100, &error));
    CHECK(clock.pathTimeAt(0) == Catch::Approx(10800.0));
    CHECK_FALSE(clock.enforceBound(0, 100, &error));
    CHECK(error == Catch::Approx(0.0));
}

TEST_CASE("Route clock: a zero period anchors nothing", "[transport_route_clock]") {
    TransportRouteClock clock;
    const auto r = clock.apply(1000u, 0, 0, true);
    CHECK(r.outcome == TransportRouteClock::Outcome::Ignored);
    CHECK_FALSE(clock.valid());
    CHECK(clock.pathTimeAt(0) == 0.0);
    CHECK_FALSE(clock.enforceBound(0, 1));
}

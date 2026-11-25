// A cooldown's numbers and its sweep (ui/cooldown_sweep.hpp; 0x005ece80, 0x005ec8f0).
#include <catch_amalgamated.hpp>

#include "ui/cooldown_sweep.hpp"

#include <limits>

using namespace wowee::ui::cooldown_sweep;

TEST_CASE("a cooldown of numbers that are none", "[cooldown]") {
    const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(msOf(1.2345) == 1235);
    for (const double odd : {inf, -inf, nan, 1e300, -1e300}) CHECK(msOf(odd) == 0);
    for (const double start : {0.0, 5.0, nan, inf, -1e10, 1e10}) {
        for (const double duration : {0.0, -5.0, 5.0, nan, inf, 1e10}) {
            State s;
            set(s, start, duration);
            for (const uint32_t now : {0u, 4000u, 4000000000u}) {
                for (const bool reverse : {false, true}) {
                    const float p = progress(s, reverse, now);
                    CHECK(p < 1.0001f);
                    if (p < 0.0f) continue;
                    std::array<Point, 10> out;
                    const int n = rim(p, reverse, out);
                    CHECK(n >= 2);
                    CHECK(n <= 10);
                }
            }
        }
    }
}

TEST_CASE("a cooldown's last second is its star's", "[cooldown]") {
    State s;
    set(s, 1.0, 2.0);
    CHECK_FALSE(update(s, false, 2999));
    CHECK(progress(s, false, 2000) == 0.5f);
    CHECK_FALSE(update(s, false, 3000));
    CHECK(s.flashing);
    CHECK_FALSE(update(s, false, 3999));
    CHECK(update(s, false, 4000));
    State reversed;
    set(reversed, 1.0, 2.0);
    CHECK_FALSE(update(reversed, true, 9000));
    CHECK_FALSE(reversed.flashing);
    CHECK(progress(reversed, true, 9000) > 0.99f);
}

// The lamps' wrapped N.L (rt_lamp_wrap.hpp) and the "Softer lamp light" switch.
#include <catch_amalgamated.hpp>
#include "rendering/rt_lamp_wrap.hpp"

#include <cmath>

using namespace wowee::rendering;

TEST_CASE("off is the 0.25 wrap, on the 0.5", "[rt_lamp_wrap]") {
    CHECK(lampWrap(false) == 0.25f);
    CHECK(lampWrap(true) == 0.5f);
}

TEST_CASE("a lamp straight on lights a face fully either way", "[rt_lamp_wrap]") {
    CHECK(wrappedNdotL(1.0f, lampWrap(false)) == Catch::Approx(1.0f));
    CHECK(wrappedNdotL(1.0f, lampWrap(true)) == Catch::Approx(1.0f));
}

TEST_CASE("the softer light reaches faces turned further away", "[rt_lamp_wrap]") {
    // A face turned 20 degrees past a lamp: nothing at 0.25, some at 0.5.
    const float past20 = -std::sin(20.0f * 3.14159265f / 180.0f);
    CHECK(wrappedNdotL(past20, lampWrap(false)) <= 0.0f);
    CHECK(wrappedNdotL(past20, lampWrap(true)) > 0.0f);
    // Thirty degrees is where even the softer light ends.
    CHECK(wrappedNdotL(-0.5f, lampWrap(true)) == Catch::Approx(0.0f).margin(1e-6));
}

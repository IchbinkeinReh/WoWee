// How a drunk player's facing sways (rendering/drunk_sway.hpp; 0x006cf3d0).
#include <catch_amalgamated.hpp>

#include "rendering/drunk_sway.hpp"

using wowee::rendering::drunkSway;

TEST_CASE("a drunk player's facing sways, a sober one's does not", "[drunk]") {
    CHECK(drunkSway(1234.0, 0.0f) == 0.0f);
    CHECK(drunkSway(0.0, 1.0f) == Catch::Approx(0.01308997f).epsilon(1e-4));
    CHECK(drunkSway(0.0, 0.5f) == Catch::Approx(0.5f * 0.01308997f).epsilon(1e-4));
    // x = 1000 at 57295.78 ms: cos 154 + cos 195 + cos 93, a third.
    CHECK(drunkSway(57295.78, 1.0f) == Catch::Approx(0.01308997 * (std::cos(154.0) + std::cos(195.0) + std::cos(93.0)) / 3.0).margin(2e-5));
    float low = 1.0f, high = -1.0f;
    for (int ms = 0; ms < 20000; ms += 16) {
        low = std::min(low, drunkSway(ms, 1.0f));
        high = std::max(high, drunkSway(ms, 1.0f));
    }
    CHECK(low < -0.008f);
    CHECK(high > 0.008f);
}

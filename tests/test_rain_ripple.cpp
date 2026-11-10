// Rain's rings on the ray traced water: when they fall, and the ring.

#include "rendering/rain_ripple.hpp"

#include <catch_amalgamated.hpp>

using namespace wowee::rendering::rain_ripple;

TEST_CASE("rings fall only while it rains and the weather is drawn", "[rain_ripple]") {
    REQUIRE(strength(true, 0.6f, 2, false, true) == Catch::Approx(0.6f));
    REQUIRE(strength(false, 0.6f, 2, false, true) == 0.0f);  // snow or sand, or none
    REQUIRE(strength(true, 0.6f, 0, false, true) == 0.0f);   // weatherDensity 0
    REQUIRE(strength(true, 0.6f, 2, true, true) == 0.0f);    // the camera indoors
    REQUIRE(strength(true, 0.6f, 2, false, false) == 0.0f);  // the setting off
    REQUIRE(strength(true, 0.0f, 2, false, true) == 0.0f);
    REQUIRE(strength(true, 1.7f, 2, false, true) == 1.0f);
}

TEST_CASE("a ring runs out from its drop and dies", "[rain_ripple]") {
    // Where the crest is: the slope's largest swing sits near the radius.
    auto crestAt = [](float age) {
        float best = 0.0f, at = 0.0f;
        for (float d = 0.0f; d < 1.0f; d += 0.002f) {
            const float s = std::abs(ringSlope(d, age));
            if (s > best) { best = s; at = d; }
        }
        return at;
    };
    REQUIRE(crestAt(0.2f) < crestAt(0.5f));
    REQUIRE(crestAt(0.5f) < crestAt(0.9f));
    REQUIRE(crestAt(0.9f) < kMaxRadius + kRingWidth);
    // Nothing far from the crest, and nothing once the ring has lived.
    REQUIRE(std::abs(ringSlope(kMaxRadius * 0.5f + 0.5f, 0.5f)) < 1e-6f);
    REQUIRE(ringSlope(0.3f, 1.0f) == 0.0f);
    // It weakens as it grows: the steepest at a quarter of its life over the steepest at three quarters.
    float early = 0.0f, late = 0.0f;
    for (float d = 0.0f; d < 1.0f; d += 0.002f) {
        early = std::max(early, std::abs(ringSlope(d, 0.25f)));
        late = std::max(late, std::abs(ringSlope(d, 0.75f)));
    }
    REQUIRE(late < early);
    REQUIRE(early <= kRingSlope);
}

TEST_CASE("a ring stays inside the cells the shader looks at", "[rain_ripple]") {
    // The shader takes the 3x3 cells round a point. A drop two cells off lies
    // at least 1.15 cells away (it falls within 0.15..0.85 of its cell), so a
    // ring with its crest's tail (three half-widths) must end before that, in
    // the smaller layer's cells too, or it would show cut off.
    REQUIRE(kMaxRadius + 3.0f * kRingWidth <= 1.15f * kSecondLayer * kCell);
}

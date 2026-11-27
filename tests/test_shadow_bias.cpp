// U22: the sun shadow map's depth bias in yards (shadow_bias.hpp, the same
// sum as shadow_csm.glsli's csmSampleCascade).
#include <catch_amalgamated.hpp>

#include "rendering/shadow_bias.hpp"

#include <cmath>

using namespace wowee::rendering::shadow_bias;

TEST_CASE("the shadow bias follows the texel and the slope, not the depth range (U22)", "[shadow]") {
    // The units' tile: 48 yards over 2048 texels.
    const float unitTexel = 48.0f / 2048.0f;
    // Flat ground under a sun 15 degrees up (Durotar at 08:30) and at noon.
    const float low = std::sin(15.0f * 3.14159265f / 180.0f);
    const float noon = 1.0f;
    // Under the low sun the old bias was over a yard along the light with the
    // default 500-yard shadow distance (depth range 3249 yards): the gap
    // between the feet and the shadow.
    CHECK(legacyDepthBiasYards(3249.0f, low) > 1.0f);
    // Now a few texels: the shadow touches the feet.
    CHECK(depthBiasYards(unitTexel, low) < 0.2f);
    CHECK(depthBiasYards(unitTexel, noon) == Catch::Approx(unitTexel));
    // Still at least what one texel spans along the slope, so lit ground does
    // not shadow itself.
    const float tangent = std::sqrt(1.0f - low * low) / low;
    CHECK(depthBiasYards(unitTexel, low) >= unitTexel * tangent);
    // A grazing sun is held to the steepest slope.
    CHECK(depthBiasYards(unitTexel, 0.0f) == Catch::Approx(unitTexel * (1.0f + 1.5f * kMaxTangent)));
    // Coarser cascades take proportionally more.
    CHECK(depthBiasYards(0.25f, low) == Catch::Approx(depthBiasYards(unitTexel, low) * 0.25f / unitTexel));
}

// The normal map derived from a diffuse texture.
//
// This was eighty lines duplicated between the character renderer and the WMO
// renderer, differing in one value: the Sobel strength, 5 for skin and cloth
// and 2 for stone. Both copies were private members of a renderer class and
// needed a Vulkan context to reach, so neither had ever been tested.
//
// What is pinned here is the arithmetic, against values worked out by hand
// rather than recorded from the implementation: a flat texture has no
// gradient, a step has one pointing across it, the alpha is opaque, and the
// sampling wraps at the edges rather than clamping.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "rendering/normal_map.hpp"

using wowee::rendering::generateNormalMap;

namespace {

/// An RGBA8 image built from a luminance per pixel.
std::vector<uint8_t> grey(const std::vector<uint8_t>& lum) {
    std::vector<uint8_t> out(lum.size() * 4);
    for (size_t i = 0; i < lum.size(); ++i) {
        out[i * 4 + 0] = lum[i];
        out[i * 4 + 1] = lum[i];
        out[i * 4 + 2] = lum[i];
        out[i * 4 + 3] = 255;
    }
    return out;
}

}  // namespace

TEST_CASE("a flat texture has no gradient anywhere", "[normalmap]") {
    // Every normal points straight out: 0,0,1. That encodes as 127,127,255,
    // not 128: (0 * 0.5 + 0.5) * 255 is 127.5 and the cast to uint8_t
    // truncates. Worth pinning, because a rounding change here would shift
    // every flat surface in the game by one step.
    const std::vector<uint8_t> src = grey(std::vector<uint8_t>(16, 128));
    const auto out = generateNormalMap(src.data(), 4, 4, 5.0f);

    REQUIRE(out.size() == 4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        INFO("pixel " << i);
        CHECK(out[i * 4 + 0] == 127);   // nx = 0
        CHECK(out[i * 4 + 1] == 127);   // ny = 0
        CHECK(out[i * 4 + 2] == 255);   // nz = 1
    }
}

TEST_CASE("the alpha channel is opaque", "[normalmap]") {
    // Nothing reads a height from it: the alpha is 255 whatever the texture.
    std::vector<uint8_t> lum(16, 128);
    lum[5] = 0;
    lum[10] = 255;
    const std::vector<uint8_t> src = grey(lum);
    const auto out = generateNormalMap(src.data(), 4, 4, 2.0f);

    for (int i = 0; i < 16; ++i) {
        INFO("pixel " << i);
        CHECK(out[i * 4 + 3] == 255);
    }
}

TEST_CASE("a vertical edge tilts the normal across it", "[normalmap]") {
    // Left half black, right half white. The gradient runs in x, so ny stays
    // at its neutral 128 and nx moves off it.
    std::vector<uint8_t> lum(16, 0);
    for (int y = 0; y < 4; ++y) {
        lum[y * 4 + 2] = 255;
        lum[y * 4 + 3] = 255;
    }
    const std::vector<uint8_t> src = grey(lum);
    const auto out = generateNormalMap(src.data(), 4, 4, 5.0f);

    // Column 1 sits left of the step, so height rises to its right and the
    // normal leans the other way.
    const int at = 0 * 4 + 1;
    CHECK(out[at * 4 + 0] != 127);
    CHECK(out[at * 4 + 1] == 127);
}

TEST_CASE("strength scales the tilt without changing its direction", "[normalmap]") {
    // A gentle step, not black to white. A full-range edge normalises to very
    // nearly +-1 at either strength, so both encode to the same saturated byte
    // and the difference this is about cannot be seen.
    std::vector<uint8_t> lum(16, 100);
    for (int y = 0; y < 4; ++y) {
        lum[y * 4 + 2] = 112;
        lum[y * 4 + 3] = 112;
    }
    const std::vector<uint8_t> src = grey(lum);
    const auto weak = generateNormalMap(src.data(), 4, 4, 2.0f);
    const auto strong = generateNormalMap(src.data(), 4, 4, 5.0f);

    const int at = 0 * 4 + 1;
    const int weakX = static_cast<int>(weak[at * 4 + 0]) - 127;
    const int strongX = static_cast<int>(strong[at * 4 + 0]) - 127;
    CHECK(weakX != 0);
    // Same side of neutral, and further from it.
    CHECK((weakX < 0) == (strongX < 0));
    CHECK(std::abs(strongX) > std::abs(weakX));
    // A stronger tilt leaves less of the normal pointing outwards.
    CHECK(strong[at * 4 + 2] < weak[at * 4 + 2]);
}

TEST_CASE("sampling wraps rather than clamping at the edges", "[normalmap]") {
    // One bright column at x=0. A clamping sampler would see no step at the
    // right edge; a wrapping one sees the same step from the other side, so
    // the last column tilts too.
    std::vector<uint8_t> lum(16, 0);
    for (int y = 0; y < 4; ++y) lum[y * 4 + 0] = 255;
    const std::vector<uint8_t> src = grey(lum);
    const auto out = generateNormalMap(src.data(), 4, 4, 5.0f);

    const int lastColumn = 0 * 4 + 3;
    CHECK(out[lastColumn * 4 + 0] != 127);
}

TEST_CASE("nothing to read answers nothing", "[normalmap]") {
    CHECK(generateNormalMap(nullptr, 4, 4, 5.0f).empty());

    const std::vector<uint8_t> src = grey(std::vector<uint8_t>(16, 128));
    CHECK(generateNormalMap(src.data(), 0, 4, 5.0f).empty());
    CHECK(generateNormalMap(src.data(), 4, 0, 5.0f).empty());
}

// How a pixel finds its own surface among the half-resolution traced texels,
// and how a texel keeps its history (rt_upsample.hpp, the mirror of
// rt_upsample.glsli): both sides of an edge keep texels of their own, a
// grazing floor matches by its plane, a corner by its normal, a history is
// held loosely where few samples say anything, and a still camera finds a
// texel's own history.
#include <catch_amalgamated.hpp>

#include "rendering/rt_upsample.hpp"

#include <vector>

using namespace wowee::rendering::rt_upsample;

TEST_CASE("rt_upsample: the checkerboard takes the nearest and the farthest in turn", "[rt_upsample]") {
    CHECK_FALSE(takesFarthest({0, 0}));
    CHECK(takesFarthest({1, 0}));
    CHECK(takesFarthest({0, 1}));
    CHECK_FALSE(takesFarthest({1, 1}));
    CHECK_FALSE(takesFarthest({7, 3}));

    // Reversed depth: the object in front at 0.9, the wall behind at 0.3.
    const std::array<float, 4> depth{0.9f, 0.3f, 0.3f, 0.3f};
    const std::array<bool, 4> valid{true, true, true, true};
    CHECK(pickBlockSample(depth, valid, {0, 0}) == 0);
    CHECK(pickBlockSample(depth, valid, {1, 0}) != 0);
}

TEST_CASE("rt_upsample: a block's sample is never the sky while it has a surface", "[rt_upsample]") {
    const std::array<bool, 4> valid{true, true, true, true};
    CHECK(pickBlockSample({0.0f, 0.5f, 0.0f, 0.0f}, valid, {1, 0}) == 1);
    CHECK(pickBlockSample({0.0f, 0.5f, 0.0f, 0.0f}, valid, {0, 0}) == 1);
    CHECK(pickBlockSample({0.0f, 0.0f, 0.0f, 0.0f}, valid, {0, 0}) == -1);
}

TEST_CASE("rt_upsample: a surface of the tracing's comes first, near or far", "[rt_upsample]") {
    // Grass (no surface of the tracing's) in front of the ground.
    const std::array<float, 4> depth{0.9f, 0.9f, 0.2f, 0.9f};
    const std::array<bool, 4> valid{false, false, true, false};
    CHECK(pickBlockSample(depth, valid, {0, 0}) == 2);
    CHECK(pickBlockSample(depth, valid, {1, 0}) == 2);
    // None of the tracing's: any surface, by the same rule.
    const std::array<bool, 4> none{false, false, false, false};
    CHECK(pickBlockSample(depth, none, {0, 0}) == 0);
    CHECK(pickBlockSample(depth, none, {1, 0}) == 2);
}

namespace {

// A scene of distances (0 the sky) at full resolution, traced at half: how
// many surface pixels find no texel of their own (distance match above 0.6)
// within `rings` rings around their texel, with the nearest sample of every
// block (the old rule) or the checkerboard.
int pixelsWithoutTexel(const std::vector<std::vector<float>>& dist, bool checkerboard, int rings) {
    const int h = static_cast<int>(dist.size()), w = static_cast<int>(dist[0].size());
    const int th = h / 2, tw = w / 2;
    std::vector<std::vector<float>> texel(th, std::vector<float>(tw, 0.0f));
    const std::array<bool, 4> valid{true, true, true, true};
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            std::array<float, 4> depth{};
            for (int i = 0; i < 4; ++i) {
                const float d = dist[ty * 2 + (i >> 1)][tx * 2 + (i & 1)];
                depth[i] = d > 0.0f ? 1.0f / d : 0.0f;  // reversed depth
            }
            const int sub = pickBlockSample(depth, valid, checkerboard ? glm::ivec2(tx, ty) : glm::ivec2(0, 0));
            texel[ty][tx] = sub < 0 ? 0.0f : dist[ty * 2 + (sub >> 1)][tx * 2 + (sub & 1)];
        }
    }
    int missing = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (dist[y][x] <= 0.0f) continue;
            bool found = false;
            for (int oy = -rings; oy <= rings && !found; ++oy) {
                for (int ox = -rings; ox <= rings && !found; ++ox) {
                    const int ty = y / 2 + oy, tx = x / 2 + ox;
                    if (ty < 0 || tx < 0 || ty >= th || tx >= tw) continue;
                    found = distanceMatch(texel[ty][tx], dist[y][x]) > 0.6f;
                }
            }
            if (!found) ++missing;
        }
    }
    return missing;
}

}  // namespace

TEST_CASE("rt_upsample: both sides of an edge keep texels of their own", "[rt_upsample]") {
    // A wall at 30 yards; in front of it at 10 a post three pixels wide and
    // four pickets one pixel wide, a pixel apart - a fence, a tree's twigs.
    std::vector<std::vector<float>> dist(24, std::vector<float>(24, 30.0f));
    for (auto& row : dist) {
        for (int x : {3, 4, 5, 9, 11, 13, 15}) row[x] = 10.0f;
    }
    const int before = pixelsWithoutTexel(dist, false, 1);
    const int after = pixelsWithoutTexel(dist, true, 1);
    // The nearest-always rule leaves the wall between the pickets (x 10 and
    // 12) with no wall texel in the 3x3 around: lit as without the tracing,
    // and as the fence moves over the blocks, switching to and fro.
    CHECK(before >= 2 * 24);
    CHECK(after == 0);
    CHECK(pixelsWithoutTexel(dist, true, 2) == 0);
}

TEST_CASE("rt_upsample: a grazing floor matches by its plane", "[rt_upsample]") {
    // The camera 1.7 yards above a floor, looking along it: two points on it
    // two pixels apart at 50 yards differ in distance by far more than 3%.
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    const glm::vec3 pixel(0.0f, 50.0f, -1.7f);
    const glm::vec3 texel(0.0f, 54.0f, -1.7f);
    const float expected = glm::length(pixel), texelDist = glm::length(texel);
    CHECK(distanceMatch(texelDist, expected) < 0.1f);
    CHECK(planeMatch(pixel, texel, n, expected, texelDist) > 0.9f);
    // A post standing on it at the same distance is off the plane (the
    // tolerance at 50 yards is half a yard: the normals rebuilt from the
    // depth wander, and a few degrees over the yards between two texels on a
    // grazing floor are that much).
    const glm::vec3 post(0.0f, 50.0f, 0.3f);
    CHECK(planeMatch(post, texel, n, glm::length(post), texelDist) < 0.01f);
    // Not past a quarter of the distance along the plane.
    const glm::vec3 far(0.0f, 80.0f, -1.7f);
    CHECK(planeMatch(pixel, far, n, expected, glm::length(far)) == Catch::Approx(0.0f).margin(1e-6));
    // The sky is nothing.
    CHECK(planeMatch(pixel, texel, n, expected, 0.0f) == 0.0f);
    CHECK(distanceMatch(0.0f, expected) == 0.0f);
}

TEST_CASE("rt_upsample: across a corner each face weighs its own", "[rt_upsample]") {
    const glm::vec3 a(1.0f, 0.0f, 0.0f), b(0.0f, 1.0f, 0.0f);
    CHECK(normalWeight(a, a) == Catch::Approx(1.0f));
    CHECK(normalWeight(a, b) == Catch::Approx(0.05f));
    CHECK(normalWeight(a, -a) == Catch::Approx(0.05f));
    // A smoothed or bumped normal some 25 degrees off still counts fully.
    const glm::vec3 tilted = glm::normalize(glm::vec3(1.0f, 0.45f, 0.0f));
    CHECK(normalWeight(a, tilted) > 0.95f);
}

TEST_CASE("rt_upsample: a history is held loosely where few samples say anything", "[rt_upsample]") {
    // One sample: the spread is nothing, and a 0-or-1 sample of a penumbra
    // must not hold a settled 0.5 history.
    CHECK(clampWindow(0.0f, 1.0f) == Catch::Approx(0.8f));
    CHECK(clampWindow(0.0f, 1.0f) >= 0.5f);
    CHECK(clampWindow(0.0f, 2.0f) > clampWindow(0.0f, 3.0f));
    CHECK(clampWindow(0.0f, 3.0f) > clampWindow(0.0f, 4.0f));
    // Four or more: as before.
    CHECK(clampWindow(0.1f, 4.0f) == Catch::Approx(0.35f));
    CHECK(clampWindow(0.1f, 13.0f) == Catch::Approx(0.35f));
}

TEST_CASE("rt_upsample: a still camera finds a texel's own history", "[rt_upsample]") {
    const glm::vec2 own(10.5f, 20.5f);
    // Half resolution: its own texel whole, the neighbour traced a pixel to
    // the side a quarter, a texel away next to nothing.
    CHECK(historyWeight(own, own, 2.0f) == Catch::Approx(1.001f));
    CHECK(historyWeight(own + glm::vec2(1.0f, 0.0f), own, 2.0f) == Catch::Approx(0.251f));
    CHECK(historyWeight(own + glm::vec2(2.0f, 0.0f), own, 2.0f) == Catch::Approx(0.001f));
    CHECK(historyWeight(own + glm::vec2(4.0f, 2.0f), own, 2.0f) > 0.0f);
    // Full resolution: the neighbour a pixel away carries nothing but the
    // thousandth that keeps a history where nothing nearer does.
    CHECK(historyWeight(own + glm::vec2(1.0f, 0.0f), own, 1.0f) == Catch::Approx(0.001f));
    // Halfway between two texels, the two alike.
    const glm::vec2 mid = own + glm::vec2(1.0f, 0.0f);
    CHECK(historyWeight(own, mid, 2.0f) == Catch::Approx(historyWeight(own + glm::vec2(2.0f, 0.0f), mid, 2.0f)));
}

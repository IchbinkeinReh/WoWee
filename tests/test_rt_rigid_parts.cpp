// A skinned model cut into the rigid pieces the ray tracer moves by their
// bones (rt_rigid_parts.hpp). A triangle given to the wrong bone casts its
// shadow from where that bone puts it - an arm's shadow swinging with the
// leg - and a piece left out casts nothing at all.
#include <catch_amalgamated.hpp>

#include "rendering/rt_rigid_parts.hpp"

#include <algorithm>
#include <array>
#include <vector>

using namespace wowee::rendering::rt_parts;

namespace {

using Bones = std::array<uint8_t, 4>;

/// A strip of quads up a chain of bones: quad i's vertices are weighted to
/// `quadBone[i]` alone, except where `blend` puts a little of the next bone
/// on its top edge.
struct Strip {
    std::vector<Bones> indices, weights;
    std::vector<uint32_t> triangles;

    void quad(uint8_t bone, uint8_t topBone, uint8_t topWeight) {
        const uint32_t base = static_cast<uint32_t>(indices.size());
        for (int v = 0; v < 4; ++v) {
            const bool top = v >= 2;
            indices.push_back({bone, top ? topBone : bone, 0, 0});
            weights.push_back({uint8_t(top ? 255 - topWeight : 255), uint8_t(top ? topWeight : 0), 0, 0});
        }
        triangles.insert(triangles.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
};

std::vector<uint32_t> allTriangles(const std::vector<Part>& parts) {
    std::vector<uint32_t> all;
    for (const auto& p : parts) all.insert(all.end(), p.triangles.begin(), p.triangles.end());
    std::sort(all.begin(), all.end());
    return all;
}

}  // namespace

TEST_CASE("each triangle goes with the bone that carries most of it", "[rt][parts]") {
    Strip s;
    // Bones 0 <- 1 <- 2. Quad 0 on bone 0 with a little of bone 1 on top;
    // quad 1 on bone 1; quad 2 on bone 2.
    for (int i = 0; i < 10; ++i) s.quad(0, 1, 60);
    for (int i = 0; i < 10; ++i) s.quad(1, 1, 0);
    for (int i = 0; i < 10; ++i) s.quad(2, 2, 0);
    const std::vector<int16_t> parents = {-1, 0, 1};

    const auto parts = splitByBone(s.indices, s.weights, s.triangles, parents, 4);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].bone == 0);
    CHECK(parts[1].bone == 1);
    CHECK(parts[2].bone == 2);
    for (const auto& p : parts) CHECK(p.triangles.size() == 20);
    // Every triangle once.
    const auto all = allTriangles(parts);
    REQUIRE(all.size() == s.triangles.size() / 3);
    for (size_t i = 0; i < all.size(); ++i) CHECK(all[i] == i);
    // Quad 0's triangles stay on bone 0 despite bone 1's share of the top.
    CHECK(std::all_of(parts[0].triangles.begin(), parts[0].triangles.end(),
                      [](uint32_t t) { return t < 20; }));
}

TEST_CASE("a sliver of a piece joins its nearest ancestor big enough", "[rt][parts]") {
    Strip s;
    // A hand (bone 1) of many triangles, a finger (bone 2, child of the
    // hand) of two, and a fingertip (bone 3, child of the finger) of two.
    for (int i = 0; i < 20; ++i) s.quad(1, 1, 0);
    s.quad(2, 2, 0);
    s.quad(3, 3, 0);
    const std::vector<int16_t> parents = {-1, 0, 1, 2};

    const auto parts = splitByBone(s.indices, s.weights, s.triangles, parents, 8);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].bone == 1);
    CHECK(parts[0].triangles.size() == 44);
}

TEST_CASE("a small piece with nothing bigger above it stays a piece of its root", "[rt][parts]") {
    Strip s;
    s.quad(2, 2, 0);  // two triangles on bone 2, under a root with none
    const std::vector<int16_t> parents = {-1, 0, 1};
    const auto parts = splitByBone(s.indices, s.weights, s.triangles, parents, 8);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].bone == 0);
    CHECK(parts[0].triangles.size() == 2);
}

TEST_CASE("a model without bones is one piece", "[rt][parts]") {
    Strip s;
    s.quad(0, 0, 0);
    s.quad(0, 0, 0);
    const auto parts = splitByBone(s.indices, s.weights, s.triangles, {}, 8);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].triangles.size() == 4);
}

TEST_CASE("bone indices past the skeleton are not followed", "[rt][parts]") {
    Strip s;
    for (int i = 0; i < 8; ++i) s.quad(1, 1, 0);
    s.quad(7, 7, 0);  // no such bone: weight on nothing, so the root's
    const std::vector<int16_t> parents = {-1, 0};
    const auto parts = splitByBone(s.indices, s.weights, s.triangles, parents, 1);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].bone == 0);
    CHECK(parts[0].triangles.size() == 2);
    CHECK(parts[1].bone == 1);
    CHECK(allTriangles(parts).size() == 18);
}

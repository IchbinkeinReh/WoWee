// rendering::ray_mesh: where a pick ray first meets a skinned model's
// triangles, and which of several overlapping units that makes the one
// pointed at.

#include <catch_amalgamated.hpp>

#include "rendering/ray_mesh.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering::ray_mesh;

namespace {

// A unit's body as one upright square, a yard wide and two tall, facing +x,
// its four corners on bone 0.
std::vector<SkinnedVertex> body() {
    std::vector<SkinnedVertex> v(4);
    const glm::vec3 corners[4] = {{0, -0.5f, 0}, {0, 0.5f, 0}, {0, 0.5f, 2}, {0, -0.5f, 2}};
    for (int i = 0; i < 4; ++i) {
        v[i].position = corners[i];
        v[i].boneIndices[0] = 0;
        v[i].boneWeights[0] = 255;
    }
    return v;
}
const std::vector<uint16_t> kQuad = {0, 1, 2, 0, 2, 3};
const std::vector<std::pair<uint32_t, uint32_t>> kAll = {{0u, 6u}};

}  // namespace

TEST_CASE("a ray meets a triangle from either face, in front of its origin", "[ray_mesh]") {
    const glm::vec3 a(0, -1, -1), b(0, 1, -1), c(0, 0, 1);
    auto t = rayTriangle(glm::vec3(-5, 0, 0), glm::vec3(1, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f));
    t = rayTriangle(glm::vec3(5, 0, 0), glm::vec3(-1, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f));
    CHECK_FALSE(rayTriangle(glm::vec3(5, 0, 0), glm::vec3(1, 0, 0), a, b, c));   // behind
    CHECK_FALSE(rayTriangle(glm::vec3(-5, 3, 0), glm::vec3(1, 0, 0), a, b, c));  // beside
    // An unnormalised direction keeps its own parameter.
    t = rayTriangle(glm::vec3(-5, 0, 0), glm::vec3(2, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(2.5f));
}

TEST_CASE("a skinned model is hit where its bones hold it, by the world ray's distance", "[ray_mesh]") {
    const auto v = body();
    // Placed ten yards out along x and doubled in size.
    const glm::mat4 model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)), glm::vec3(2.0f));
    const std::vector<glm::mat4> rest(1, glm::mat4(1.0f));
    auto t = rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(10.0f));
    // Above its head at rest (four yards tall once doubled)...
    CHECK_FALSE(rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 4.5f), glm::vec3(1, 0, 0), 100.0f));
    // ...but the bone lifts it a yard, two in the world.
    const std::vector<glm::mat4> lifted(1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1)));
    t = rayHitSkinned(v, kQuad, kAll, lifted, model, glm::vec3(0, 0, 4.5f), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(10.0f));
    // Not past the limit, and not on a batch it does not draw.
    CHECK_FALSE(rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 9.0f));
    const std::vector<std::pair<uint32_t, uint32_t>> none;
    CHECK_FALSE(rayHitSkinned(v, kQuad, none, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 100.0f));
    // A vertex's weights split between two bones.
    auto split = v;
    for (auto& s : split) {
        s.boneIndices[1] = 1;
        s.boneWeights[0] = 128;
        s.boneWeights[1] = 127;
    }
    const std::vector<glm::mat4> two = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0))};
    t = rayHitSkinned(split, kQuad, kAll, two, glm::mat4(1.0f), glm::vec3(-5, 0, 1), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f + 2.0f * 127.0f / 255.0f).margin(1e-3f));
}

TEST_CASE("of two units in one another the one in front at the pointer is picked", "[ray_mesh]") {
    // Two of the same body on one spot, one turned a quarter so its square
    // faces +y: from the front the first is met, from the side the turned
    // one - each where its body shows, whatever their centres.
    const auto v = body();
    const std::vector<glm::mat4> rest(1, glm::mat4(1.0f));
    const glm::mat4 front(1.0f);
    const glm::mat4 turned = glm::translate(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1)),
                                            glm::vec3(0.3f, 0, 0));
    auto hits = [&](const glm::vec3& origin, const glm::vec3& dir) {
        return std::vector<std::optional<float>>{
            rayHitSkinned(v, kQuad, kAll, rest, front, origin, dir, 100.0f),
            rayHitSkinned(v, kQuad, kAll, rest, turned, origin, dir, 100.0f)};
    };
    // From in front (-x) through the first body's middle: the first.
    auto best = nearestPrecise(hits(glm::vec3(-5, 0, 1), glm::vec3(1, 0, 0)));
    REQUIRE(best);
    CHECK(*best == 0u);
    // From the side (-y): the first is edge-on there, the turned one shows.
    best = nearestPrecise(hits(glm::vec3(0, -5, 1), glm::vec3(0, 1, 0)));
    REQUIRE(best);
    CHECK(*best == 1u);
    // Where the ray meets neither, nothing: the spheres decide.
    CHECK_FALSE(nearestPrecise(hits(glm::vec3(-5, 3, 1), glm::vec3(1, 0, 0))));
    CHECK_FALSE(nearestPrecise({}));
}

// The client's blob shadow under a unit (0x007e49e0, 0x007e4480,
// 0x007e2d60, 0x007e3820, 0x0071ed80): which settings draw it, the box it
// is laid over, its texture coordinates, the height fade and the colour the
// ground is multiplied by.
#include <catch_amalgamated.hpp>

#include "rendering/blob_shadow.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace bs = wowee::rendering::blob_shadow;

namespace {
glm::vec2 uvAt(const bs::Projection& p, const glm::vec3& w) {
    const glm::vec4 h(w, 1.0f);
    return {glm::dot(p.uRow, h), glm::dot(p.vRow, h)};
}
}  // namespace

TEST_CASE("blob shadows are drawn with shadowLOD 1 and no dynamic shadows", "[blob_shadow]") {
    CHECK(bs::kDefaultShadowLOD == 1);
    CHECK(bs::drawn(1, 0));
    CHECK_FALSE(bs::drawn(0, 0));
    CHECK_FALSE(bs::drawn(1, 1));
    CHECK_FALSE(bs::drawn(1, 4));
}

TEST_CASE("a box closed on any axis draws nothing", "[blob_shadow]") {
    CHECK(bs::isEmpty({{0, 0, 0}, {1, 1, 0}}));
    CHECK(bs::isEmpty({{0, 0, 0}, {0, 1, 1}}));
    CHECK_FALSE(bs::isEmpty({{-1, -1, 0}, {1, 1, 2}}));
}

TEST_CASE("a mounted unit's box sits on the mount and takes it in", "[blob_shadow]") {
    const bs::Box rider{{-0.3f, -0.3f, 0.0f}, {0.3f, 0.3f, 2.0f}};
    const bs::Box mount{{-1.5f, -0.5f, 0.0f}, {1.5f, 0.5f, 2.2f}};
    const bs::Box b = bs::mountedBox(rider, mount, 1.8f);
    // Raised by 1.8 less half its 2.0 height.
    CHECK(b.max.z == Catch::Approx(2.8f));
    CHECK(b.min == mount.min);
    CHECK(b.max.x == Catch::Approx(1.5f));
}

TEST_CASE("the blob covers the box's footprint, turned with the model", "[blob_shadow]") {
    const bs::Box box{{-1.0f, -0.5f, 0.0f}, {1.0f, 0.5f, 2.0f}};
    glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 200.0f, 50.0f));
    world = glm::rotate(world, glm::radians(90.0f), glm::vec3(0, 0, 1));
    const auto p = bs::project(box, world);
    REQUIRE(p);
    // Turned a quarter, the 2 x 1 footprint spans 1 in x and 2 in y.
    CHECK(p->boxMin.x == Catch::Approx(99.5f));
    CHECK(p->boxMax.x == Catch::Approx(100.5f));
    CHECK(p->boxMin.y == Catch::Approx(199.0f));
    CHECK(p->boxMax.y == Catch::Approx(201.0f));
    // From 1.67 half-heights below the origin to one above.
    CHECK(p->boxMin.z == Catch::Approx(50.0f - 1.6666666f));
    CHECK(p->boxMax.z == Catch::Approx(51.0f));
    // Centre of the texture under the origin; u along the model's +Y, v
    // against its +X.
    const glm::vec2 c = uvAt(*p, {100.0f, 200.0f, 50.0f});
    CHECK(c.x == Catch::Approx(0.5f));
    CHECK(c.y == Catch::Approx(0.5f));
    const glm::vec3 modelX = glm::vec3(world * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 modelY = glm::vec3(world * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
    CHECK(uvAt(*p, modelX).y == Catch::Approx(0.0f).margin(1e-5));
    CHECK(uvAt(*p, modelY).x == Catch::Approx(1.0f));
    // The height coordinate: 0 at the box's floor, 1 at its top.
    CHECK(glm::dot(p->hRow, glm::vec4(0, 0, p->boxMin.z, 1)) == Catch::Approx(0.0f).margin(1e-5));
    CHECK(glm::dot(p->hRow, glm::vec4(0, 0, p->boxMax.z, 1)) == Catch::Approx(1.0f));
}

TEST_CASE("the blob's box is scaled and held to five yards", "[blob_shadow]") {
    const bs::Box box{{-4.0f, -4.0f, 0.0f}, {4.0f, 4.0f, 3.0f}};
    const glm::mat4 world = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    const auto p = bs::project(box, world);
    REQUIRE(p);
    CHECK(p->boxMax.x == Catch::Approx(5.0f));
    CHECK(p->boxMin.y == Catch::Approx(-5.0f));
    // 3 x 2 = 6 high, held to 5: a half-height of 2.5.
    CHECK(p->boxMax.z == Catch::Approx(2.5f));
    CHECK_FALSE(bs::project({{0, 0, 0}, {0, 1, 1}}, glm::mat4(1.0f)));
}

TEST_CASE("the height fade comes in over the lowest sixth and out over the top", "[blob_shadow]") {
    // ShadowMod is grey 1 - fade: white where the fade is gone.
    CHECK(bs::modTexel(0) == Catch::Approx(1.0f));
    CHECK(bs::modTexel(63) == Catch::Approx(1.0f));
    CHECK(bs::modTexel(32) == Catch::Approx(0.0f));
    CHECK(bs::modTexel(5) == Catch::Approx(1.0f - 60.0f / 126.0f).margin(1.0f / 255.0f));
    CHECK(bs::modAt(0.5f) == Catch::Approx(0.0f));
    CHECK(bs::modAt(-1.0f) == Catch::Approx(1.0f));
    CHECK(bs::modAt(2.0f) == Catch::Approx(1.0f));
    CHECK(bs::fadeTexel(32) == Catch::Approx(1.0f));
}

TEST_CASE("the ground is multiplied by the blob over white, by the model's alpha", "[blob_shadow]") {
    const glm::vec3 dark(0.2f);
    CHECK(bs::modulation(dark, 1.0f, 0.5f).x == Catch::Approx(0.2f));
    CHECK(bs::modulation(dark, 0.0f, 0.5f).x == Catch::Approx(1.0f));
    CHECK(bs::modulation(dark, 0.5f, 0.5f).x == Catch::Approx(0.6f));
    // Above the box, nothing.
    CHECK(bs::modulation(dark, 1.0f, 1.0f).x == Catch::Approx(1.0f));
}

TEST_CASE("only triangles facing up take the blob", "[blob_shadow]") {
    const glm::mat4 id(1.0f);
    const glm::vec3 a(0, 0, 0), b(1, 0, 0), c(0, 1, 0);
    CHECK(bs::facesUp(id, a, b, c));
    CHECK_FALSE(bs::facesUp(id, a, c, b));
    // Turned over, the same winding faces down.
    const glm::mat4 flip = glm::rotate(id, glm::radians(180.0f), glm::vec3(1, 0, 0));
    CHECK_FALSE(bs::facesUp(flip, a, b, c));
}

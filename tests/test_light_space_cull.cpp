// Whether a box can reach a shadow cascade's tile (rendering/light_space_cull.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/light_space_cull.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::boxInLightTile;

namespace {
// A cascade looking straight down on the origin, 20 yards either way, as
// Renderer::computeLightSpaceMatrix builds one (ortho, Vulkan y flip).
glm::mat4 downwardCascade(float halfExtent) {
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 300), glm::vec3(0), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 1.0f, 2000.0f);
    proj[1][1] *= -1.0f;
    return proj * view;
}
}  // namespace

TEST_CASE("a box under the tile is drawn into it", "[light_space_cull]") {
    const glm::mat4 ls = downwardCascade(20.0f);
    CHECK(boxInLightTile(ls, {-5, -5, 0}, {5, 5, 10}));
}

TEST_CASE("a box straddling the tile's edge is drawn into it", "[light_space_cull]") {
    const glm::mat4 ls = downwardCascade(20.0f);
    CHECK(boxInLightTile(ls, {15, -2, 0}, {40, 2, 10}));
    CHECK(boxInLightTile(ls, {-40, -2, 0}, {-19.5f, 2, 10}));
}

TEST_CASE("a box wholly beside the tile is not", "[light_space_cull]") {
    // 50 yards off a 20-yard tile: inside the old 180-yard distance floor,
    // and no texel of this cascade can show it.
    const glm::mat4 ls = downwardCascade(20.0f);
    CHECK_FALSE(boxInLightTile(ls, {50, -5, 0}, {60, 5, 10}));
    CHECK_FALSE(boxInLightTile(ls, {-5, -60, 0}, {5, -50, 10}));
}

TEST_CASE("height does not take a box off a downward tile", "[light_space_cull]") {
    // Depth is the rasteriser's business: a tower far above or below the
    // centre is still on the tile.
    const glm::mat4 ls = downwardCascade(20.0f);
    CHECK(boxInLightTile(ls, {-1, -1, -500}, {1, 1, -400}));
    CHECK(boxInLightTile(ls, {-1, -1, 250}, {1, 1, 290}));
}

TEST_CASE("a low sun reaches casters far up its beam", "[light_space_cull]") {
    // Sun from the +x side, low: a building 300 yards toward the sun lies on
    // the tile's line of sight and can shadow it, so it is kept, while one
    // the same distance to the side is not.
    const glm::vec3 sunTravel = glm::normalize(glm::vec3(-1.0f, 0.0f, -0.2f));
    const glm::vec3 center(0.0f);
    const glm::mat4 view = glm::lookAt(center - sunTravel * 900.0f, center, glm::vec3(0, 0, 1));
    glm::mat4 proj = glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 3000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 ls = proj * view;
    CHECK(boxInLightTile(ls, {295, -5, 55}, {305, 5, 65}));
    CHECK_FALSE(boxInLightTile(ls, {-5, 295, 0}, {5, 305, 10}));
}

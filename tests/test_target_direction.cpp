// TargetDirectionEnemy/Friend's choice (0x005252d0, 0x00518f40): units
// projected through the camera (0x004f6d20) and scored by where they are on
// screen and in the world from the target.

#include <catch_amalgamated.hpp>

#include "game/target_direction.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::game::target_direction;

namespace {

/// A camera at the origin looking along +x, z up, as WoWee builds one.
View camera() {
    View v;
    v.position = glm::vec3(0.0f);
    v.forward = glm::vec3(1.0f, 0.0f, 0.0f);
    v.aspect = 4.0f / 3.0f;
    v.nearPlane = 0.2f;
    v.farPlane = 1000.0f;
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), v.aspect, v.nearPlane, v.farPlane);
    proj[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAt(v.position, v.forward, glm::vec3(0.0f, 0.0f, 1.0f));
    v.viewProjection = proj * view;
    return v;
}

}  // namespace

TEST_CASE("A point ahead projects into the frame with y up", "[target_direction]") {
    const View v = camera();
    ScreenPoint centre, above, behind;
    REQUIRE(project(v, {10.0f, 0.0f, 0.0f}, centre));
    REQUIRE(project(v, {10.0f, 0.0f, 2.0f}, above));
    CHECK(above.y > centre.y);
    // x spans a/sqrt(a*a+1) = 0.8 and y 0.6: the middle is (0.4, 0.3).
    CHECK(centre.x == Catch::Approx(0.4f).margin(1e-4));
    CHECK(centre.y == Catch::Approx(0.3f).margin(1e-4));
    CHECK_FALSE(project(v, {-10.0f, 0.0f, 0.0f}, behind));
}

TEST_CASE("The unit on the asked side of the screen is chosen", "[target_direction]") {
    const View v = camera();
    const glm::vec3 origin(10.0f, 0.0f, 0.0f);
    // In WoW's world y grows to the left of a camera looking along +x.
    const std::vector<Candidate> units = {
        {1, {10.0f, 3.0f, 0.0f}},    // left of the target on screen
        {2, {10.0f, -3.0f, 0.0f}},   // right of it
        {3, {50.0f, 0.0f, 0.0f}},    // beyond the 41 yards
    };
    const float right = 3.14159265f / 2.0f;
    CHECK(choose(v, right, 0.0f, 9, origin, 8, glm::vec3(0.0f), units) == 2);
    CHECK(choose(v, -right, 0.0f, 9, origin, 8, glm::vec3(0.0f), units) == 1);
    // Straight up the screen passes over units below the target. (Ones level
    // with it are not behind the asked direction - only a negative dot is.)
    const std::vector<Candidate> below = {{1, {10.0f, 3.0f, -1.0f}}, {2, {10.0f, -3.0f, -1.0f}}};
    CHECK(choose(v, 0.0f, 0.0f, 9, origin, 8, glm::vec3(0.0f), below) == 0);
}

TEST_CASE("The target and the player are never chosen", "[target_direction]") {
    const View v = camera();
    const std::vector<Candidate> units = {{8, {10.0f, -3.0f, 0.0f}}, {9, {10.0f, -4.0f, 0.0f}}};
    CHECK(choose(v, 1.5707963f, 0.0f, 9, {10.0f, 0.0f, 0.0f}, 8, glm::vec3(0.0f), units) == 0);
}

// The questgiver's mark turned to the camera (rendering/quest_giver_markers.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/quest_giver_markers.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using wowee::rendering::markerFrameFacing;

namespace {
// The questgiver's name attachment: somewhere over its head, turned with it
// and at its scale.
glm::mat4 attachment(float facing, float scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 32.0f));
    m = glm::rotate(m, facing, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, glm::vec3(scale));
}
}  // namespace

TEST_CASE("the mark faces the camera whichever way the questgiver faces", "[quest_markers]") {
    const glm::vec3 camera(10.0f, 5.0f, 34.0f);  // due -Y of the mark, a little above
    for (float facing : {0.0f, 1.0f, 2.5f, -2.0f}) {
        const glm::mat4 f = markerFrameFacing(attachment(facing, 1.0f), camera);
        const glm::vec3 front = glm::normalize(glm::vec3(f[0]));
        CHECK(front.x == Catch::Approx(0.0f).margin(1e-5));
        CHECK(front.y == Catch::Approx(-1.0f).margin(1e-5));
        CHECK(front.z == Catch::Approx(0.0f).margin(1e-5));
        // Upright.
        CHECK(glm::normalize(glm::vec3(f[2])).z == Catch::Approx(1.0f));
    }
}

TEST_CASE("the mark keeps the attachment's place and size", "[quest_markers]") {
    const glm::mat4 f = markerFrameFacing(attachment(0.7f, 1.5f), glm::vec3(0.0f, 0.0f, 30.0f));
    CHECK(f[3].x == Catch::Approx(10.0f));
    CHECK(f[3].y == Catch::Approx(20.0f));
    CHECK(f[3].z == Catch::Approx(32.0f));
    CHECK(glm::length(glm::vec3(f[0])) == Catch::Approx(1.5f));
    CHECK(glm::length(glm::vec3(f[1])) == Catch::Approx(1.5f));
    CHECK(glm::length(glm::vec3(f[2])) == Catch::Approx(1.5f));
    // A rotation, not a mirror: the model's left stays on its left.
    CHECK(glm::determinant(glm::mat3(f)) > 0.0f);
}

TEST_CASE("a camera straight overhead leaves the mark upright", "[quest_markers]") {
    const glm::mat4 f = markerFrameFacing(attachment(0.0f, 1.0f), glm::vec3(10.0f, 20.0f, 50.0f));
    CHECK(glm::normalize(glm::vec3(f[2])).z == Catch::Approx(1.0f));
    CHECK(std::isfinite(f[0].x));
}

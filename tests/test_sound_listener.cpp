#include <catch_amalgamated.hpp>

#include "audio/sound_listener.hpp"

using namespace wowee::audio::sound_listener;
using Catch::Approx;

// Where the world frame puts the sound listener each frame (0x004fa5f0).

TEST_CASE("At the character: two back along its facing, four up", "[sound_listener]") {
    const Anchor a{glm::vec3(100.0f, 200.0f, 10.0f), glm::vec3(1.0f, 0.0f, 0.0f)};
    const auto p = place(a, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(p.position.x == Approx(98.0f));
    CHECK(p.position.y == Approx(200.0f));
    CHECK(p.position.z == Approx(14.0f));
    CHECK(p.forward == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(p.up == glm::vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("Not a unit: the camera's way, pitch and all", "[sound_listener]") {
    const Anchor a{glm::vec3(0.0f, 0.0f, 0.0f), std::nullopt};
    const glm::vec3 camFwd = glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f));
    const auto p = place(a, glm::vec3(5.0f), camFwd);
    CHECK(p.forward == camFwd);
    CHECK(p.position.y == Approx(-2.0f * camFwd.y));
    CHECK(p.position.z == Approx(4.0f - 2.0f * camFwd.z));
}

TEST_CASE("No listener object: at the camera", "[sound_listener]") {
    const auto p = place(std::nullopt, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(p.position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(p.forward == glm::vec3(0.0f, 0.0f, -1.0f));
}

TEST_CASE("A facing is level: cosine and sine of the yaw", "[sound_listener]") {
    const glm::vec3 f = levelFacing(1.5707963f);
    CHECK(f.x == Approx(0.0f).margin(1e-6));
    CHECK(f.y == Approx(1.0f));
    CHECK(f.z == 0.0f);
}

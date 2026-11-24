// How a unit's portrait is framed and drawn (rendering/portrait_camera.hpp,
// PortraitButton.cpp: 0x00619580, 0x006bfe00).
#include <catch_amalgamated.hpp>

#include "rendering/figure_camera.hpp"
#include "rendering/portrait_camera.hpp"

#include <vector>

#include <cmath>

namespace pc = wowee::rendering::portrait_camera;

TEST_CASE("a camera's field of view is the diagonal one", "[portrait_camera]") {
    // Square, as a portrait is: the vertical is the diagonal over root two.
    CHECK(pc::verticalFov(0.785f, 1.0f) == Catch::Approx(0.785f / std::sqrt(2.0f)));
    // The human male's portrait camera, 0.785: a little under 32 degrees
    // top to bottom, which at its 0.71 yards from the face is the head.
    const float v = pc::verticalFov(0.785f, 1.0f);
    const float seen = 2.0f * 0.71f * std::tan(v * 0.5f);
    CHECK(seen < 0.45f);
    // Wider than tall narrows it further; never wider than the diagonal.
    CHECK(pc::verticalFov(1.0f, 2.0f) < pc::verticalFov(1.0f, 1.0f));
    CHECK(pc::verticalFov(1.0f, 0.01f) <= 1.0f);
}

TEST_CASE("a still portrait is the artist's framing, a live one has room", "[portrait_camera]") {
    CHECK(pc::kStillHeadroom == 1.0f);
    CHECK(pc::kAnimatedHeadroom > pc::kStillHeadroom);
    // Drawn on more than one frame, so the frames in flight all get it, and
    // on few enough that it is a picture rather than a slow animation.
    CHECK(pc::kStillFrames >= 2);
    CHECK(pc::kStillFrames <= 4);
}

namespace {
namespace fc = wowee::rendering::figure_camera;
struct Cam {
    int type;
    float fov;
    glm::vec3 positionBase;
    glm::vec3 targetBase;
};
}  // namespace

// 0x005974a0: the camera of type 1 as a PlayerModel's model comes.
TEST_CASE("a model frame takes the character sheet's camera", "[figure_camera]") {
    const std::vector<Cam> wolf{{0, 0.95f, {1.693f, 0.724f, 0.884f}, {1.12f, -0.159f, 0.877f}},
                                {1, 0.6762f, {5.556f, 0.0f, 1.806f}, {-0.139f, 0.0f, 0.833f}}};
    const auto cam = fc::asItComes(wolf, glm::vec3(9.0f));
    CHECK(cam.eye == glm::vec3(5.556f, 0.0f, 1.806f));
    CHECK(cam.target == glm::vec3(-0.139f, 0.0f, 0.833f));
    CHECK(cam.diagonalFov == 0.6762f);
    // None of that type: its own, at the middle of the box.
    const auto none = fc::asItComes(std::vector<Cam>{wolf[0]}, glm::vec3(0.1f, 0.2f, 1.5f));
    CHECK(none.eye.x == Catch::Approx(5.5556f).margin(0.001));
    CHECK(none.eye.z == Catch::Approx(2.4167f).margin(0.001));
    CHECK(none.target == glm::vec3(0.1f, 0.2f, 1.5f));
    CHECK(none.diagonalFov == 0.5f);
}

// 0x0095fba0: the model's matrix; the camera holds still.
TEST_CASE("a model moves, turns and grows under its camera", "[figure_camera]") {
    const float ddc = 0.49029f;   // 16:9
    const auto still = fc::pose({0, 0, 0}, 1.0f, 0.61f, {0, 0, 0}, 1.0f, ddc);
    CHECK(still.offset == glm::vec3(0.0f));
    CHECK(still.facing == 0.61f);
    CHECK(still.scale == 1.0f);
    // What it had as the model came is no change.
    const auto came = fc::pose({1, 2, 3}, 2.0f, 0.0f, {1, 2, 3}, 2.0f, ddc);
    CHECK(came.offset == glm::vec3(0.0f));
    CHECK(came.scale == 1.0f);
    const auto moved = fc::pose({-1, 0, 0.5f}, 1.5f, 0.0f, {0, 0, 0}, 1.0f, ddc);
    CHECK(moved.offset.x == Catch::Approx(-1.0 / (0.49029 * 5.0 / 3.0)).margin(0.001));
    CHECK(moved.offset.z == Catch::Approx(0.5 / (0.49029 * 5.0 / 3.0)).margin(0.001));
    CHECK(moved.scale == 1.5f);
    // In the model's own units: half as far where it is twice as large.
    const auto large = fc::pose({-1, 0, 0}, 2.0f, 0.0f, {0, 0, 0}, 2.0f, ddc);
    CHECK(large.offset.x == Catch::Approx(moved.offset.x / 2.0f).margin(0.001));
}

TEST_CASE("a model without a portrait camera is taken from its stand's bounds", "[figure_camera]") {
    const auto cam = fc::portraitWithoutOne({0.5f, 0.0f, 2.0f}, 3.0f);
    CHECK(cam.eye == glm::vec3(0.5f + 3.0f * 1.7f, 0.0f, 2.0f));
    CHECK(cam.target == glm::vec3(0.5f, 0.0f, 2.0f));
    CHECK(cam.diagonalFov == 1.57f);
}

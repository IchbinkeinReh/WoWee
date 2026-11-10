// A UI model frame's pose and screen (0x0095fba0, 0x0047bf90).
#include <catch_amalgamated.hpp>

#include "rendering/model_frame_view.hpp"

namespace mf = wowee::rendering::model_frame;

TEST_CASE("the screen's DDC height: a diagonal of 1 (0x0047bf90)", "[model_frame]") {
    CHECK(mf::ddcHeight(4.0f / 3.0f) == Catch::Approx(0.6f));
    CHECK(mf::ddcHeight(16.0f / 9.0f) == Catch::Approx(0.4903f).epsilon(0.001));
}

TEST_CASE("a model frame's pose (0x0095fba0)", "[model_frame]") {
    mf::Pose pose;
    pose.position = {0.1f, 0.2f, 0.0f};
    pose.effectiveScale = 2.0f;
    const glm::mat4 m = mf::modelMatrix(pose, 0.6f);
    CHECK(m[3].x == Catch::Approx(0.2f));
    CHECK(m[3].y == Catch::Approx(0.4f));
    // ModelScale 1, effective scale 2, at 4:3: twice the model's units.
    CHECK(m[0].x == Catch::Approx(2.0f).epsilon(0.001));
    // A quarter turn of facing takes the model's X to the frame's Y.
    pose.facing = 1.5707964f;
    const glm::vec4 x = mf::modelMatrix(pose, 0.6f) * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(x.x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(x.y == Catch::Approx(2.0f).epsilon(0.001));
}

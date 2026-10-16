// How a unit's portrait is framed and drawn (rendering/portrait_camera.hpp,
// PortraitButton.cpp: 0x00619580, 0x006bfe00).
#include <catch_amalgamated.hpp>

#include "rendering/portrait_camera.hpp"

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

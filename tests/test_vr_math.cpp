// The headset's arithmetic (vr/vr_math.hpp), held to numbers: none of it
// needs a headset, and all of it is where a mistake would be felt first - a
// world that swims, a pointer a hand's width off the button, a stick that
// walks the wrong way.

#include <catch_amalgamated.hpp>

#include "rendering/camera.hpp"
#include "rendering/depth_convention.hpp"
#include "vr/vr_math.hpp"

#include <glm/gtc/quaternion.hpp>
#include <cmath>

using namespace wowee;
using Catch::Approx;

namespace {

glm::vec3 clipToNdc(const glm::mat4& p, const glm::vec3& viewPoint) {
    const glm::vec4 c = p * glm::vec4(viewPoint, 1.0f);
    return glm::vec3(c) / c.w;
}

vr::Fov symmetricFov(float verticalDeg, float aspect) {
    const float halfV = glm::radians(verticalDeg * 0.5f);
    const float halfH = std::atan(aspect * std::tan(halfV));
    return vr::Fov{.angleLeft = -halfH, .angleRight = halfH, .angleUp = halfV, .angleDown = -halfV};
}

}  // namespace

TEST_CASE("A symmetric eye projects as the desktop camera does", "[vr]") {
    rendering::Camera camera;  // 54 degrees, 16:9, near 0.2, far 30000
    const vr::Fov fov = symmetricFov(camera.getFovDegrees(), camera.getAspectRatio());
    const glm::mat4 eye = vr::eyeProjection(fov, camera.getNearPlane(), camera.getFarPlane());
    const glm::mat4& desk = camera.getProjectionMatrix();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            INFO("column " << c << " row " << r);
            CHECK(eye[c][r] == Approx(desk[c][r]).margin(1e-5));
        }
    }
    const glm::mat4 forward = vr::eyeForwardProjection(fov, camera.getNearPlane(), camera.getFarPlane());
    const glm::mat4& deskForward = camera.getForwardDepthProjectionMatrix();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(forward[c][r] == Approx(deskForward[c][r]).margin(1e-5));
}

TEST_CASE("An eye's projection keeps the scene's reversed depth", "[vr]") {
    const vr::Fov fov{.angleLeft = -0.9f, .angleRight = 0.7f, .angleUp = 0.8f, .angleDown = -0.95f};
    const float nearZ = 0.2f, farZ = 30000.0f;
    const glm::mat4 p = vr::eyeProjection(fov, nearZ, farZ);
    // Straight ahead at the near plane: depth 1. At the far plane: depth 0.
    CHECK(clipToNdc(p, {0.0f, 0.0f, -nearZ}).z == Approx(rendering::depth::kNear).margin(1e-5));
    CHECK(clipToNdc(p, {0.0f, 0.0f, -farZ}).z == Approx(rendering::depth::kFar).margin(1e-4));
    // Nearer is greater, as the scene's depth test asks.
    CHECK(clipToNdc(p, {0.0f, 0.0f, -5.0f}).z > clipToNdc(p, {0.0f, 0.0f, -50.0f}).z);
    // And it is what depth::viewDistance reads back.
    const float d = clipToNdc(p, {0.0f, 0.0f, -37.0f}).z;
    CHECK(rendering::depth::viewDistance(d, nearZ, farZ) == Approx(37.0f).epsilon(1e-3));
}

TEST_CASE("An asymmetric eye puts its frustum's edges on the screen's", "[vr]") {
    // A left eye: wider toward the nose's far side, as a headset reports it.
    const vr::Fov fov{.angleLeft = -0.95f, .angleRight = 0.75f, .angleUp = 0.85f, .angleDown = -0.9f};
    const glm::mat4 p = vr::eyeProjection(fov, 0.2f, 1000.0f);
    const float z = -10.0f;
    // A point on each edge of the field of view, ten units out.
    const float xl = std::tan(fov.angleLeft) * 10.0f;
    const float xr = std::tan(fov.angleRight) * 10.0f;
    const float yu = std::tan(fov.angleUp) * 10.0f;
    const float yd = std::tan(fov.angleDown) * 10.0f;
    CHECK(clipToNdc(p, {xl, 0.0f, z}).x == Approx(-1.0f).margin(1e-5));
    CHECK(clipToNdc(p, {xr, 0.0f, z}).x == Approx(1.0f).margin(1e-5));
    // Vulkan's Y runs down: the top edge is -1.
    CHECK(clipToNdc(p, {0.0f, yu, z}).y == Approx(-1.0f).margin(1e-5));
    CHECK(clipToNdc(p, {0.0f, yd, z}).y == Approx(1.0f).margin(1e-5));
    // The view axis is off the screen's centre, toward the narrower side.
    CHECK(clipToNdc(p, {0.0f, 0.0f, z}).x > 0.0f);
}

TEST_CASE("A ray meets the interface's quad at the pixel it points at", "[vr]") {
    // A 2 by 1.125 metre quad two metres ahead, facing back at the viewer,
    // showing a 1920 by 1080 image.
    vr::Pose quad;
    quad.position = {0.0f, 1.5f, -2.0f};
    const glm::vec2 size(2.0f, 1.125f);
    const glm::ivec2 image(1920, 1080);

    SECTION("straight at the centre") {
        auto hit = vr::rayQuadHit({0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, quad, size, image);
        REQUIRE(hit);
        CHECK(hit->pixel.x == Approx(960.0f));
        CHECK(hit->pixel.y == Approx(540.0f));
        CHECK(hit->distance == Approx(2.0f));
    }
    SECTION("up and to the left is toward the image's top-left") {
        // Half a metre left and a quarter up on the quad.
        const glm::vec3 from(0.0f, 1.5f, 0.0f);
        const glm::vec3 to(-0.5f, 1.75f, -2.0f);
        auto hit = vr::rayQuadHit(from, glm::normalize(to - from), quad, size, image);
        REQUIRE(hit);
        CHECK(hit->pixel.x == Approx((-0.5f / 2.0f + 0.5f) * 1920.0f).margin(0.01));
        CHECK(hit->pixel.y == Approx((0.5f - 0.25f / 1.125f) * 1080.0f).margin(0.01));
    }
    SECTION("a turned quad") {
        // Turned 90 degrees to the left: it now stands to the viewer's left.
        vr::Pose turned;
        turned.orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        turned.position = {-2.0f, 1.5f, 0.0f};
        auto hit = vr::rayQuadHit({0.0f, 1.5f, 0.0f}, {-1.0f, 0.0f, 0.0f}, turned, size, image);
        REQUIRE(hit);
        CHECK(hit->pixel.x == Approx(960.0f).margin(0.01));
        CHECK(hit->pixel.y == Approx(540.0f).margin(0.01));
    }
    SECTION("misses") {
        CHECK_FALSE(vr::rayQuadHit({0.0f, 1.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, quad, size, image));   // away
        CHECK_FALSE(vr::rayQuadHit({0.0f, 1.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, quad, size, image));   // along
        CHECK_FALSE(vr::rayQuadHit({3.0f, 1.5f, 0.0f}, {0.0f, 0.0f, -1.0f}, quad, size, image));  // beside
    }
}

TEST_CASE("OpenXR's axes turn into the world's with the body", "[vr]") {
    // Facing +X (yaw 0): ahead (-Z) is +X, right (+X) is -Y, up (+Y) is +Z.
    CHECK(glm::distance(vr::xrToWorld({0, 0, -1}, 0.0f), glm::vec3(1, 0, 0)) < 1e-6f);
    CHECK(glm::distance(vr::xrToWorld({1, 0, 0}, 0.0f), glm::vec3(0, -1, 0)) < 1e-6f);
    CHECK(glm::distance(vr::xrToWorld({0, 1, 0}, 0.0f), glm::vec3(0, 0, 1)) < 1e-6f);
    // Facing +Y (yaw 90): ahead is +Y, right is +X.
    CHECK(glm::distance(vr::xrToWorld({0, 0, -1}, 90.0f), glm::vec3(0, 1, 0)) < 1e-6f);
    CHECK(glm::distance(vr::xrToWorld({1, 0, 0}, 90.0f), glm::vec3(1, 0, 0)) < 1e-6f);
    // The right is the camera's own right for that yaw.
    rendering::Camera camera;
    camera.setRotation(37.0f, 0.0f);
    CHECK(glm::distance(vr::xrToWorld({1, 0, 0}, 37.0f), camera.getRight()) < 1e-5f);
    CHECK(glm::distance(vr::xrToWorld({0, 0, -1}, 37.0f), camera.getForward()) < 1e-5f);
    // And a yaw read back from a direction is the one it was made with.
    CHECK(vr::worldYawDeg(vr::xrToWorld({0, 0, -1}, -120.0f)) == Approx(-120.0f).margin(1e-4));
    CHECK(vr::wrapDegrees(540.0f) == Approx(180.0f));
    CHECK(vr::wrapDegrees(-190.0f) == Approx(170.0f));
}

TEST_CASE("The left stick holds the movement keys it points at", "[vr]") {
    const float dz = 0.2f;
    SECTION("rest and drift") {
        CHECK_FALSE(vr::stickToMoveKeys({0.0f, 0.0f}, dz).any());
        CHECK_FALSE(vr::stickToMoveKeys({0.1f, -0.12f}, dz).any());
    }
    SECTION("straight ahead, even pushed a little off") {
        const auto k = vr::stickToMoveKeys({0.3f, 0.95f}, dz);
        CHECK(k.forward);
        CHECK_FALSE(k.left);
        CHECK_FALSE(k.right);
        CHECK_FALSE(k.back);
    }
    SECTION("a diagonal holds two") {
        const auto k = vr::stickToMoveKeys({-0.7f, 0.7f}, dz);
        CHECK(k.forward);
        CHECK(k.left);
        CHECK_FALSE(k.right);
    }
    SECTION("back and sideways") {
        CHECK(vr::stickToMoveKeys({0.0f, -0.8f}, dz).back);
        const auto r = vr::stickToMoveKeys({0.9f, 0.05f}, dz);
        CHECK(r.right);
        CHECK_FALSE(r.forward);
    }
    SECTION("as a world direction, up the stick is along the view") {
        const glm::vec2 m = vr::stickToWorldMove({0.0f, 1.0f}, 90.0f, dz);
        CHECK(m.x == Approx(0.0f).margin(1e-6));
        CHECK(m.y == Approx(1.0f));
        const glm::vec2 r = vr::stickToWorldMove({1.0f, 0.0f}, 0.0f, dz);
        CHECK(r.x == Approx(0.0f).margin(1e-6));
        CHECK(r.y == Approx(-1.0f));  // facing +X, right is -Y
        // The deadzone's edge is a standstill, not a jump to speed.
        CHECK(glm::length(vr::stickToWorldMove({0.0f, 0.21f}, 0.0f, dz)) < 0.02f);
    }
}

TEST_CASE("A snap turn turns once per push", "[vr]") {
    vr::Turn turn;
    const float dt = 1.0f / 90.0f;
    CHECK(turn.update(0.2f, dt) == 0.0f);
    // Right is a turn to the right: negative in the world's sense.
    CHECK(turn.update(0.9f, dt) == Approx(-30.0f));
    // Held over: nothing more.
    for (int i = 0; i < 50; ++i) CHECK(turn.update(0.95f, dt) == 0.0f);
    // Eased back but not to rest: still nothing.
    CHECK(turn.update(0.5f, dt) == 0.0f);
    CHECK(turn.update(0.9f, dt) == 0.0f);
    // Back to rest, then left.
    CHECK(turn.update(0.1f, dt) == 0.0f);
    CHECK(turn.update(-0.8f, dt) == Approx(30.0f));
}

TEST_CASE("A smooth turn turns in proportion to the push", "[vr]") {
    vr::Turn::Settings s;
    s.smooth = true;
    s.rateDegPerSec = 90.0f;
    vr::Turn turn(s);
    CHECK(turn.update(0.1f, 0.5f) == 0.0f);  // inside the deadzone
    CHECK(turn.update(1.0f, 0.5f) == Approx(-45.0f));
    CHECK(turn.update(-0.5f, 1.0f) == Approx(45.0f));
}

TEST_CASE("The head-following panel lets the head look around it", "[vr]") {
    vr::LazyFollow follow(30.0f, 100.0f);
    follow.snapTo(0.0f);
    // Glancing within the slack moves nothing.
    CHECK(follow.update(25.0f, 0.1f) == Approx(0.0f));
    CHECK(follow.update(-28.0f, 0.1f) == Approx(0.0f));
    // Past it, the panel turns toward the head at its rate...
    CHECK(follow.update(60.0f, 0.1f) == Approx(10.0f));
    // ...and keeps coming until it is within half the slack of the head.
    float yaw = 0.0f;
    for (int i = 0; i < 100; ++i) yaw = follow.update(60.0f, 0.1f);
    CHECK(std::abs(vr::wrapDegrees(60.0f - yaw)) <= 15.0f + 1e-3f);
    // Across the seam at 180 it takes the short way round.
    follow.snapTo(170.0f);
    CHECK(follow.update(-150.0f, 0.1f) == Approx(180.0f));
}

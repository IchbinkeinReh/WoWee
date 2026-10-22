// Tests for rendering::Camera setter guards and degenerate-pose math.
#include <catch_amalgamated.hpp>
#include "rendering/camera.hpp"
#include <cmath>
#include <limits>

using namespace wowee::rendering;

TEST_CASE("Camera::setPosition rejects NaN/inf", "[camera]") {
    Camera cam;
    glm::vec3 originalPos = cam.getPosition();

    cam.setPosition(glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0));
    REQUIRE(cam.getPosition() == originalPos);

    cam.setPosition(glm::vec3(std::numeric_limits<float>::infinity(), 0, 0));
    REQUIRE(cam.getPosition() == originalPos);

    cam.setPosition(glm::vec3(10, 20, 30));
    REQUIRE(cam.getPosition() == glm::vec3(10, 20, 30));
}

TEST_CASE("Camera::setRotation rejects NaN", "[camera]") {
    Camera cam;
    cam.setRotation(45.0f, 30.0f);
    cam.setRotation(std::numeric_limits<float>::quiet_NaN(), 60.0f);
    // Yaw stays at 45, pitch stays at 30 (set was rejected wholesale)
    glm::vec3 fwdAfterBad = cam.getForward();
    cam.setRotation(45.0f, 30.0f);
    REQUIRE(cam.getForward() == fwdAfterBad);
}

TEST_CASE("Camera::setFov rejects NaN/zero/180+", "[camera]") {
    Camera cam;
    float originalFov = cam.getFovDegrees();

    cam.setFov(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(0.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(-30.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(180.0f);
    REQUIRE(cam.getFovDegrees() == originalFov);

    cam.setFov(90.0f);
    REQUIRE(cam.getFovDegrees() == 90.0f);
}

TEST_CASE("Camera::setAspectRatio rejects non-positive", "[camera]") {
    Camera cam;
    float originalAspect = cam.getAspectRatio();
    cam.setAspectRatio(0.0f);
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(-1.0f);
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(cam.getAspectRatio() == originalAspect);
    cam.setAspectRatio(2.0f);
    REQUIRE(cam.getAspectRatio() == 2.0f);
}

TEST_CASE("Camera::getRight/getUp return finite at +/-89 pitch", "[camera]") {
    Camera cam;
    cam.setRotation(0.0f, 89.0f);
    glm::vec3 r = cam.getRight();
    glm::vec3 u = cam.getUp();
    REQUIRE(std::isfinite(r.x));
    REQUIRE(std::isfinite(r.y));
    REQUIRE(std::isfinite(r.z));
    REQUIRE(std::isfinite(u.x));
    REQUIRE(std::isfinite(u.y));
    REQUIRE(std::isfinite(u.z));
}

TEST_CASE("Camera::getRight/getUp degrade safely at +/-90 pitch", "[camera]") {
    Camera cam;
    // Force the degenerate pose: forward exactly = world up
    cam.setRotation(0.0f, 90.0f);
    glm::vec3 r = cam.getRight();
    glm::vec3 u = cam.getUp();
    REQUIRE(std::isfinite(r.x));
    REQUIRE(std::isfinite(r.y));
    REQUIRE(std::isfinite(r.z));
    REQUIRE(std::isfinite(u.x));
    REQUIRE(std::isfinite(u.y));
    REQUIRE(std::isfinite(u.z));
    // Right should be the +X fallback (since cross would be zero).
    REQUIRE(r == glm::vec3(1.0f, 0.0f, 0.0f));
}

// A headset's eye hands the camera a whole projection; the scene's reversed
// one is still made from it, and clearing it puts the field of view back.
TEST_CASE("Camera projection override is reversed like its own and clears", "[camera]") {
    wowee::rendering::Camera camera;
    const glm::mat4 own = camera.getProjectionMatrix();
    glm::mat4 forward = camera.getForwardDepthProjectionMatrix();
    forward[2][0] = 0.25f;  // an off-centre frustum
    camera.setProjectionOverride(forward);
    CHECK(camera.getForwardDepthProjectionMatrix()[2][0] == Catch::Approx(0.25f));
    CHECK(camera.getProjectionMatrix()[2][0] == Catch::Approx(0.25f));
    // Survives a field of view change...
    camera.setFov(70.0f);
    CHECK(camera.getProjectionMatrix()[2][0] == Catch::Approx(0.25f));
    // ...and is gone when cleared, the field of view then being 70.
    camera.setFov(54.0f);
    camera.clearProjectionOverride();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(camera.getProjectionMatrix()[c][r] == Catch::Approx(own[c][r]));
}

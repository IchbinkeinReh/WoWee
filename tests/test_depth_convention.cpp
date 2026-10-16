// The scene's reversed depth and the camera's near-plane sweep: the two halves
// of keeping the camera from seeing into a building through its wall at the
// client's 0.2 near clip without distant surfaces fighting.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/camera.hpp"
#include "rendering/camera_near_sweep.hpp"
#include "rendering/depth_convention.hpp"

namespace {

using namespace wowee::rendering;

float depthAt(const glm::mat4& proj, float viewDistance) {
    const glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, -viewDistance, 1.0f);
    return clip.z / clip.w;
}

// The next float after `d` in the direction away from the near plane, and the
// view distance it stands for: the smallest step in distance a 32-bit depth
// buffer can tell apart there.
float resolvableStep(const glm::mat4& proj, float viewDistance, bool reversed) {
    const float d = depthAt(proj, viewDistance);
    const float next = std::nextafter(d, reversed ? 0.0f : 1.0f);
    // Invert by bisection on the projection itself.
    float lo = viewDistance, hi = viewDistance * 2.0f;
    for (int i = 0; i < 200; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float dm = depthAt(proj, mid);
        const bool past = reversed ? (dm <= next) : (dm >= next);
        if (past) hi = mid; else lo = mid;
    }
    return hi - viewDistance;
}

}  // namespace

TEST_CASE("the camera's projection puts the near plane at 1 and the far plane at 0", "[depth]") {
    Camera camera;
    const glm::mat4& proj = camera.getProjectionMatrix();
    CHECK(camera.getNearPlane() == Catch::Approx(0.2f));
    CHECK(depthAt(proj, camera.getNearPlane()) == Catch::Approx(depth::kNear).margin(1e-5));
    CHECK(depthAt(proj, camera.getFarPlane()) == Catch::Approx(depth::kFar).margin(1e-5));
    // Nearer is larger, which is what the scene's depth tests ask.
    CHECK(depthAt(proj, 10.0f) > depthAt(proj, 11.0f));
    CHECK(depth::kNearer == VK_COMPARE_OP_GREATER);
    CHECK(depth::kNearerOrEqual == VK_COMPARE_OP_GREATER_OR_EQUAL);
}

TEST_CASE("reversing a projection leaves the screen position and the view distance alone", "[depth]") {
    const glm::mat4 forward = glm::perspective(glm::radians(54.0f), 16.0f / 9.0f, 0.2f, 30000.0f);
    const glm::mat4 rev = depth::reversed(forward);
    const glm::vec4 p(3.0f, -2.0f, -57.0f, 1.0f);
    const glm::vec4 a = forward * p;
    const glm::vec4 b = rev * p;
    CHECK(b.x == Catch::Approx(a.x));
    CHECK(b.y == Catch::Approx(a.y));
    CHECK(b.w == Catch::Approx(a.w));
    CHECK(b.z / b.w == Catch::Approx(1.0f - a.z / a.w).margin(1e-6));
    // And the distance comes back out of the depth.
    CHECK(depth::viewDistance(b.z / b.w, 0.2f, 30000.0f) == Catch::Approx(57.0f).epsilon(1e-3));
}

TEST_CASE("at the client's near clip, reversed depth resolves distant surfaces finer than before", "[depth]") {
    // Before: standard depth, near 0.5. Now: reversed, near 0.2.
    glm::mat4 before = glm::perspective(glm::radians(54.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    Camera camera;
    const glm::mat4& now = camera.getUnjitteredProjectionMatrix();
    for (float distance : {50.0f, 300.0f, 1000.0f, 1583.0f}) {
        const float stepBefore = resolvableStep(before, distance, false);
        const float stepNow = resolvableStep(now, distance, true);
        INFO("distance " << distance << ": before " << stepBefore << ", now " << stepNow);
        CHECK(stepNow < stepBefore);
        // And within a thousandth of the distance everywhere.
        CHECK(stepNow < 1e-3f * distance);
    }
}

TEST_CASE("the camera sweep holds the near plane at 1.75 times its size", "[camera]") {
    const glm::vec2 h = camera_sweep::nearHalfExtents(0.2f, 54.0f, 16.0f / 9.0f);
    CHECK(h.y == Catch::Approx(0.2f * std::tan(glm::radians(27.0f))));
    CHECK(h.x == Catch::Approx(h.y * 16.0f / 9.0f));
    const auto offsets = camera_sweep::sweepOffsets(0.2f, 54.0f, 16.0f / 9.0f);
    float maxX = 0.0f, maxY = 0.0f;
    for (const auto& o : offsets) {
        maxX = std::max(maxX, std::abs(o.x));
        maxY = std::max(maxY, std::abs(o.y));
    }
    CHECK(maxX == Catch::Approx(h.x * 1.75f));
    CHECK(maxY == Catch::Approx(h.y * 1.75f));
}

TEST_CASE("the sweep draws a shortened camera a ninth of a yard further in", "[camera]") {
    // Nothing in the way: as far as wanted.
    CHECK(camera_sweep::sweptDistance(8.0f, 8.0f, 0.5f) == 8.0f);
    // A wall 3 yards out.
    CHECK(camera_sweep::sweptDistance(8.0f, 3.0f, 0.5f) == Catch::Approx(3.0f - 1.0f / 9.0f));
    // Never inside the minimum.
    CHECK(camera_sweep::sweptDistance(8.0f, 0.3f, 0.5f) == 0.5f);
}

// The positions the ray traced lighting takes out of the scene depth
// (rt_reconstruct.hpp): a normal made from neighbouring pixels of a surface a
// yard from the eye, with the camera where the world's coordinates are
// large - Goldshire is ten thousand yards from the origin.
#include <catch_amalgamated.hpp>

#include "rendering/rt_reconstruct.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace wowee::rendering::rt_reconstruct;

namespace {

const glm::dvec3 kEye(-9493.17, 59.43, 57.81);
const glm::dvec3 kForward = glm::normalize(glm::dvec3(0.3, 1.0, -0.15));

struct View {
    glm::dmat4 viewProj;
    glm::mat4 viewProjF;
};

View makeView() {
    const glm::dmat4 proj = glm::perspective(glm::radians(60.0), 1920.0 / 1009.0, 0.5, 30000.0);
    const glm::dmat4 view = glm::lookAt(kEye, kEye + kForward, glm::dvec3(0, 0, 1));
    return {proj * view, glm::mat4(proj * view)};
}

/// Where a world point lands: uv and depth, exactly, then the depth as the
/// depth buffer keeps it (a float).
void project(const View& v, const glm::dvec3& p, glm::vec2& uv, float& depth) {
    const glm::dvec4 c = v.viewProj * glm::dvec4(p, 1.0);
    uv = glm::vec2(glm::dvec2(c) / c.w * 0.5 + 0.5);
    depth = static_cast<float>(c.z / c.w);
}

/// The surface a yard ahead: a plane turned a little from the eye, its
/// normal, and the point on it at uv.
const glm::dvec3 kPlaneNormal = glm::normalize(-kForward + glm::dvec3(0.4, 0.0, 0.2));
const glm::dvec3 kPlanePoint = kEye + kForward * 1.2;

glm::dvec3 onPlane(const View& v, const glm::dvec2& uv) {
    const glm::dmat4 inv = glm::inverse(v.viewProj);
    glm::dvec4 a = inv * glm::dvec4(uv * 2.0 - 1.0, -1.0, 1.0);
    glm::dvec4 b = inv * glm::dvec4(uv * 2.0 - 1.0, 0.5, 1.0);
    const glm::dvec3 o = glm::dvec3(a) / a.w, d = glm::normalize(glm::dvec3(b) / b.w - o);
    const double t = glm::dot(kPlanePoint - o, kPlaneNormal) / glm::dot(d, kPlaneNormal);
    return o + d * t;
}

}  // namespace

TEST_CASE("camera-relative unprojection gives a close surface its normal", "[rt][reconstruct]") {
    const View v = makeView();
    const glm::mat4 invRel = cameraRelativeInverse(v.viewProjF, glm::vec3(kEye));
    const glm::mat4 invWorld = glm::inverse(v.viewProjF);
    const glm::dvec2 pixel(1.0 / 1920.0, 1.0 / 1009.0);

    double worstRel = 0.0, worstWorld = 0.0;
    for (int i = 0; i < 64; ++i) {
        const glm::dvec2 uv(0.3 + 0.006 * (i % 8), 0.3 + 0.006 * (i / 8));
        glm::vec3 rel[3], world[3];
        const glm::dvec2 offs[3] = {{0, 0}, {pixel.x, 0}, {0, pixel.y}};
        for (int k = 0; k < 3; ++k) {
            glm::vec2 puv;
            float depth;
            project(v, onPlane(v, uv + offs[k]), puv, depth);
            rel[k] = relativeFromDepth(invRel, puv, depth);
            const glm::vec4 w = invWorld * glm::vec4(puv * 2.0f - 1.0f, depth, 1.0f);
            world[k] = glm::vec3(w) / w.w;
        }
        auto angle = [](glm::vec3 n) {
            n = glm::normalize(n);
            const double c = std::abs(glm::dot(glm::dvec3(n), kPlaneNormal));
            return std::acos(std::min(c, 1.0)) * 57.29578;
        };
        worstRel = std::max(worstRel, angle(glm::cross(rel[1] - rel[0], rel[2] - rel[0])));
        const glm::vec3 nw = glm::cross(world[1] - world[0], world[2] - world[0]);
        worstWorld = std::max(worstWorld, glm::length(nw) > 0.0f ? angle(nw) : 90.0);
    }
    // A few degrees at most, from the depth buffer's own steps.
    CHECK(worstRel < 3.0);
    // What it replaced: the world-space unprojection is lost in the rounding.
    CHECK(worstWorld > 20.0);
}

TEST_CASE("camera-relative unprojection puts a point where it is", "[rt][reconstruct]") {
    const View v = makeView();
    const glm::mat4 invRel = cameraRelativeInverse(v.viewProjF, glm::vec3(kEye));
    for (double dist : {1.0, 10.0, 200.0}) {
        const glm::dvec3 p = kEye + kForward * dist + glm::dvec3(0.1, -0.2, 0.05) * dist * 0.1;
        glm::vec2 uv;
        float depth;
        project(v, p, uv, depth);
        const glm::vec3 rel = relativeFromDepth(invRel, uv, depth);
        // Within the float view-projection's own rounding of the camera's
        // place (a few thousandths of a yard) plus the depth's step.
        CHECK(glm::length(glm::dvec3(rel) - (p - kEye)) < 0.01 + dist * 2e-4);
    }
}

// The tracer places a surface at its distance along the view's direction
// through the pixel (rtGbufWorld). The direction was taken at a depth under
// half a yard from the eye, where it came out a tenth of a yard off at fifty
// - by a different amount for /cam 50.00 and 49.99, so every shadow edge
// moved while the picture did not (the Sunstrider Isle, looking down from
// fifty yards, the camera at these two places).
TEST_CASE("the tracer's view direction puts the ground where it is, wherever the camera stands",
          "[rt][reconstruct]") {
    // The client's projection: 70 degrees, near 0.2, far 30000, Y flipped,
    // depth reversed (depth_convention.hpp).
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), 1920.0f / 1032.0f, 0.2f, 30000.0f);
    proj[1][1] *= -1.0f;
    glm::mat4 rev = proj;
    for (int c = 0; c < 4; ++c) rev[c][2] = proj[c][3] - proj[c][2];
    const glm::vec3 forward(-0.017738f, 0.0340438f, -0.999263f);
    for (const glm::vec3 eye : {glm::vec3(10694.283203f, -6255.481934f, 52.6661f),
                                glm::vec3(10694.283203f, -6255.481445f, 52.6561f)}) {
        const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 viewProj = rev * view;
        const glm::mat4 invRel = cameraRelativeInverse(viewProj, eye);
        double worstNow = 0.0, worstBefore = 0.0;
        for (double ox : {0.0, 10.0, -15.0}) {
            for (double oy : {0.0, 12.0, -9.0}) {
                const glm::dvec3 ground(10694.28 + ox, -6255.48 + oy, 3.0);
                // Rasterised as the client draws it, with this float matrix.
                const glm::dvec4 c = glm::dmat4(viewProj) * glm::dvec4(ground, 1.0);
                const glm::vec2 uv(glm::dvec2(c) / c.w * 0.5 + 0.5);
                const float depth = static_cast<float>(c.z / c.w);
                // Its distance as rt_prepare stores it, then placed as
                // rtGbufWorld places it.
                const float dist = glm::length(relativeFromDepth(invRel, uv, depth));
                const glm::dvec3 now = glm::dvec3(eye) + glm::dvec3(viewDirection(invRel, uv)) * double(dist);
                const glm::dvec3 before = glm::dvec3(eye) + glm::dvec3(viewDirection(invRel, uv, 0.5f)) * double(dist);
                worstNow = std::max(worstNow, glm::length(now - ground));
                worstBefore = std::max(worstBefore, glm::length(before - ground));
            }
        }
        // Within the world's own float step (a thousandth of a yard) and the
        // camera's place in the float matrix.
        CHECK(worstNow < 0.005);
        // And the old depth's direction really was a tenth of a yard off.
        CHECK(worstBefore > 0.05);
    }
}

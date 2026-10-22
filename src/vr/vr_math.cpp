#include "vr/vr_math.hpp"

#include "rendering/depth_convention.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::vr {

glm::mat4 eyeForwardProjection(const Fov& fov, float nearZ, float farZ) {
    // Tangents of the four half-angles: the frustum's edges at unit distance.
    const float l = std::tan(fov.angleLeft);
    const float r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp);
    const float d = std::tan(fov.angleDown);

    // Right-handed view space looking down -Z, as glm::lookAt builds it.
    // x: the tangent x/-z from l..r onto -1..1.
    // y: the same from d..u, then negated - Vulkan's Y runs down, which is
    //    what the camera's [1][1] *= -1 does for its symmetric frustum. Here
    //    the off-centre term flips with it.
    // z: 0 at the near plane, 1 at the far one (GLM_FORCE_DEPTH_ZERO_TO_ONE's
    //    right-handed frustum), reversed afterwards by the caller or
    //    eyeProjection.
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / (r - l);
    m[2][0] = (r + l) / (r - l);
    m[1][1] = -2.0f / (u - d);
    m[2][1] = -(u + d) / (u - d);
    m[2][2] = farZ / (nearZ - farZ);
    m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
    m[2][3] = -1.0f;
    return m;
}

glm::mat4 eyeProjection(const Fov& fov, float nearZ, float farZ) {
    return rendering::depth::reversed(eyeForwardProjection(fov, nearZ, farZ));
}

std::optional<QuadHit> rayQuadHit(const glm::vec3& origin, const glm::vec3& direction, const Pose& quad,
                                  const glm::vec2& size, const glm::ivec2& imagePixels) {
    if (size.x <= 0.0f || size.y <= 0.0f || imagePixels.x <= 0 || imagePixels.y <= 0) return std::nullopt;
    const glm::quat toQuad = glm::conjugate(glm::normalize(quad.orientation));
    const glm::vec3 o = toQuad * (origin - quad.position);
    const glm::vec3 d = toQuad * direction;
    if (std::abs(d.z) < 1e-6f) return std::nullopt;  // along the plane
    const float t = -o.z / d.z;
    if (!(t > 0.0f)) return std::nullopt;  // behind the ray's start
    const glm::vec3 p = o + t * d;
    const glm::vec2 half = size * 0.5f;
    if (std::abs(p.x) > half.x || std::abs(p.y) > half.y) return std::nullopt;
    QuadHit hit;
    // The image's rows run down from its top edge; the quad's +Y is up.
    hit.pixel = glm::vec2((p.x / size.x + 0.5f) * static_cast<float>(imagePixels.x),
                          (0.5f - p.y / size.y) * static_cast<float>(imagePixels.y));
    hit.distance = t * glm::length(direction);
    return hit;
}

glm::vec3 xrToWorld(const glm::vec3& v, float bodyYawDeg) {
    const float b = glm::radians(bodyYawDeg);
    const glm::vec3 forward(std::cos(b), std::sin(b), 0.0f);
    // The camera's right: forward x up, as Camera::getRight works it out.
    const glm::vec3 right(std::sin(b), -std::cos(b), 0.0f);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    return v.x * right + v.y * up - v.z * forward;
}

float wrapDegrees(float degrees) {
    float a = std::fmod(degrees, 360.0f);
    if (a <= -180.0f) a += 360.0f;
    if (a > 180.0f) a -= 360.0f;
    return a;
}

float worldYawDeg(const glm::vec3& worldDirection) {
    if (std::abs(worldDirection.x) < 1e-9f && std::abs(worldDirection.y) < 1e-9f) return 0.0f;
    return wrapDegrees(glm::degrees(std::atan2(worldDirection.y, worldDirection.x)));
}

namespace {
// sin(22.5 degrees): the stick's circle cut into eight sectors, so a push
// within 22.5 degrees of straight ahead is forward alone and one between 22.5
// and 67.5 is a diagonal.
constexpr float kSectorEdge = 0.38268343f;
}  // namespace

MoveKeys stickToMoveKeys(const glm::vec2& stick, float deadzone) {
    MoveKeys keys;
    const float len = glm::length(stick);
    if (!(len > deadzone) || len <= 0.0f) return keys;
    const glm::vec2 n = stick / len;
    keys.forward = n.y > kSectorEdge;
    keys.back = n.y < -kSectorEdge;
    keys.right = n.x > kSectorEdge;
    keys.left = n.x < -kSectorEdge;
    return keys;
}

glm::vec2 stickToWorldMove(const glm::vec2& stick, float yawDeg, float deadzone) {
    const float len = glm::length(stick);
    if (!(len > deadzone) || len <= 0.0f) return glm::vec2(0.0f);
    const float magnitude = std::clamp((len - deadzone) / std::max(1.0f - deadzone, 1e-6f), 0.0f, 1.0f);
    const glm::vec2 n = stick / len;
    const float b = glm::radians(yawDeg);
    const glm::vec2 forward(std::cos(b), std::sin(b));
    const glm::vec2 right(std::sin(b), -std::cos(b));
    return (forward * n.y + right * n.x) * magnitude;
}

float Turn::update(float stickX, float deltaSeconds) {
    const float push = std::abs(stickX);
    if (settings_.smooth) {
        if (!(push > settings_.deadzone)) return 0.0f;
        return -stickX * settings_.rateDegPerSec * std::max(deltaSeconds, 0.0f);
    }
    if (armed_ && push >= settings_.press) {
        armed_ = false;
        return stickX > 0.0f ? -settings_.stepDeg : settings_.stepDeg;
    }
    if (!armed_ && push <= settings_.release) armed_ = true;
    return 0.0f;
}

float LazyFollow::update(float headYawDeg, float deltaSeconds) {
    const float diff = wrapDegrees(headYawDeg - yaw_);
    if (!following_ && std::abs(diff) > slackDeg_) following_ = true;
    if (following_) {
        const float step = rateDegPerSec_ * std::max(deltaSeconds, 0.0f);
        if (std::abs(diff) <= step) {
            yaw_ = wrapDegrees(headYawDeg);
        } else {
            yaw_ = wrapDegrees(yaw_ + (diff > 0.0f ? step : -step));
        }
        if (std::abs(wrapDegrees(headYawDeg - yaw_)) <= slackDeg_ * 0.5f) following_ = false;
    }
    return yaw_;
}

}  // namespace wowee::vr

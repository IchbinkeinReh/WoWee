#pragma once

// The headset's arithmetic, with nothing of OpenXR in it: the per-eye
// projection, where a controller's ray meets the interface's quad, what a stick
// means to the character, and when a turn snaps. Plain values in and out, so
// the tests can hold each of them to a number without a headset.
//
// Conventions:
//   - OpenXR's spaces are right-handed with +Y up and -Z ahead, in metres.
//   - The world is the client's: +Z up, a yaw in degrees turning from +X
//     toward +Y (counter-clockwise seen from above), lengths in yards.
//   - Clip space is Vulkan's (Y down, depth 0..1) with the scene's depth
//     reversed (rendering/depth_convention.hpp): near at 1, far at 0.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>

namespace wowee::vr {

/// Metres to the world's yards.
inline constexpr float kYardsPerMetre = 1.0f / 0.9144f;

/// An eye's field of view as OpenXR gives it (XrFovf): four angles in
/// radians from the view axis, left and down negative.
struct Fov {
    float angleLeft = 0.0f;
    float angleRight = 0.0f;
    float angleUp = 0.0f;
    float angleDown = 0.0f;
};

/// A pose as OpenXR gives it (XrPosef).
struct Pose {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
};

/// The projection for one eye in the camera's "forward" form: the same
/// convention Camera::updateProjectionMatrix builds before reversing the
/// depth - Vulkan's Y down, depth 0 at the near plane and 1 at the far one.
/// Asymmetric where the eye's field of view is.
[[nodiscard]] glm::mat4 eyeForwardProjection(const Fov& fov, float nearZ, float farZ);

/// The same with the scene's reversed depth: what the scene draws with.
[[nodiscard]] glm::mat4 eyeProjection(const Fov& fov, float nearZ, float farZ);

/// Where a ray meets a quad, as a pixel of the image shown on it.
struct QuadHit {
    glm::vec2 pixel{0.0f};  ///< from the image's top-left corner
    float distance = 0.0f;  ///< along the ray, in the ray's units
};

/// The quad lies in its pose's XY plane, centred on it, `size` across and up
/// - an OpenXR quad layer. Nothing when the ray points away from the plane,
/// runs along it, or meets it outside the quad.
[[nodiscard]] std::optional<QuadHit> rayQuadHit(const glm::vec3& origin, const glm::vec3& direction,
                                                const Pose& quad, const glm::vec2& size,
                                                const glm::ivec2& imagePixels);

/// A direction or offset in OpenXR's axes, in the world's, for a body facing
/// `bodyYawDeg`: OpenXR's -Z ahead becomes the body's forward, +X its right,
/// +Y the world's up. Lengths are kept (no metre-to-yard scaling).
[[nodiscard]] glm::vec3 xrToWorld(const glm::vec3& v, float bodyYawDeg);

/// The world yaw a direction points along, in degrees (-180, 180].
[[nodiscard]] float worldYawDeg(const glm::vec3& worldDirection);

/// An angle in degrees brought into (-180, 180].
[[nodiscard]] float wrapDegrees(float degrees);

/// The movement keys a stick holds: up is forward, as OpenXR reports a
/// thumbstick (+Y up). Diagonals hold two. Inside the deadzone, none.
struct MoveKeys {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    [[nodiscard]] bool any() const { return forward || back || left || right; }
};
[[nodiscard]] MoveKeys stickToMoveKeys(const glm::vec2& stick, float deadzone);

/// Where a stick sends the character, as a world direction of length 0 to 1,
/// for a view facing `yawDeg`: up on the stick is along the view.
[[nodiscard]] glm::vec2 stickToWorldMove(const glm::vec2& stick, float yawDeg, float deadzone);

/// The turn a right stick asks for. Snapping by default: one step when the
/// stick is pushed past `press`, and no other until it has come back inside
/// `release` - so holding it over turns once. Smooth, it turns at `rate`
/// degrees a second in proportion to the push.
///
/// Degrees in the world's sense: positive turns left (counter-clockwise),
/// so pushing right gives a negative turn.
class Turn {
public:
    struct Settings {
        bool smooth = false;
        float stepDeg = 30.0f;
        float press = 0.7f;
        float release = 0.35f;
        float rateDegPerSec = 120.0f;
        float deadzone = 0.2f;
    };
    Turn() = default;
    explicit Turn(const Settings& settings) : settings_(settings) {}
    /// The turn for this frame, from the stick's X (-1 left .. 1 right).
    float update(float stickX, float deltaSeconds);
    [[nodiscard]] const Settings& settings() const { return settings_; }

private:
    Settings settings_{};
    bool armed_ = true;
};

/// A head-following panel's yaw, lazily: it stays where it is while the head
/// looks within `slackDeg` of it, and beyond that turns toward the head at
/// `rateDegPerSec` until it is back inside half the slack - so glancing at a
/// corner of it does not drag it away.
class LazyFollow {
public:
    LazyFollow(float slackDeg = 30.0f, float rateDegPerSec = 150.0f)
        : slackDeg_(slackDeg), rateDegPerSec_(rateDegPerSec) {}
    float update(float headYawDeg, float deltaSeconds);
    void snapTo(float yawDeg) { yaw_ = wrapDegrees(yawDeg); following_ = false; }
    [[nodiscard]] float yaw() const { return yaw_; }

private:
    float slackDeg_;
    float rateDegPerSec_;
    float yaw_ = 0.0f;
    bool following_ = false;
};

}  // namespace wowee::vr

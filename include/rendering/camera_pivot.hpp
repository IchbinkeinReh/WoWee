#pragma once

// Where the third-person camera pivots on the unit it follows: the camera's
// target heights (+0x14c / +0x150 / +0x154), worked out by 0x00604e00 when
// it takes a unit to look at and kept between [0.8333, the unit's ceiling].
//
//  - A unit with a model: its Breath attachment (17, looked up through the
//    model's attachment lookup, 0x00827460) at its bind height, plus 3.5/36,
//    at the unit's scale (object scale x the display's model scale). Without
//    one, nine tenths of the model's collision box height (the M2 header's
//    +0xbc..+0xd0 box) at that scale.
//  - The ceiling (0x00718080): the unit's collision height (+0x854: the
//    display's CreatureModelData CollisionHeight at its scale) less 1/6; a
//    unit that is no player at most 73/36 less 1/6; 15 where that comes to
//    nothing.
//
// A shapeshift form's display is the unit's display, so Metamorphosis'
// demon (DemonForm.m2 at the display's 0.5) is looked at by its own Breath
// attachment and kept below its own collision height - not by the head of
// whatever model it was before.

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <optional>

namespace wowee::rendering::camera_pivot {

/// 3.5/36: what 0x00604e00 adds over the Breath attachment.
inline constexpr float kBreathLift = 0.097222224f;
/// The lowest pivot (0x00604e00's clamp).
inline constexpr float kMinPivot = 0.8333333f;
/// The default collision height (73/36) a unit that is no player is held to.
inline constexpr float kUnitCeilingHeight = 2.0277777f;
/// What the ceiling stays below the collision height by.
inline constexpr float kBelowCollisionTop = 0.16666667f;
/// The ceiling where the unit has no height to give one.
inline constexpr float kNoCeiling = 15.0f;
/// M2 attachment id 17, Breath.
inline constexpr unsigned kBreathAttachment = 17;

/// The pivot the model gives, above its feet, before the clamp.
[[nodiscard]] inline float modelPivot(std::optional<float> breathZ, float boxMinZ, float boxMaxZ, float scale) {
    if (breathZ) return (*breathZ + kBreathLift) * scale;
    return (boxMaxZ - boxMinZ) * scale * 0.9f;
}

/// 0x00718080: the highest pivot for a unit of that collision height.
[[nodiscard]] inline float pivotCeiling(float collisionHeight, bool isPlayer) {
    if (!isPlayer && collisionHeight > kUnitCeilingHeight) return kUnitCeilingHeight - kBelowCollisionTop;
    return collisionHeight - kBelowCollisionTop;
}

/// 0x00604e00's clamp: below the floor the floor, at or past the ceiling
/// the ceiling (a ceiling of nothing reads as 15).
[[nodiscard]] inline float clampPivot(float pivot, float ceiling) {
    if (ceiling <= 0.0f) ceiling = kNoCeiling;
    if (pivot < kMinPivot) return kMinPivot;
    if (pivot >= ceiling) return ceiling;
    return pivot;
}

// ---- A unit on a mount, swimming (0x00604e00) --------------------------
//
// The client keeps three heights: +0x14c for a zoom target under 1.8315
// (0x00604640), +0x150 past it, +0x154 while the unit swims (its movement
// flags 0x200000). Each is the model's pivot above, less how far the pose
// it holds there lowers the top of its animation's bounds against Stand's
// (0x0082ced0: the sequence's box, its top at +0x34, at the unit's scale):
//  - mounted, the rider's Mount pose (AnimationData 91) - both +0x14c and
//    +0x150 - and the mount's seat (its attachment 0, at the rider's scale
//    times the mount display's, +0x990) lifts all three;
//  - on a flight path, a mount with sequence 135 has its seat lowered by
//    that pose's drop (the mount's Stand top less its 135 top, its scale);
//  - swimming, the Swim pose (42) of a model that has one, +0x154 only.
// A stand state (sitting, kneeling) lowers +0x14c by its pose as well -
// not carried here: the camera is given no stand-state pose.
// Every height is then held between 0.8333 and the ceiling, which for a
// mounted player is its mounted collision height (0x006d7720) less 1/6.

/// AnimationData ids the heights are measured by.
inline constexpr unsigned kAnimStand = 0;
inline constexpr unsigned kAnimSwim = 42;
inline constexpr unsigned kAnimMount = 91;
inline constexpr unsigned kAnimTaxiSeat = 135;

/// How far a pose lowers its bounds' top below Stand's, at the scale. A
/// model without the pose (or without Stand) lowers nothing.
[[nodiscard]] inline float poseDrop(std::optional<float> standTopZ, std::optional<float> poseTopZ, float scale) {
    if (!standTopZ || !poseTopZ) return 0.0f;
    return (*standTopZ - *poseTopZ) * scale;
}

/// The mount's seat over the unit's feet: its attachment 0 at the mount's
/// size, less the flight-path pose's drop where it has one (both already
/// at the mount's size). No attachment 0, no lift.
[[nodiscard]] inline float seatLift(std::optional<float> seatZ, float mountScale, float taxiSeatDrop = 0.0f) {
    if (!seatZ) return 0.0f;
    return *seatZ * mountScale - taxiSeatDrop;
}

struct TargetHeights {
    float ground = 0.0f;  ///< +0x14c / +0x150
    float swim = 0.0f;    ///< +0x154
};

/// 0x00604e00's heights (+0x14c and +0x150 as one: no stand-state pose),
/// clamped.
[[nodiscard]] inline TargetHeights targetHeights(float modelPivotHeight, bool mounted, float mountPoseDrop,
                                                 float swimPoseDrop, float lift, float ceiling) {
    TargetHeights h;
    h.ground = modelPivotHeight - (mounted ? mountPoseDrop : 0.0f);
    h.swim = modelPivotHeight - swimPoseDrop;
    if (mounted) {
        h.ground += lift;
        h.swim += lift;
    }
    h.ground = clampPivot(h.ground, ceiling);
    h.swim = clampPivot(h.swim, ceiling);
    return h;
}

/// 0x00604640: which of them the camera eases to.
[[nodiscard]] inline float selectHeight(const TargetHeights& h, bool swimming) {
    return swimming ? h.swim : h.ground;
}

/// 0x006d7720: a player's collision height while it rides - half its own
/// (the model's CollisionHeight, unscaled) plus the mount model's
/// MountHeight at the mount display's scale, or its own where that is
/// more; at the rider's scale (`scale`, as the unmounted height has it).
[[nodiscard]] inline float mountedCollisionHeight(float modelCollisionHeight, float mountHeight,
                                                  float mountDisplayScale, float scale) {
    float h = modelCollisionHeight * 0.5f + mountDisplayScale * mountHeight;
    if (h < modelCollisionHeight) h = modelCollisionHeight;
    return h * scale;
}

// ---- Easing (0x00603230 -> 0x006010e0, 0x00603d30) ----------------------

/// cameraHeightSmoothSpeed's default: yards a second the height eases by.
inline constexpr float kHeightSmoothSpeed = 1.2f;
/// The seconds a collision's pull-in eases back out over (0x00606f90 sets
/// +0x214 / +0x1e4 to 2.0).
inline constexpr float kPullInEaseSeconds = 2.0f;
/// A ninth of a yard: what the collision keeps clear, and the pull-in's
/// threshold (0x00605d60, 0x00606f90).
inline constexpr float kNinth = 1.0f / 9.0f;
inline constexpr float kPullInOffset = 0.111112066f;

/// 0x008ca080: the client's ease, half a cosine from `a` to `b`.
[[nodiscard]] inline float cosineLerp(float a, float b, float t) {
    return (1.0f - std::cos(t * glm::pi<float>())) * 0.5f * (b - a) + a;
}

/// One of the camera's eased values - the height +0x128 (with +0x210 ..
/// +0x21c), the distance +0x118 (with +0x1e0 .. +0x1ec): where it is, where
/// it set off from, its target, the seconds since and how long it takes.
struct Ease {
    float value = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    float elapsed = 0.0f;
    float duration = 0.0f;
    bool running = false;
    bool placed = false;  ///< +0x98 bit 0x80: until set, a target is taken at once

    /// 0x006010e0: off to `target` over `seconds` from where it is; the
    /// first target is taken at once.
    void setTarget(float target, float seconds) {
        if (std::fabs(value - target) < 0.001f && placed) {
            to = target;
            return;
        }
        if (!placed) {
            value = from = to = target;
            running = false;
            placed = true;
            return;
        }
        from = value;
        to = target;
        elapsed = 0.0f;
        duration = seconds;
        running = true;
    }
    /// 0x00603230: to `target` at `speed` yards a second, unless already
    /// on its way there.
    void setTargetAtSpeed(float target, float speed) {
        if (placed && running && std::fabs(to - target) < 0.001f) return;
        setTarget(target, speed > 0.0f ? std::fabs(target - value) / speed : 0.0f);
    }
    /// 0x00606f90: a collision drew it in to `held`; from there it eases
    /// back to its target over two seconds.
    void pullIn(float held) {
        value = from = held;
        elapsed = 0.0f;
        duration = kPullInEaseSeconds;
        running = true;
    }
    /// 0x00603d30: a frame on.
    void step(float dt) {
        if (std::fabs(to - value) < 2.3841858e-07f) {
            value = to;
            running = false;
            return;
        }
        if (!running) return;
        elapsed += dt;
        const float t = duration > 0.0f ? elapsed / duration : 1.0f;
        if (t < 1.0f) {
            value = cosineLerp(from, to, t);
        } else {
            value = to;
            running = false;
        }
    }
};

// ---- The pivot's own collision (0x00605d60) ------------------------------

/// The height the pivot may stand at over the feet. The line straight up
/// from 0.8333 over the feet to the height wanted (the larger of the eased
/// height and its target) is cut where it first meets the world -
/// `hitShare` of the way along, none for nothing - and, cut, drawn a ninth
/// further in, no shorter than a ninth. Then held no lower than a unit's
/// floor (`unitFloor`: three quarters of its collision height, 0 for none)
/// or 0.8333, and no higher than the eased height `current`.
[[nodiscard]] inline float collidedHeight(float wanted, float current, std::optional<float> hitShare,
                                          float unitFloor) {
    float len = wanted - kMinPivot;
    if (len <= kNinth) len = kNinth;
    float cut = len;
    if (hitShare) cut = *hitShare * len;
    if (std::fabs(len - cut) >= 2.3841858e-07f) {
        cut -= kNinth;
        if (cut <= kNinth) cut = kNinth;
    }
    float h = cut + kMinPivot;
    if (h < unitFloor) h = unitFloor;
    if (h < kMinPivot) h = kMinPivot;
    if (current < h) h = current;
    return h;
}

/// A unit's floor for that: three quarters of its collision height.
[[nodiscard]] inline float unitHeightFloor(float collisionHeight) { return collisionHeight * 0.75f; }

/// 0x00606f90: whether the frame's collision draws an eased value in - it
/// stands more than a ninth past what the collision allows - and to where:
/// just past it.
[[nodiscard]] inline std::optional<float> pulledIn(float eased, float collided) {
    if (eased - collided > kNinth) return collided + kPullInOffset;
    return std::nullopt;
}

}  // namespace wowee::rendering::camera_pivot

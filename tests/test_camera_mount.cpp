// The camera on a mount, in water and against what is over it, as the
// client has it: the mounted and swimming heights of 0x00604e00 chosen by
// 0x00604640, the mounted collision height of 0x006d7720, the height's and
// the distance's eases (0x00603230 -> 0x006010e0, 0x00603d30), the pivot's
// own line up over the feet and the pull-in that follows a hit (0x00605d60,
// 0x00606f90), and the model's fade by the camera's closeness along half a
// cosine (0x00606f90 -> 0x008ca080).
#include <catch_amalgamated.hpp>

#include "game/vehicle_camera.hpp"
#include "rendering/camera_pivot.hpp"

using Catch::Matchers::WithinAbs;
namespace cp = wowee::rendering::camera_pivot;

namespace {
// BloodElfMale.m2: Breath 1.8726, Stand's box top 2.2295, Mount's (91)
// 1.2503, Swim's (42) 1.8666; CreatureModelData CollisionHeight 2.031.
constexpr float kElfBreath = 1.8726436f;
constexpr float kElfStandTop = 2.2294583f;
constexpr float kElfMountTop = 1.2502623f;
constexpr float kElfSwimTop = 1.8666444f;
constexpr float kElfCollision = 2.0309999f;
// RidingHorse.m2: attachment 0 at 1.8657, MountHeight 1.8657.
constexpr float kHorseSeat = 1.8657202f;
constexpr float kHorseMountHeight = 1.8657200f;
// RidingWyvern.m2: attachment 0 at 1.1939, Stand top 2.0198, sequence 135
// top 4.2964, MountHeight 1.1939.
constexpr float kWyvernSeat = 1.1938910f;
constexpr float kWyvernStandTop = 2.0198479f;
constexpr float kWyvernTaxiTop = 4.2963505f;
}  // namespace

TEST_CASE("a pose lowers the pivot by its bounds' top under Stand's", "[camera]") {
    // The elf's Mount pose sits a yard lower than it stands.
    CHECK_THAT(cp::poseDrop(kElfStandTop, kElfMountTop, 1.0f), WithinAbs(0.979196f, 1e-5));
    CHECK_THAT(cp::poseDrop(kElfStandTop, kElfSwimTop, 1.0f), WithinAbs(0.362814f, 1e-5));
    // At the unit's scale.
    CHECK_THAT(cp::poseDrop(kElfStandTop, kElfMountTop, 0.5f), WithinAbs(0.489598f, 1e-5));
    // A model without the pose lowers nothing.
    CHECK(cp::poseDrop(kElfStandTop, std::nullopt, 1.0f) == 0.0f);
    CHECK(cp::poseDrop(std::nullopt, kElfMountTop, 1.0f) == 0.0f);
}

TEST_CASE("the seat is the mount's attachment 0 at the mount's size", "[camera]") {
    CHECK_THAT(cp::seatLift(kHorseSeat, 1.0f), WithinAbs(1.865720f, 1e-5));
    CHECK_THAT(cp::seatLift(kHorseSeat, 1.2f), WithinAbs(2.238864f, 1e-5));
    // On a flight path a mount with sequence 135 is measured in that pose:
    // the wyvern's is taller than it stands, which lifts the seat.
    const float taxiDrop = cp::poseDrop(kWyvernStandTop, kWyvernTaxiTop, 1.0f);
    CHECK_THAT(cp::seatLift(kWyvernSeat, 1.0f, taxiDrop), WithinAbs(3.470394f, 1e-5));
    // No attachment 0, no lift.
    CHECK(cp::seatLift(std::nullopt, 1.0f) == 0.0f);
}

TEST_CASE("a mounted player's collision height is half its own and the mount's", "[camera]") {
    // Elf on the horse: 2.031 / 2 + 1.8657 = 2.8812.
    CHECK_THAT(cp::mountedCollisionHeight(kElfCollision, kHorseMountHeight, 1.0f, 1.0f), WithinAbs(2.881220f, 1e-5));
    // The mount display's scale scales the MountHeight; the rider's the whole.
    CHECK_THAT(cp::mountedCollisionHeight(kElfCollision, kHorseMountHeight, 1.5f, 1.0f), WithinAbs(3.814080f, 1e-5));
    CHECK_THAT(cp::mountedCollisionHeight(kElfCollision, kHorseMountHeight, 1.0f, 2.0f), WithinAbs(5.762440f, 1e-5));
    // A mount lower than half the rider: the rider's own.
    CHECK_THAT(cp::mountedCollisionHeight(kElfCollision, 0.5f, 1.0f, 1.0f), WithinAbs(kElfCollision, 1e-6));
}

TEST_CASE("mounted, the pivot is the Breath less the Mount pose plus the seat", "[camera]") {
    const float breath = cp::modelPivot(kElfBreath, 0.0f, 2.0f, 1.0f);  // 1.969866
    const float mountDrop = cp::poseDrop(kElfStandTop, kElfMountTop, 1.0f);
    const float swimDrop = cp::poseDrop(kElfStandTop, kElfSwimTop, 1.0f);
    // On foot: the Breath under the elf's ceiling (2.031 - 1/6).
    const auto foot = cp::targetHeights(breath, false, mountDrop, swimDrop, cp::seatLift(kHorseSeat, 1.0f),
                                        cp::pivotCeiling(kElfCollision, true));
    CHECK_THAT(foot.ground, WithinAbs(1.864333f, 1e-5));
    CHECK_THAT(foot.swim, WithinAbs(1.607052f, 1e-5));
    // On the horse: 1.9699 - 0.9792 + 1.8657 = 2.8564, under the mounted
    // ceiling 2.8812 - 1/6 = 2.7146. It was the foot pivot plus the seat:
    // 3.730.
    const float mountedCeiling =
        cp::pivotCeiling(cp::mountedCollisionHeight(kElfCollision, kHorseMountHeight, 1.0f, 1.0f), true);
    const auto horse = cp::targetHeights(breath, true, mountDrop, swimDrop, cp::seatLift(kHorseSeat, 1.0f),
                                         mountedCeiling);
    CHECK_THAT(horse.ground, WithinAbs(2.714553f, 1e-5));
    // Swimming on it: the Swim pose, not the Mount pose, and the seat.
    CHECK_THAT(horse.swim, WithinAbs(2.714553f, 1e-5));
    // On the wyvern: 1.9699 - 0.9792 + 1.1939 = 2.1846, its ceiling
    // 1.0155 + 1.1939 - 1/6 = 2.0428.
    const auto wyvern = cp::targetHeights(
        breath, true, mountDrop, swimDrop, cp::seatLift(kWyvernSeat, 1.0f),
        cp::pivotCeiling(cp::mountedCollisionHeight(kElfCollision, kWyvernSeat, 1.0f, 1.0f), true));
    CHECK_THAT(wyvern.ground, WithinAbs(2.042723f, 1e-5));
    // Unclamped where the ceiling allows it.
    const auto tall = cp::targetHeights(breath, true, mountDrop, swimDrop, cp::seatLift(kWyvernSeat, 1.0f), 15.0f);
    CHECK_THAT(tall.ground, WithinAbs(2.184561f, 1e-5));
    // 0x00604640: swimming takes the swim height.
    CHECK(cp::selectHeight(foot, true) == foot.swim);
    CHECK(cp::selectHeight(foot, false) == foot.ground);
}

TEST_CASE("the height eases to a new target at 1.2 yards a second along half a cosine", "[camera]") {
    cp::Ease e;
    // The first target is taken at once (+0x98 bit 0x80 not yet set).
    e.setTargetAtSpeed(1.864f, cp::kHeightSmoothSpeed);
    CHECK(e.value == 1.864f);
    CHECK_FALSE(e.running);
    // Mounting: 0.85 yards over 0.85 / 1.2 s.
    e.setTargetAtSpeed(2.714f, cp::kHeightSmoothSpeed);
    REQUIRE(e.running);
    CHECK_THAT(e.duration, WithinAbs(0.708333f, 1e-5));
    e.step(e.duration * 0.5f);
    CHECK_THAT(e.value, WithinAbs((1.864f + 2.714f) * 0.5f, 1e-4));
    e.step(e.duration * 0.25f);
    CHECK_THAT(e.value, WithinAbs(cp::cosineLerp(1.864f, 2.714f, 0.75f), 1e-4));
    // The same target again does not start over.
    const float before = e.elapsed;
    e.setTargetAtSpeed(2.714f, cp::kHeightSmoothSpeed);
    CHECK(e.elapsed == before);
    e.step(1.0f);
    CHECK(e.value == 2.714f);
    CHECK_FALSE(e.running);
}

TEST_CASE("the pivot is drawn down by what is over the unit", "[camera]") {
    const float floor = cp::unitHeightFloor(kElfCollision);  // 1.5232
    // Nothing over it: the height wanted, no more than the eased one.
    CHECK_THAT(cp::collidedHeight(1.864f, 1.864f, std::nullopt, floor), WithinAbs(1.864f, 1e-6));
    // A ceiling 2.0 over the feet against a mounted 2.714: the line up from
    // 0.8333 is 1.8813 long and meets it at 1.1667 (a share of 0.6202),
    // less a ninth: 1.0556 + 0.8333 = 1.8889.
    const float wanted = 2.714553f;
    const float len = wanted - cp::kMinPivot;
    CHECK_THAT(cp::collidedHeight(wanted, wanted, (2.0f - cp::kMinPivot) / len, floor),
               WithinAbs(1.888889f, 1e-5));
    // A low beam: never under three quarters of the collision height...
    CHECK_THAT(cp::collidedHeight(wanted, wanted, 0.05f, floor), WithinAbs(1.523250f, 1e-5));
    // ... the mounted one on a mount ...
    CHECK_THAT(cp::collidedHeight(wanted, wanted, 0.05f, cp::unitHeightFloor(2.881220f)), WithinAbs(2.160915f, 1e-5));
    // ... nor 0.8333, and never over the eased height.
    CHECK_THAT(cp::collidedHeight(wanted, wanted, 0.0f, 0.0f), WithinAbs(0.944444f, 1e-5));
    CHECK_THAT(cp::collidedHeight(wanted, 1.2f, std::nullopt, 0.0f), WithinAbs(1.2f, 1e-6));
}

TEST_CASE("a collision draws the eased value in and it eases back over two seconds", "[camera]") {
    // Only more than a ninth short.
    CHECK_FALSE(cp::pulledIn(6.0f, 5.95f));
    REQUIRE(cp::pulledIn(6.0f, 3.0f));
    CHECK_THAT(*cp::pulledIn(6.0f, 3.0f), WithinAbs(3.111112f, 1e-6));
    // The distance (+0x118) from just past the wall back to the zoom.
    cp::Ease d;
    d.placed = true;
    d.value = d.to = 6.0f;
    d.pullIn(3.111112f);
    d.to = 6.0f;
    CHECK(d.duration == cp::kPullInEaseSeconds);
    d.step(0.5f);
    CHECK_THAT(d.value, WithinAbs(cp::cosineLerp(3.111112f, 6.0f, 0.25f), 1e-5));
    CHECK(d.value < 3.111112f + (6.0f - 3.111112f) * 0.25f);  // slower out of the start than evenly
    d.step(1.5f);
    CHECK(d.value == 6.0f);
    CHECK_FALSE(d.running);
}

TEST_CASE("the camera fades its model along half a cosine (0x008ca080)", "[camera]") {
    using namespace wowee::game::vehicle;
    // 0x3fea6e99 and 1/360: the bounds the player and a possessed unit fade by.
    CHECK(kCameraFadeFar == 1.8315f);
    CHECK(kCameraFadeNear == 0.0027777778f);
    const float span = kCameraFadeFar - kCameraFadeNear;
    CHECK(cameraFadeAlpha(kCameraFadeNear + span * 0.25f, kCameraFadeNear, kCameraFadeFar) ==
          Catch::Approx(37.0f / 255.0f));
    CHECK(cameraFadeAlpha(kCameraFadeNear + span * 0.9f, kCameraFadeNear, kCameraFadeFar) ==
          Catch::Approx(249.0f / 255.0f));
    CHECK(cameraFadeAlpha(1.9f, kCameraFadeNear, kCameraFadeFar) == 1.0f);
    CHECK(cameraFadeAlpha(0.001f, kCameraFadeNear, kCameraFadeFar) == 0.0f);
}

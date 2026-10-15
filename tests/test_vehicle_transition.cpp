// A passenger's way into and out of its seat: the first state of a change
// (0x00749aa0), the waits and arcs planned (0x0074a200), the progress
// (0x00747d70), the arc's height (0x0074a7f0), the facing on the way
// (0x00747a30), the animations (0x00747b20, 0x0074b9c0) and the roster's
// record of how a change came about.
#include <catch_amalgamated.hpp>

#include <cmath>

#include "game/vehicle.hpp"
#include "game/vehicle_camera.hpp"
#include "game/vehicle_transition.hpp"

namespace gv = wowee::game::vehicle;
using gv::SeatState;

namespace {
gv::SeatRecord seatWithMoves() {
    gv::SeatRecord s;
    s.id = 7;
    s.flags = gv::kSeatFlagHasEnterAnim | gv::kSeatFlagExitAnimForced;
    s.enterSpeed = 10.0f;
    s.enterGravity = 20.0f;
    s.enterMinDuration = 0.2f;
    s.enterMaxDuration = 2.0f;
    s.exitSpeed = 5.0f;
    s.exitMinDuration = 0.1f;
    s.exitMaxDuration = 3.0f;
    return s;
}
}  // namespace

TEST_CASE("a change of transport starts its state (0x00749aa0)", "[vehicle]") {
    gv::SeatRecord s = seatWithMoves();
    // At once unless moved through.
    CHECK(gv::firstState(true, &s, false, false) == SeatState::Seated);
    CHECK(gv::firstState(false, &s, false, false) == SeatState::None);
    // Moved through: entering, straight in without a pre-delay.
    CHECK(gv::firstState(true, &s, true, false) == SeatState::Entering);
    s.enterPreDelay = 0.5f;
    CHECK(gv::firstState(true, &s, true, false) == SeatState::EnterDelay);
    // Flags 0x1 and an EnterSpeed are wanted.
    s.enterSpeed = 0.0f;
    CHECK(gv::firstState(true, &s, true, false) == SeatState::Seated);
    // Leaving: by its forced exit flag, 0x8000; of its own will 0x8.
    CHECK(gv::firstState(false, &s, true, false) == SeatState::Exiting);
    CHECK(gv::firstState(false, &s, true, true) == SeatState::None);
    s.flags |= gv::kSeatFlagExitAnimVoluntary;
    s.exitPreDelay = 1.0f;
    CHECK(gv::firstState(false, &s, true, true) == SeatState::ExitDelay);
    s.exitSpeed = 0.0f;
    CHECK(gv::firstState(false, &s, true, true) == SeatState::None);
    // FlagsB 0x1000000 moves a leaving one through whatever brought it.
    gv::SeatRecord b;
    b.flagsB = gv::kSeatFlagBExitAlwaysAnimated;
    CHECK(gv::changeIsAnimated(false, true, &b));
    CHECK_FALSE(gv::changeIsAnimated(false, false, &b));
    CHECK(gv::changeIsAnimated(true, false, nullptr));
}

TEST_CASE("waits and arcs are planned as 0x0074a200 plans them", "[vehicle]") {
    CHECK(gv::preDelayMs(0.5f) == 500);
    CHECK(gv::preDelayMs(30.0f) == 10000);
    gv::SeatRecord s = seatWithMoves();
    // Into a vehicle: the distance over the seat's speed less the vehicle's.
    gv::Arc a = gv::planArc(s, true, 5.0f, 10.0f);
    CHECK(a.durationMs == 500);
    a = gv::planArc(s, true, 5.0f, 5.0f);
    CHECK(a.durationMs == 1000);
    // Held between the min and max durations.
    CHECK(gv::planArc(s, true, 0.5f, 10.0f).durationMs == 200);
    CHECK(gv::planArc(s, true, 100.0f, 10.0f).durationMs == 2000);
    // Closing at no speed: the max; nowhere to go: no time.
    CHECK(gv::planArc(s, true, 5.0f, -1.0f).durationMs == 2000);
    CHECK(gv::planArc(s, true, 0.0f, 10.0f).durationMs == 0);
    // No max duration reads 1.5 s, more than 10 reads 10.
    s.enterMaxDuration = 0.0f;
    CHECK(gv::planArc(s, true, 100.0f, 10.0f).durationMs == 1500);
    s.enterMaxDuration = 60.0f;
    CHECK(gv::planArc(s, true, 1000.0f, 10.0f).durationMs == 10000);
    // Out of one: by the exit speed.
    CHECK(gv::planArc(s, false, 5.0f, std::nullopt).durationMs == 1000);
    // The peak g T^2 / 8 brought into the arc heights.
    s.enterMaxDuration = 2.0f;
    a = gv::planArc(s, true, 5.0f, 5.0f);  // 1 s, g 20: peak 2.5, over a max of 0
    CHECK(a.heightScale == 0.0f);
    s.enterMaxArcHeight = 3.0f;
    a = gv::planArc(s, true, 5.0f, 5.0f);
    CHECK(a.heightScale == Catch::Approx(1.0f));
    s.enterMaxArcHeight = 1.0f;
    a = gv::planArc(s, true, 5.0f, 5.0f);
    CHECK(a.heightScale == Catch::Approx(0.4f));
    CHECK(gv::arcHeight(a, 0.5f) == Catch::Approx(1.0f));
    s.enterMinArcHeight = 5.0f;
    s.enterMaxArcHeight = 10.0f;
    a = gv::planArc(s, true, 5.0f, 5.0f);
    CHECK(a.heightScale == Catch::Approx(2.0f));
    // No gravity but an arc asked for: 0.1.
    s.enterGravity = 0.0f;
    a = gv::planArc(s, true, 5.0f, 5.0f);
    CHECK(a.gravity == Catch::Approx(0.1f));
    CHECK(gv::arcHeight(a, 0.5f) == Catch::Approx(5.0f));
    // The arc is 0 at either end.
    CHECK(gv::arcHeight(a, 0.0f) == 0.0f);
    CHECK(gv::arcHeight(a, 1.0f) == Catch::Approx(0.0f).margin(1e-6));
}

TEST_CASE("progress eases by the seat's flags (0x00747d70)", "[vehicle]") {
    CHECK(gv::progress(SeatState::Entering, 0, 1000, 2000, 1500) == Catch::Approx(0.5f));
    CHECK(gv::progress(SeatState::Entering, 0, 1000, 2000, 900) == 0.0f);
    CHECK(gv::progress(SeatState::Entering, 0, 1000, 2000, 2500) == 1.0f);
    CHECK(gv::progress(SeatState::Entering, 0, 1000, 1000, 1000) == 1.0f);
    CHECK(gv::progress(SeatState::Entering, gv::kSeatFlagEnterEaseIn, 1000, 2000, 1500) == Catch::Approx(0.25f));
    CHECK(gv::progress(SeatState::Entering, gv::kSeatFlagEnterEaseOut, 1000, 2000, 1500) == Catch::Approx(0.75f));
    CHECK(gv::progress(SeatState::Entering, gv::kSeatFlagEnterEaseIn | gv::kSeatFlagEnterEaseOut, 1000, 2000,
                       1250) == Catch::Approx(0.5f - 0.5f * std::cos(0.25f * 3.1415927f)));
    // The exit's flags are its own.
    CHECK(gv::progress(SeatState::Exiting, gv::kSeatFlagEnterEaseIn, 1000, 2000, 1500) == Catch::Approx(0.5f));
    CHECK(gv::progress(SeatState::Exiting, gv::kSeatFlagExitEaseIn, 1000, 2000, 1500) == Catch::Approx(0.25f));
}

TEST_CASE("the facing on the way turns the short way (0x00747a30)", "[vehicle]") {
    CHECK(gv::flightFacing(0.0f, 0.0f, 1.0f, 0.5f) == Catch::Approx(0.5f));
    // A facing a turn away is brought within half a turn of the drawn one.
    CHECK(gv::flightFacing(0.0f, 0.0f, 1.0f + 6.2831855f, 1.0f) == Catch::Approx(1.0f));
    CHECK(gv::flightFacing(3.0f, 3.0f, -3.0f, 1.0f) == Catch::Approx(-3.0f + 6.2831855f));
}

TEST_CASE("each state plays its seat's animations (0x00747b20, 0x0074b9c0)", "[vehicle]") {
    gv::SeatRecord s = seatWithMoves();
    s.enterAnimStart = 100;
    s.enterAnimLoop = 101;
    s.exitAnimLoop = 102;
    s.rideAnimLoop = 103;
    auto a = gv::seatAnimation(SeatState::EnterDelay, s, false);
    REQUIRE(a);
    CHECK(a->start == 100u);
    CHECK(a->loop == 101u);
    a = gv::seatAnimation(SeatState::Exiting, s, false);
    REQUIRE(a);
    CHECK_FALSE(a->start);
    CHECK(a->loop == 102u);
    // Of its own will it needs flag 0x8.
    CHECK_FALSE(gv::seatAnimation(SeatState::Exiting, s, true));
    // Seated needs 0x2.
    CHECK_FALSE(gv::seatAnimation(SeatState::Seated, s, false));
    s.flags |= gv::kSeatFlagHasRideAnim;
    CHECK(gv::seatAnimation(SeatState::Seated, s, false)->loop == 103u);
    // From 1 to 2 and out of 4 the animation goes on.
    CHECK(gv::keepsAnimation(SeatState::EnterDelay, SeatState::Entering));
    CHECK(gv::keepsAnimation(SeatState::ExitDelay, SeatState::Exiting));
    CHECK_FALSE(gv::keepsAnimation(SeatState::Entering, SeatState::Seated));

    // Landing: the ExitAnimEnd with 0x200000, JumpEnd turned JumpLandRun
    // while it moves; otherwise falling, swimming, flying first.
    s.exitAnimEnd = static_cast<int32_t>(gv::kAnimJumpEnd);
    s.flags |= gv::kSeatFlagHasExitAnimEnd;
    auto l = gv::landingAnimation(&s, false, true, false, false);
    REQUIRE(l);
    CHECK(l->id == gv::kAnimJumpEnd);
    CHECK(l->oneShot);
    CHECK(gv::landingAnimation(&s, true, false, false, false)->id == gv::kAnimJumpLandRun);
    s.flags &= ~gv::kSeatFlagHasExitAnimEnd;
    l = gv::landingAnimation(&s, false, true, false, false);
    REQUIRE(l);
    CHECK(l->id == gv::kAnimFall);
    CHECK_FALSE(l->oneShot);
    CHECK(gv::landingAnimation(&s, false, false, true, false)->id == gv::kAnimSwimIdle);
    CHECK_FALSE(gv::landingAnimation(&s, false, false, false, true));
    CHECK(gv::landingAnimation(&s, false, false, false, false)->id == gv::kAnimJumpEnd);
    CHECK_FALSE(gv::landingAnimation(nullptr, false, false, false, false));
}

TEST_CASE("the roster keeps how each change of seat came about", "[vehicle]") {
    gv::VehicleRoster r;
    r.setTransport(1, 50, 0, 0.0f, true);
    auto c = r.takeChange(1);
    REQUIRE(c);
    CHECK(c->animated);
    CHECK_FALSE(r.takeChange(1));
    // The same seat again is no change.
    r.setTransport(1, 50, 0, 1.0f, false);
    CHECK_FALSE(r.takeChange(1));
    // Leaving of its own will: flags2 0x40.
    r.setMoveFlags2(1, 0x40);
    r.setTransport(1, 0, -1, 0.0f, false);
    c = r.takeChange(1);
    REQUIRE(c);
    CHECK_FALSE(c->animated);
    CHECK(c->voluntary);
    // Not seated before, not seated after: nothing.
    r.setTransport(1, 0, -1, 0.0f, true);
    CHECK_FALSE(r.takeChange(1));
}

TEST_CASE("a vehicle animation's bone column (0x00756f80)", "[vehicle]") {
    CHECK(gv::vehicleAnimBone(0) == 0);
    CHECK(gv::vehicleAnimBone(0x22) == 0x22);
    CHECK(gv::vehicleAnimBone(0x23) == gv::kWholeModelBone);
    CHECK(gv::vehicleAnimBone(-1) == gv::kWholeModelBone);
    CHECK(gv::vehicleAnimBone(0x1a) == gv::kWholeModelBone);
}

TEST_CASE("a change of seat times the camera (0x0075aac0)", "[vehicle]") {
    gv::SeatRecord s;
    s.enterMaxDuration = 5.0f;
    s.exitMaxDuration = 0.2f;
    s.enterPreDelay = 0.25f;
    // Not moved through: at once, final.
    auto t = gv::cameraTiming(SeatState::Seated, &s, false);
    CHECK(t.fixed);
    CHECK(t.durationMs == 0);
    // The max duration held to 0.5..3 s, not final; after the wait.
    t = gv::cameraTiming(SeatState::EnterDelay, &s, true);
    CHECK_FALSE(t.fixed);
    CHECK(t.durationMs == 3000);
    CHECK(t.delayMs == 250);
    CHECK(gv::cameraTiming(SeatState::Exiting, &s, true).durationMs == 500);
    // The seat's own camera times with FlagsB 0x40 / 0x80.
    s.flagsB = gv::kSeatFlagBEnterCameraTimes;
    s.cameraEnteringDelay = 0.1f;
    s.cameraEnteringDuration = 1.2f;
    t = gv::cameraTiming(SeatState::Entering, &s, true);
    CHECK(t.fixed);
    CHECK(t.delayMs == 100);
    CHECK(t.durationMs == 1200);
    // No seat: half a second.
    CHECK(gv::cameraTiming(SeatState::None, nullptr, true).durationMs == 500);
}

TEST_CASE("a seat zooms the camera (0x0074c0e0)", "[vehicle]") {
    gv::SeatRecord s;
    // Out of a seat the distance put away comes back.
    auto z = gv::seatCameraZoom(false, &s, 10.0f, true);
    CHECK(z.restore);
    CHECK_FALSE(z.zoomTo);
    // A seat that zooms nothing gives it back too; a fixed one leaves it.
    CHECK(gv::seatCameraZoom(true, &s, 10.0f, true).restore);
    s.flags = gv::kSeatFlagFixedCamera;
    s.flagsB = gv::kSeatFlagBCameraZooms;
    CHECK_FALSE(gv::seatCameraZoom(true, &s, 10.0f, true).restore);
    // Limits and the entering zoom.
    s.flags = gv::kSeatFlagCanControl;
    s.cameraEnteringZoom = 20.0f;
    z = gv::seatCameraZoom(true, &s, 10.0f, false);
    REQUIRE(z.zoomTo);
    CHECK(*z.zoomTo == Catch::Approx(20.0f));
    CHECK(z.remember);
    CHECK_FALSE(z.minDistance);
    CHECK_FALSE(z.maxDistance);
    // Kept closer with FlagsB 0x1000.
    s.flagsB |= gv::kSeatFlagBKeepCloserZoom;
    CHECK_FALSE(gv::seatCameraZoom(true, &s, 10.0f, false).zoomTo);
    // Held in the seat's limits; one remembered already is kept.
    s.flags = 0;
    s.flagsB = gv::kSeatFlagBCameraZooms | gv::kSeatFlagBZoomMin | gv::kSeatFlagBZoomMax;
    s.cameraSeatZoomMin = 12.0f;
    s.cameraSeatZoomMax = 15.0f;
    z = gv::seatCameraZoom(true, &s, 30.0f, true);
    REQUIRE(z.minDistance);
    CHECK(*z.minDistance == Catch::Approx(12.0f));
    CHECK(*z.maxDistance == Catch::Approx(15.0f));
    CHECK(*z.zoomTo == Catch::Approx(15.0f));
    CHECK_FALSE(z.remember);
    // A big entering zoom widens the max.
    s.flags = gv::kSeatFlagCanControl;
    s.flagsB = gv::kSeatFlagBCameraZooms;
    s.cameraEnteringZoom = 60.0f;
    z = gv::seatCameraZoom(true, &s, 10.0f, false);
    REQUIRE(z.maxDistance);
    CHECK(*z.maxDistance == Catch::Approx(60.0f));
}

TEST_CASE("the roster answers a unit's seat row and state (0x00613600)", "[vehicle]") {
    gv::VehicleTables tables;
    gv::VehicleRecord v;
    v.id = 36;
    v.seatIds = {101, 0, 0, 0, 0, 0, 0, 0};
    tables.addVehicle(v);
    gv::SeatRecord s;
    s.id = 101;
    s.flags = 0x800;
    tables.addSeat(s);
    gv::VehicleRoster r;
    r.setTables(&tables);
    r.setVehicleId(50, 36);
    r.setTransport(1, 50, 0, 0.0f);
    REQUIRE(r.seatOf(1));
    CHECK(r.seatOf(1)->id == 101u);
    CHECK(r.vehicleOf(1)->id == 36u);
    CHECK(r.seatState(1) == 0);
    r.setSeatState(1, 3);
    CHECK(r.seatState(1) == 3);
    r.forget(1);
    CHECK(r.seatState(1) == 0);
    CHECK_FALSE(r.seatOf(1));
}

TEST_CASE("the camera's ease is 0x005fffd0's cubic (0x005fe800's split)", "[vehicle]") {
    CHECK(gv::cameraEase(0.0f) == Catch::Approx(0.0f));
    CHECK(gv::cameraEase(0.25f) == Catch::Approx(0.15625f));
    CHECK(gv::cameraEase(0.5f) == Catch::Approx(0.5f));
    // Past the half it is split from 1, and the cubic is not even in it.
    CHECK(gv::cameraEase(0.75f) == Catch::Approx(0.78125f));
    CHECK(gv::cameraEase(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("a vehicle's camera offsets are eased over the move (0x005fe890, 0x006006a0)", "[vehicle]") {
    gv::EasedCameraOffset yaw;
    // At once where the times are one.
    yaw.set(0.5f, 100, 100);
    CHECK(yaw.value == Catch::Approx(0.5f));
    yaw.set(1.5f, 1000, 2000);
    yaw.step(1500);
    CHECK(yaw.value == Catch::Approx(1.0f));
    yaw.step(2001);
    CHECK(yaw.value == Catch::Approx(1.5f));
    CHECK(yaw.startMs == yaw.endMs);
}

TEST_CASE("a camera offset's target is where it eases to, or where it rests", "[vehicle]") {
    gv::EasedCameraOffset yaw;
    CHECK(yaw.target() == 0.0f);
    yaw.set(0.5f, 100, 100);
    CHECK(yaw.target() == Catch::Approx(0.5f));
    // On its way back to 0 it is already released, whatever it is at now.
    yaw.set(0.0f, 1000, 1500);
    yaw.step(1200);
    CHECK(yaw.value != 0.0f);
    CHECK(yaw.target() == 0.0f);
    yaw.step(1600);
    CHECK(yaw.target() == 0.0f);
}

TEST_CASE("the mouse adds to a view's pitch being eased to (0x006020b0 -> 0x005ffc20)", "[vehicle][camera]") {
    gv::EasedCameraOffset pitch;
    pitch.value = -10.0f;
    pitch.set(30.0f, 1000, 2000);
    pitch.step(1500);
    REQUIRE(pitch.value == Catch::Approx(10.0f));
    // Up 5: from, to and where it is all move, and it keeps easing.
    pitch.shift(5.0f, -88.0f, 88.0f);
    CHECK(pitch.value == Catch::Approx(15.0f));
    CHECK(pitch.easing());
    CHECK(pitch.target() == Catch::Approx(35.0f));
    pitch.step(1500);
    CHECK(pitch.value == Catch::Approx(15.0f));  // not lost to the next step
    pitch.step(2001);
    CHECK(pitch.value == Catch::Approx(35.0f));
    CHECK_FALSE(pitch.easing());

    // Each end held to the limits on its own.
    gv::EasedCameraOffset up;
    up.value = 0.0f;
    up.set(80.0f, 0, 1000);
    up.shift(20.0f, -88.0f, 88.0f);
    CHECK(up.from == Catch::Approx(20.0f));
    CHECK(up.to == Catch::Approx(88.0f));
    CHECK(up.value == Catch::Approx(20.0f));

    // At rest it is only moved and held.
    gv::EasedCameraOffset rest;
    rest.value = 85.0f;
    rest.shift(10.0f, -88.0f, 88.0f);
    CHECK(rest.value == Catch::Approx(88.0f));
    CHECK_FALSE(rest.easing());
}

TEST_CASE("the wheel moves where a view's distance ends, not where it is (0x006000e0)", "[vehicle][camera]") {
    gv::EasedCameraOffset distance;
    distance.value = 5.0f;
    distance.set(15.0f, 1000, 2000);
    distance.step(1500);
    REQUIRE(distance.value == Catch::Approx(10.0f));
    distance.retarget(25.0f);
    CHECK(distance.value == Catch::Approx(10.0f));
    CHECK(distance.target() == Catch::Approx(25.0f));
    // From where it set out to the new end, in the time it had.
    distance.step(1500);
    CHECK(distance.value == Catch::Approx(15.0f));
    distance.step(2001);
    CHECK(distance.value == Catch::Approx(25.0f));
    CHECK_FALSE(distance.easing());
    distance.retarget(8.0f);
    CHECK(distance.value == Catch::Approx(8.0f));
}

TEST_CASE("a seat picks the camera's distance mode (0x0074c0e0 -> 0x00600590)", "[vehicle]") {
    gv::SeatRecord fixedCamera;
    fixedCamera.flags = gv::kSeatFlagFixedCamera;
    auto m = gv::seatCameraMode(true, &fixedCamera, true);
    CHECK(m.mode == 1);
    CHECK(m.animate);
    gv::SeatRecord zooms;
    zooms.flagsB = gv::kSeatFlagBCameraZooms;
    m = gv::seatCameraMode(true, &zooms, false);
    CHECK(m.mode == 0);
    CHECK_FALSE(m.animate);
    m = gv::seatCameraMode(false, &fixedCamera, false);
    CHECK(m.mode == 0);
    CHECK(m.animate);
    CHECK_FALSE(gv::seatCameraMode(false, nullptr, true).animate);
    // The saved distances: the ordinary one held to 0..50, the vehicle's
    // 50 while it is the "-1.0" it starts as.
    CHECK(gv::savedModeDistance(0, 80.0f, 3.0f) == 50.0f);
    CHECK(gv::savedModeDistance(0, 12.0f, 3.0f) == 12.0f);
    CHECK(gv::savedModeDistance(1, 12.0f, -1.0f) == 50.0f);
    CHECK(gv::savedModeDistance(1, 12.0f, 30.0f) == 30.0f);
}

TEST_CASE("the vehicle camera chases as 0x007160b0 and 0x006ff4c0 step", "[vehicle]") {
    // One step at rate 20 over 0.05 s: x = 1, d = 1 / 2.715.
    float value = 1.0f;
    float velocity = 0.0f;
    gv::chaseStep(value, velocity, 0.0f, 20.0f, 0.05f);
    const float d = 1.0f / 2.715f;
    CHECK(value == Catch::Approx(2.0f * d));
    CHECK(velocity == Catch::Approx(-20.0f * d));
    // It settles on the target without passing it.
    for (int i = 0; i < 200; ++i) {
        gv::chaseStep(value, velocity, 0.0f, 20.0f, 0.016f);
        CHECK(value >= -1e-4f);
    }
    CHECK(value == Catch::Approx(0.0f).margin(1e-4));
    glm::vec3 p(2.0f, 0.0f, 0.0f), v(0.0f);
    gv::chaseStep(p, v, glm::vec3(0.0f), 20.0f, 0.05f);
    CHECK(p.x == Catch::Approx(4.0f * d));
    // A rate of 0.0001 or less is no chase; the rate is 20 over the column.
    CHECK_FALSE(gv::seatChaseRate(0.0f));
    CHECK(*gv::seatChaseRate(2.0f) == Catch::Approx(10.0f));
}

TEST_CASE("a seat's CameraOffset is (Y, -X, Z) turned by its facing (0x00759200)", "[vehicle]") {
    gv::SeatRecord seat;
    seat.cameraOffset = glm::vec3(1.0f, -10.0f, 3.0f);  // a yard right, ten behind, three up
    // Render yaw 0 looks along +x; its right is +y there.
    glm::vec3 o = gv::seatCameraOffset(seat, 0.0f);
    CHECK(o.x == Catch::Approx(-10.0f));
    CHECK(o.y == Catch::Approx(1.0f));
    CHECK(o.z == Catch::Approx(3.0f));
    o = gv::seatCameraOffset(seat, 1.5707964f);
    CHECK(o.x == Catch::Approx(-1.0f));
    CHECK(o.y == Catch::Approx(-10.0f).margin(1e-5));
}

TEST_CASE("a move into or out of a seat waits on the vehicle's animation (0x0074bcb0, 0x00748070)", "[vehicle]") {
    gv::SeatRecord seat;
    seat.vehicleEnterAnim = 150;
    seat.vehicleEnterAnimBone = 5;
    seat.vehicleExitAnim = 151;
    seat.vehicleExitAnimBone = 40;  // past 0x22: the whole model
    // No flag, no animation.
    CHECK_FALSE(gv::vehicleSeatAnim(seat, true, false));
    seat.flags = gv::kSeatFlagVehicleEnterAnim | gv::kSeatFlagVehicleExitAnimForced;
    auto in = gv::vehicleSeatAnim(seat, true, false);
    REQUIRE(in);
    CHECK(in->animation == 150u);
    CHECK(in->bone == 5);
    CHECK_FALSE(in->timed);
    CHECK(in->waitMs == 3000u);
    CHECK(in->eventBase == 0x30475624u);  // "$VG0"
    // Leaving of its own will asks 0x40000, which this seat has not.
    CHECK_FALSE(gv::vehicleSeatAnim(seat, false, true));
    auto out = gv::vehicleSeatAnim(seat, false, false);
    REQUIRE(out);
    CHECK(out->bone == gv::kWholeModelBone);
    CHECK(out->eventBase == 0x30545624u);  // "$VT0"
    // A delay where the seat gives one, three seconds at most.
    seat.flags |= gv::kSeatFlagEnterAnimDelayed;
    seat.vehicleEnterAnimDelay = 0.75f;
    in = gv::vehicleSeatAnim(seat, true, false);
    CHECK(in->timed);
    CHECK(in->waitMs == 750u);
    seat.vehicleEnterAnimDelay = 9.0f;
    CHECK(gv::vehicleSeatAnim(seat, true, false)->waitMs == 3000u);
    // 0x1fa or past it is no animation.
    seat.vehicleEnterAnim = 0x1fa;
    CHECK_FALSE(gv::vehicleSeatAnim(seat, true, false));
}

TEST_CASE("the camera's closeness fades its model between the bounds (0x00606f90)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    CHECK(cameraFadeAlpha(5.0f, kCameraFadeNear, kCameraFadeFar) == 1.0f);
    CHECK(cameraFadeAlpha(kCameraFadeFar, kCameraFadeNear, kCameraFadeFar) == 1.0f);
    CHECK(cameraFadeAlpha(0.0f, kCameraFadeNear, kCameraFadeFar) == 0.0f);
    CHECK(cameraFadeAlpha(1.0f, 0.0f, 2.0f) == Catch::Approx(0.5f));
    // A vehicle's own: its scalars times its model's radius.
    CHECK(cameraFadeAlpha(3.0f, 0.5f * 4.0f, 1.0f * 4.0f) == Catch::Approx(0.5f));
}

TEST_CASE("the vehicle camera is pulled in where the world is in the way and eases out (0x00759580)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    CHECK(vehicleCameraHeight(5.0f) == Catch::Approx(2.0277777f - 0.16666667f));
    CHECK(vehicleCameraHeight(1.0f) == Catch::Approx(1.0f - 0.16666667f));
    CameraPullIn pull;
    const glm::vec3 origin(0.0f);
    const glm::vec3 camera(9.0f, 0.0f, 0.0f);  // a reach of 10
    // Nothing in the way: one yard short of the reach, the camera itself.
    CHECK(vehicleCameraPulledIn(origin, camera, std::nullopt, pull).x == Catch::Approx(9.0f));
    // A hit halfway snaps in: 10 * 0.5 - 1.
    CHECK(vehicleCameraPulledIn(origin, camera, 0.5f, pull).x == Catch::Approx(4.0f));
    CHECK(pull.held == 0.5f);
    // A farther hit keeps the share held.
    CHECK(vehicleCameraPulledIn(origin, camera, 0.9f, pull).x == Catch::Approx(4.0f));
    // A hit within a yard: the vehicle's point.
    CHECK(vehicleCameraPulledIn(origin, camera, 0.05f, pull).x == Catch::Approx(0.0f));
    // Eased back out toward 1.
    pull.held = 0.5f;
    for (int i = 0; i < 200; ++i) easeCameraPullIn(pull, 0.05f);
    CHECK(pull.held == Catch::Approx(1.0f).margin(0.001));
}

TEST_CASE("the fade's far bound widens looked at steeply from close above (0x00606f90)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    // Level, or far off: as it is.
    CHECK(cameraFadeFarBound(kCameraFadeFar, false, 1.0f, 2.0f, -0.5f) == kCameraFadeFar);
    CHECK(cameraFadeFarBound(kCameraFadeFar, false, 3.0f, 2.0f, -1.5f) == kCameraFadeFar);
    // At 68 degrees down, about as it is; steeper, wider.
    CHECK(cameraFadeFarBound(kCameraFadeFar, false, 1.0f, 2.0f, -1.1868f) == Catch::Approx(kCameraFadeFar).epsilon(0.01));
    const float steep = cameraFadeFarBound(kCameraFadeFar, false, 1.0f, 2.0f, -1.5f);
    CHECK(steep == Catch::Approx(kCameraFadeFar / std::pow((1.5707964f - 1.5f) * 2.604353f, 2.0f)));
    // A third at the barber shop.
    CHECK(cameraFadeFarBound(3.0f, true, 5.0f, 2.0f, 0.0f) == Catch::Approx(1.0f));
}

TEST_CASE("the vehicle camera's mode at a change of seat (0x0075aac0)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    CameraTiming t{.delayMs = 300, .durationMs = 1500, .fixed = true};
    CHECK(cameraModeAtChange(SeatState::EnterDelay, t, 10).mode == CameraMode::EnterWait);
    CHECK(cameraModeAtChange(SeatState::EnterDelay, t, 10).ms == 300u);
    CHECK(cameraModeAtChange(SeatState::ExitDelay, t, 10).mode == CameraMode::ExitWait);
    t.delayMs = 0;
    CHECK(cameraModeAtChange(SeatState::Entering, t, 10).mode == CameraMode::Entering);
    CHECK(cameraModeAtChange(SeatState::Exiting, t, 10).mode == CameraMode::Exiting);
    // Settled at once: seated, or off.
    t.fixed = false;
    CHECK(cameraModeAtChange(SeatState::Seated, t, 10).mode == CameraMode::Seated);
    CHECK(cameraModeAtChange(SeatState::None, t, 10).mode == CameraMode::Off);
    // On the way, with a time not yet final, it is entering all the same.
    CHECK(cameraModeAtChange(SeatState::Entering, t, 10).mode == CameraMode::Entering);
}

TEST_CASE("the vehicle camera's modes move on with their time (0x0075af40)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    CameraModeClock c{.mode = CameraMode::EnterWait, .startMs = 1000, .ms = 300};
    stepCameraMode(c, 1299, true, 2000);
    CHECK(c.mode == CameraMode::EnterWait);
    // 50 ms over: entering for the final time less that.
    stepCameraMode(c, 1350, true, 2000);
    CHECK(c.mode == CameraMode::Entering);
    CHECK(c.ms == 1950u);
    CHECK(c.startMs == 1350u);
    stepCameraMode(c, 3300, true, 2000);
    CHECK(c.mode == CameraMode::Seated);
    // A wait to leave goes on to entering, as the client has it; with no
    // final time, a second.
    CameraModeClock w{.mode = CameraMode::ExitWait, .startMs = 0, .ms = 100};
    stepCameraMode(w, 100, false, 2000);
    CHECK(w.mode == CameraMode::Entering);
    CHECK(w.ms == 1000u);
    CameraModeClock x{.mode = CameraMode::Exiting, .startMs = 0, .ms = 500};
    stepCameraMode(x, 500, true, 500);
    CHECK(x.mode == CameraMode::Off);
}

TEST_CASE("how far along its mode the vehicle camera is (0x007598f0)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    SeatRecord seat;
    CameraModeClock c{.mode = CameraMode::Entering, .startMs = 1000, .ms = 1000};
    // On its own way in, with no camera time of the seat's: the way's share.
    CHECK(cameraModeProgress(c, 1500, SeatState::Entering, &seat, 0.2f) == Catch::Approx(0.2f));
    // With FlagsB 0x40: its own time.
    seat.flagsB = kSeatFlagBEnterCameraTimes;
    CHECK(cameraModeProgress(c, 1500, SeatState::Entering, &seat, 0.2f) == Catch::Approx(0.5f));
    CHECK(cameraModeProgress(c, 9000, SeatState::Entering, &seat, 0.2f) == 1.0f);
    c.mode = CameraMode::Seated;
    CHECK(cameraModeProgress(c, 1000, SeatState::Seated, &seat, 0.0f) == 1.0f);
    CHECK(cameraSpringsOnTheWay(CameraMode::Entering, nullptr) == false);
    seat.flagsB = 0x40000;
    CHECK(cameraSpringsOnTheWay(CameraMode::Exiting, &seat));
    CHECK_FALSE(cameraSpringsOnTheWay(CameraMode::Entering, &seat));
    CHECK(cameraWaySpringRate(1000, 0.0f) == Catch::Approx(4.0f / 1.001f + 1.0f));
}

TEST_CASE("a passenger's own place is its offset on the vehicle, turned (0x004f4460)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    // Ahead and to the left come out as a seat's CameraOffset of (Y, -X) does.
    SeatRecord seat;
    seat.cameraOffset = glm::vec3(-0.5f, 2.0f, 1.0f);  // (Y, -X): 2 ahead, 0.5 to the left
    const glm::vec3 a = transportOffsetAbout(glm::vec3(2.0f, 0.5f, 1.0f), 0.7f);
    const glm::vec3 b = seatCameraOffset(seat, 0.7f);
    CHECK(a.x == Catch::Approx(b.x));
    CHECK(a.y == Catch::Approx(b.y));
    CHECK(a.z == Catch::Approx(b.z));
}

TEST_CASE("the vehicle camera's facing in each mode (0x0075a1c0)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    SeatRecord seat;  // no facing chase, no spring, no FlagsB 0x1
    CameraFacing f;
    f.restartAt(0.0f);
    // Waiting, it holds the facing of the change: no turn.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::EnterWait, &seat, 0.0f, 1000, false, 0.016f, false, false) == 0.0f);
    CHECK(f.out == 0.0f);
    CHECK(f.target == Catch::Approx(1.0f));
    // Entering, eased from it to the facing by the way's share.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Entering, &seat, 0.5f, 1000, false, 0.016f, false, false) ==
          Catch::Approx(0.5f));
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Entering, &seat, 1.0f, 1000, false, 0.016f, false, false) ==
          Catch::Approx(0.5f));
    // Seated, the facing itself; within a half turn of the last.
    CHECK(stepCameraFacing(f, 1.0f + 6.2831855f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.016f, false, false) ==
          Catch::Approx(0.0f).margin(1e-5f));
    // The camera's own yaw held (+0xac): no turn.
    CHECK(stepCameraFacing(f, 1.5f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.016f, false, true) == 0.0f);
    CHECK(f.out == Catch::Approx(1.5f));
}

TEST_CASE("a seat's FlagsB 0x1 holds the camera's yaw unless the keys turn (0x0075a1c0)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    SeatRecord seat;
    seat.flagsB = 0x1;
    CameraFacing f;
    f.restartAt(0.0f);
    f.holdsYaw = seatHoldsCameraYaw(CameraMode::Entering, &seat);
    REQUIRE(f.holdsYaw);
    f.follows = true;
    // On the way in the facing is held where it was.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Entering, &seat, 0.5f, 1000, false, 0.016f, false, false) == 0.0f);
    CHECK(f.out == 0.0f);
    CHECK(f.target == 0.0f);
    // Seated, the turn is taken back (0x005fe5f0) while the facing moves.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.016f, false, false) == 0.0f);
    CHECK(f.out == Catch::Approx(1.0f));
    // The keys turning: the next step turns with it.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.016f, true, false) == 0.0f);
    CHECK_FALSE(f.follows);
    CHECK(stepCameraFacing(f, 1.25f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.016f, true, false) ==
          Catch::Approx(0.25f));
    // Waiting to leave the seat holds nothing (+0x8 0x2 is for 1 to 3).
    CHECK_FALSE(seatHoldsCameraYaw(CameraMode::ExitWait, &seat));
}

TEST_CASE("the driver's camera trails its vehicle's keyed turn by the chased facing (0x00604490)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    // The keys turn the vehicle at once; the camera turns by the facing
    // chased at 20 / CameraFacingChaseRate, so it lags the turn and catches
    // it up after.
    SeatRecord seat;
    seat.flags = kSeatFlagCanControl;
    seat.cameraFacingChaseRate = 10.0f;  // a rate of 2
    CameraFacing f;
    f.restartAt(0.0f);
    float turned = stepCameraFacing(f, 0.5f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.05f, true, false);
    CHECK(turned > 0.0f);
    CHECK(turned < 0.5f);
    for (int i = 0; i < 200; ++i)
        turned += stepCameraFacing(f, 0.5f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.05f, false, false);
    CHECK(turned == Catch::Approx(0.5f).margin(1e-3f));
    // A mouse button held - steering or looking about - holds the camera's
    // own yaw (0x005fa170 -> 0x006047e0, +0xac): the facing turns it not.
    CHECK(stepCameraFacing(f, 1.0f, CameraMode::Seated, &seat, 1.0f, 0, false, 0.05f, false, true) == 0.0f);
}

TEST_CASE("from a control seat the vehicle camera watches the vehicle (0x0075a930)", "[vehicle]") {
    using namespace wowee::game::vehicle;
    SeatRecord seat;
    CHECK_FALSE(cameraWatchesVehicle(false, &seat));
    seat.flags = kSeatFlagCanControl;
    CHECK(cameraWatchesVehicle(false, &seat));
    CHECK_FALSE(cameraWatchesVehicle(true, &seat));
    CHECK_FALSE(cameraWatchesVehicle(false, nullptr));
}

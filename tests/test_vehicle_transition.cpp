// A passenger's way into and out of its seat: the first state of a change
// (0x00749aa0), the waits and arcs planned (0x0074a200), the progress
// (0x00747d70), the arc's height (0x0074a7f0), the facing on the way
// (0x00747a30), the animations (0x00747b20, 0x0074b9c0) and the roster's
// record of how a change came about.
#include <catch_amalgamated.hpp>

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

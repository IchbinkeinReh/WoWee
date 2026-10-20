// Vehicles and their passengers: the roster the update and movement packets
// fill (0x0074c750, 0x007489c0), a seat's row (0x00756ec0), the passenger's
// matrix on the seat (0x007490f0), the seat's attachment (DAT_00a2d3f0) and
// its ride animation (0x00747b20).
#include <catch_amalgamated.hpp>

#include "game/vehicle.hpp"
#include "rendering/vehicle_seat.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace gv = wowee::game::vehicle;
namespace vs = wowee::rendering::vehicle_seat;

namespace {
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps)));
}
}  // namespace

TEST_CASE("a seat is the vehicle row's column for it (0x00756ec0)", "[vehicle]") {
    gv::VehicleTables tables;
    gv::VehicleRecord vehicle;
    vehicle.id = 36;
    vehicle.seatIds = {101, 102, 0, 0, 0, 0, 0, 0};
    tables.addVehicle(vehicle);
    gv::SeatRecord driver;
    driver.id = 101;
    gv::SeatRecord gunner;
    gunner.id = 102;
    gunner.flags = gv::kSeatFlagHidePassenger;
    tables.addSeat(driver);
    tables.addSeat(gunner);

    REQUIRE(tables.seat(36, 0) != nullptr);
    CHECK(tables.seat(36, 0)->id == 101u);
    CHECK(tables.seat(36, 1)->flags == gv::kSeatFlagHidePassenger);
    // An empty column, a seat of 8 or more, an unknown vehicle: none.
    CHECK(tables.seat(36, 2) == nullptr);
    CHECK(tables.seat(36, 8) == nullptr);
    CHECK(tables.seat(37, 0) == nullptr);
}

TEST_CASE("the roster keeps vehicles and who sits in them", "[vehicle]") {
    gv::VehicleRoster roster;
    roster.setVehicleId(0x10, 36);
    CHECK(roster.vehicleId(0x10) == 36u);
    // SET_VEHICLE_REC_ID with 0 resets it (0x0074c7b0).
    roster.setVehicleId(0x10, 0);
    CHECK(roster.vehicleId(0x10) == 0u);

    roster.setTransport(0x20, 0x10, 1, 0.5f);
    REQUIRE(roster.passenger(0x20).has_value());
    CHECK(roster.passenger(0x20)->vehicleGuid == 0x10u);
    CHECK(roster.passenger(0x20)->seat == 1);
    CHECK(roster.passenger(0x20)->facing == Catch::Approx(0.5f));

    // No transport, no seat byte, or one past the eight: in no seat.
    roster.setTransport(0x20, 0, 1, 0.0f);
    CHECK_FALSE(roster.passenger(0x20).has_value());
    roster.setTransport(0x21, 0x10, -1, 0.0f);
    CHECK_FALSE(roster.passenger(0x21).has_value());
    roster.setTransport(0x22, 0x10, 8, 0.0f);
    CHECK_FALSE(roster.passenger(0x22).has_value());

    roster.setVehicleId(0x30, 5);
    roster.setTransport(0x30, 0x10, 0, 0.0f);
    roster.forget(0x30);
    CHECK(roster.vehicleId(0x30) == 0u);
    CHECK_FALSE(roster.passenger(0x30).has_value());
}

TEST_CASE("a seat's attachment is DAT_00a2d3f0's", "[vehicle]") {
    CHECK(vs::seatModelAttachment(0) == 20);
    CHECK(vs::seatModelAttachment(1) == 34);
    CHECK(vs::seatModelAttachment(2) == 19);
    CHECK(vs::seatModelAttachment(10) == 16);
    CHECK(vs::seatModelAttachment(11) == 37);
    CHECK(vs::seatModelAttachment(20) == 46);
    CHECK(vs::seatModelAttachment(22) == -1);
    CHECK(vs::seatModelAttachment(-1) == -1);
}

TEST_CASE("the passenger sits on the seat's offset in the attachment's frame (0x007490f0)", "[vehicle]") {
    vs::PassengerPose pose;
    pose.seatOffset = glm::vec3(0.5f, 0.0f, 1.0f);
    // A frame at (10, 20, 30), turned a quarter, the vehicle twice its size.
    glm::mat4 frame = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 30.0f));
    frame = glm::rotate(frame, glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    frame = glm::scale(frame, glm::vec3(2.0f));

    const glm::mat4 world = vs::passengerWorld(frame, pose);
    // The offset is scaled with the vehicle and turned with the frame...
    CHECK(near(glm::vec3(world[3]), glm::vec3(10.0f, 21.0f, 32.0f)));
    // ...but the passenger is drawn at its own size.
    CHECK(glm::length(glm::vec3(world[0])) == Catch::Approx(1.0f));
    // Facing the frame's way: its forward is the frame's.
    CHECK(near(glm::normalize(glm::vec3(world[0])), glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("its facing less the seat's yaw turns it, then the roll and pitch", "[vehicle]") {
    vs::PassengerPose pose;
    pose.facing = 1.0f;
    pose.seatYaw = 1.0f + glm::half_pi<float>();
    const glm::mat4 local = vs::passengerLocal(pose, 1.0f);
    // facing - yaw = -pi/2: forward goes to -y.
    CHECK(near(glm::vec3(local[0]), glm::vec3(0.0f, -1.0f, 0.0f)));

    vs::PassengerPose pitched;
    pitched.seatPitch = glm::half_pi<float>();
    const glm::mat4 p = vs::passengerLocal(pitched, 1.0f);
    // Pitch about y: forward goes down.
    CHECK(near(glm::vec3(p[0]), glm::vec3(0.0f, 0.0f, -1.0f)));

    vs::PassengerPose rolled;
    rolled.seatRoll = glm::half_pi<float>();
    const glm::mat4 r = vs::passengerLocal(rolled, 1.0f);
    // Roll about x: left goes up.
    CHECK(near(glm::vec3(r[1]), glm::vec3(0.0f, 0.0f, 1.0f)));
}

TEST_CASE("the passenger's own attachment lands on the seat (0x00748400)", "[vehicle]") {
    vs::PassengerPose pose;
    pose.seatOffset = glm::vec3(0.0f, 0.0f, 2.0f);
    pose.passengerScale = 1.5f;
    pose.passengerAttachment = glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::mat4 local = vs::passengerLocal(pose, 1.0f);
    // The model's attachment point, by the matrix, is at the seat's offset.
    const glm::vec3 attachment = glm::vec3(local * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(near(attachment, glm::vec3(0.0f, 0.0f, 2.0f)));
    CHECK(near(glm::vec3(local[3]), glm::vec3(0.0f, 0.0f, 0.5f)));
}

TEST_CASE("a seat's ride animation needs Flags 0x2 (0x00747b20)", "[vehicle]") {
    CHECK_FALSE(vs::rideAnimation(0x0, 91, 92).has_value());
    const auto both = vs::rideAnimation(gv::kSeatFlagHasRideAnim, 91, 92);
    REQUIRE(both.has_value());
    CHECK(both->start == 91u);
    CHECK(both->loop == 92u);
    const auto loopOnly = vs::rideAnimation(gv::kSeatFlagHasRideAnim, -1, 92);
    REQUIRE(loopOnly.has_value());
    CHECK_FALSE(loopOnly->start.has_value());
    CHECK(loopOnly->loop == 92u);
    CHECK_FALSE(vs::rideAnimation(gv::kSeatFlagHasRideAnim, -1, -1).has_value());
}

TEST_CASE("an asked seat change lasts until the seat state changes or 2 s pass", "[vehicle]") {
    gv::VehicleRoster roster;
    constexpr uint64_t kPlayer = 0x11;
    roster.setSeatState(kPlayer, 3);
    CHECK_FALSE(roster.seatChangeAsked(kPlayer, 1000));
    roster.markSeatChangeAsked(kPlayer, 1000);
    CHECK(roster.seatChangeAsked(kPlayer, 2999));
    // 0x0074af70: gone at 2000 ms.
    CHECK_FALSE(roster.seatChangeAsked(kPlayer, 3000));
    // 0x007489c0: gone at the next state.
    roster.markSeatChangeAsked(kPlayer, 5000);
    roster.setSeatState(kPlayer, 5);
    CHECK_FALSE(roster.seatChangeAsked(kPlayer, 5001));
}

TEST_CASE("a seated passenger's upper body is SpineLow, else the head (0x0073e840)", "[vehicle]") {
    CHECK(vs::upperKeyBone(true, true) == 4);
    CHECK(vs::upperKeyBone(true, false) == 4);
    CHECK(vs::upperKeyBone(false, true) == 6);
    CHECK(vs::upperKeyBone(false, false) == vs::kNoUpperBody);
}

TEST_CASE("what a seated passenger is asked goes to its upper body unless it stands (0x007385c0)",
          "[vehicle]") {
    // BehaviorID 0: Stand and FlyStand give the upper body back.
    CHECK(vs::isStandBehavior(0));
    CHECK(vs::isStandBehavior(229));
    CHECK_FALSE(vs::seatedUpperBodyTakes(0));
    CHECK_FALSE(vs::seatedUpperBodyTakes(229));
    // Casts, attacks and emotes take it.
    CHECK(vs::seatedUpperBodyTakes(53));   // SpellCastDirected
    CHECK(vs::seatedUpperBodyTakes(17));   // Attack1H
    CHECK(vs::seatedUpperBodyTakes(67));   // EmoteWave
    CHECK(vs::seatedUpperBodyTakes(124));  // ChannelCastDirected
    // Movement a seated unit is never asked, and death, do not.
    CHECK_FALSE(vs::seatedUpperBodyTakes(5));   // Run
    CHECK_FALSE(vs::seatedUpperBodyTakes(42));  // Swim
    CHECK_FALSE(vs::seatedUpperBodyTakes(1));   // Death
}

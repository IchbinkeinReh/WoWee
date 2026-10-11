// The active mover, as Unit_C.cpp and Player_C.cpp switch it: a unit's
// SMSG_CLIENT_CONTROL_UPDATE (0x0072cca0), the player's PLAYER_FARSIGHT
// (0x006e2880), and a full-speed pitcher's pitch over time (0x006ef7a0).
#include <catch_amalgamated.hpp>
#include "game/vehicle_mover.hpp"

using namespace wowee::game::vehicle;

namespace {
constexpr uint64_t kPlayer = 0x0000000000000011ull;
constexpr uint64_t kVehicle = 0xF150001234000056ull;
}  // namespace

TEST_CASE("control of the unit the camera looks through makes it the mover", "[vehicle]") {
    // The farsight is the vehicle: control given moves it.
    CHECK(moverAfterControlUpdate(kVehicle, true, kVehicle, kPlayer, true) == kVehicle);
    // Taken back while it moves: no mover.
    CHECK(moverAfterControlUpdate(kVehicle, false, kVehicle, kVehicle, false) == uint64_t{0});
    // Taken back from one that is not the mover: nothing changes.
    CHECK_FALSE(moverAfterControlUpdate(kVehicle, false, kVehicle, kPlayer, false).has_value());
}

TEST_CASE("control of another unit waits for the camera", "[vehicle]") {
    // Before PLAYER_FARSIGHT names the vehicle, its control changes nothing.
    CHECK_FALSE(moverAfterControlUpdate(kVehicle, true, kPlayer, kPlayer, true).has_value());
    // The player given control while the camera still looks through the
    // vehicle that is no mover: nothing.
    CHECK_FALSE(moverAfterControlUpdate(kPlayer, true, kVehicle, 0, false).has_value());
    // The mover losing control hands it to the camera's controlled unit.
    CHECK(moverAfterControlUpdate(kVehicle, false, kPlayer, kVehicle, true) == kPlayer);
    CHECK(moverAfterControlUpdate(kVehicle, false, kPlayer, kVehicle, false) == uint64_t{0});
}

TEST_CASE("the farsight object moves where it is controlled, else the player", "[vehicle]") {
    CHECK(moverAfterFarsight(kVehicle, true, true, kPlayer, true) == kVehicle);
    CHECK(moverAfterFarsight(kVehicle, true, false, kPlayer, true) == kPlayer);
    CHECK(moverAfterFarsight(kVehicle, false, true, kPlayer, true) == kPlayer);
    CHECK(moverAfterFarsight(0, false, false, kPlayer, true) == kPlayer);
    CHECK(moverAfterFarsight(0, false, false, kPlayer, false) == uint64_t{0});
}

TEST_CASE("CMSG_FAR_SIGHT says 1 once the object is there and 0 once it is gone", "[vehicle]") {
    CHECK(farSightAnnouncement(kVehicle, true, false) == true);
    CHECK_FALSE(farSightAnnouncement(kVehicle, true, true).has_value());
    CHECK_FALSE(farSightAnnouncement(kVehicle, false, false).has_value());
    CHECK(farSightAnnouncement(0, false, true) == false);
    CHECK_FALSE(farSightAnnouncement(0, false, false).has_value());
}

TEST_CASE("a pitch over time runs its way at the pitch rate", "[vehicle]") {
    const PitchOverTime up = pitchOverTime(0.0f, 0.5f, 1.0f);
    CHECK(up.up);
    CHECK(up.durationMs == 500u);
    const PitchOverTime down = pitchOverTime(0.2f, -0.3f, 2.0f);
    CHECK_FALSE(down.up);
    CHECK(down.durationMs == 250u);
    // Wrapped to a half turn.
    const PitchOverTime wrapped = pitchOverTime(3.0f, -3.0f, 1.0f);
    CHECK(wrapped.up);
    CHECK(wrapped.durationMs == 283u);
}

TEST_CASE("the aim keys net out, and an unmoved stop sends nothing", "[vehicle]") {
    CHECK(aimKeyDirection(true, false) == 1);
    CHECK(aimKeyDirection(false, true) == -1);
    CHECK(aimKeyDirection(true, true) == 0);
    CHECK(pitchUnchangedSinceSent(1.0f, 1.05f, 0.2f, 0.25f, true));
    CHECK_FALSE(pitchUnchangedSinceSent(1.0f, 1.2f, 0.2f, 0.2f, true));
    CHECK_FALSE(pitchUnchangedSinceSent(1.0f, 1.0f, 0.2f, 0.4f, true));
}

TEST_CASE("the mover's flags2 hold its strafe and jump keys", "[vehicle]") {
    CHECK_FALSE(moverKeyRestrictions(0).noStrafe);
    CHECK_FALSE(moverKeyRestrictions(0).noJump);
    CHECK(moverKeyRestrictions(0x1).noStrafe);
    CHECK_FALSE(moverKeyRestrictions(0x1).noJump);
    CHECK(moverKeyRestrictions(0x2).noJump);
    CHECK_FALSE(moverKeyRestrictions(0x2).noStrafe);
    // Flags2 0x10 (pitches over time) holds neither.
    CHECK_FALSE(moverKeyRestrictions(0x10).noStrafe);
}

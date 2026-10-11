// What VehicleMenuBar reads of a vehicle: the vehicle a unit's seats count
// from (0x0074c650), how many seats it shows (0x00757470) and which vehicle
// and seat each is (0x00757550), what UnitVehicleSeatInfo says of one
// (0x007579e0), and the seat map of an indicator (0x00614d90).
#include <catch_amalgamated.hpp>

#include "game/vehicle.hpp"

#include <unordered_set>

namespace gv = wowee::game::vehicle;

namespace {
// Vehicle guids (high 0xF150) and a player's.
constexpr uint64_t kTank = 0xF150000100000001ull;
constexpr uint64_t kTurret = 0xF150000200000002ull;
constexpr uint64_t kPlayer = 0x0000000000000042ull;
constexpr uint64_t kOther = 0x0000000000000043ull;

struct World {
    gv::VehicleTables tables;
    gv::VehicleRoster roster;
    std::unordered_set<uint64_t> players{kPlayer, kOther};
    std::function<bool(uint64_t)> known = [](uint64_t) { return true; };
    std::function<bool(uint64_t)> isPlayer = [this](uint64_t g) { return players.count(g) != 0; };

    World() {
        // A tank: driver (control, root), an empty column, a gunner seat that
        // ejects, and a seat the turret rides.
        gv::VehicleRecord tank;
        tank.id = 10;
        tank.seatIds = {101, 0, 102, 103, 0, 0, 0, 0};
        tables.addVehicle(tank);
        gv::VehicleRecord turret;
        turret.id = 20;
        turret.seatIds = {201, 202, 0, 0, 0, 0, 0, 0};
        tables.addVehicle(turret);
        gv::SeatRecord s;
        s.id = 101;
        s.flags = 0x800 | gv::kSeatFlagCanSwitch;
        tables.addSeat(s);
        s = {};
        s.id = 102;
        s.flagsB = gv::kSeatFlagBEjectable;
        tables.addSeat(s);
        s = {};
        s.id = 103;
        tables.addSeat(s);
        s = {};
        s.id = 201;
        s.flags = 0x800;
        tables.addSeat(s);
        s = {};
        s.id = 202;
        tables.addSeat(s);
        roster.setTables(&tables);
        roster.setVehicleId(kTank, 10);
        roster.setVehicleId(kTurret, 20);
    }
};
}  // namespace

TEST_CASE("a unit's seats count from the vehicle at the top of its chain (0x0074c650)", "[vehicle]") {
    World w;
    // Not riding: a vehicle is its own, anything else has none.
    CHECK(w.roster.rootVehicle(kTank, 0, w.known) == kTank);
    CHECK(w.roster.rootVehicle(kPlayer, 0, w.known) == 0u);
    // The turret rides the tank; the player the turret.
    w.roster.setTransport(kTurret, kTank, 3, 0.0f);
    w.roster.setTransport(kPlayer, kTurret, 0, 0.0f);
    CHECK(w.roster.rootVehicle(kPlayer, 0, w.known) == kTank);
    CHECK(w.roster.rootVehicle(kTurret, 0, w.known) == kTank);
    // Stopped where it meets the one asked after.
    CHECK(w.roster.rootVehicle(kPlayer, kTurret, w.known) == kTurret);
    // A vehicle out of view ends the walk with none.
    std::function<bool(uint64_t)> noTank = [](uint64_t g) { return g != kTank; };
    CHECK(w.roster.rootVehicle(kPlayer, 0, noTank) == 0u);
    // A transport that is neither a vehicle's nor a player's is not walked.
    w.roster.setTransport(kOther, 0xF130000300000003ull, 0, 0.0f);
    CHECK(w.roster.rootVehicle(kOther, 0, w.known) == 0u);
}

TEST_CASE("a vehicle shows its seats, a riding vehicle's in place of its own (0x00757470, 0x00757550)", "[vehicle]") {
    World w;
    CHECK(w.roster.uiSeatCount(kTank, w.isPlayer) == 3u);
    auto at = w.roster.uiSeat(kTank, 1, w.isPlayer);
    REQUIRE(at);
    CHECK(at->vehicleGuid == kTank);
    CHECK(at->seat == 2);  // the empty column is skipped
    CHECK_FALSE(w.roster.uiSeat(kTank, 3, w.isPlayer));

    // The turret in seat 3: its two seats stand in for that one.
    w.roster.setTransport(kTurret, kTank, 3, 0.0f);
    CHECK(w.roster.uiSeatCount(kTank, w.isPlayer) == 4u);
    at = w.roster.uiSeat(kTank, 2, w.isPlayer);
    REQUIRE(at);
    CHECK(at->vehicleGuid == kTurret);
    CHECK(at->seat == 0);
    at = w.roster.uiSeat(kTank, 3, w.isPlayer);
    REQUIRE(at);
    CHECK(at->vehicleGuid == kTurret);
    CHECK(at->seat == 1);

    // A player who is a vehicle rides as a passenger, not as seats.
    w.roster.setVehicleId(kOther, 20);
    w.roster.setTransport(kOther, kTank, 0, 0.0f);
    CHECK(w.roster.uiSeatCount(kTank, w.isPlayer) == 4u);
    CHECK(w.roster.occupant(kTank, 0) == kOther);
    CHECK(w.roster.occupant(kTank, 2) == 0u);
}

TEST_CASE("UnitVehicleSeatInfo's answers for a seat (0x007579e0)", "[vehicle]") {
    World w;
    w.roster.setTransport(kTurret, kTank, 3, 0.0f);
    w.roster.setTransport(kPlayer, kTank, 2, 0.0f);

    auto info = gv::uiSeatInfo(w.roster, kTank, 0, w.isPlayer);
    REQUIRE(info);
    CHECK(std::string(info->controlType) == "Root");
    CHECK(info->occupant == 0u);
    CHECK(info->canSwitchSeats);
    CHECK_FALSE(info->ejectable);

    info = gv::uiSeatInfo(w.roster, kTank, 1, w.isPlayer);
    REQUIRE(info);
    CHECK(std::string(info->controlType) == "None");
    CHECK(info->occupant == kPlayer);
    CHECK(info->ejectable);
    CHECK_FALSE(info->canSwitchSeats);

    // The turret's control seat is a child's.
    info = gv::uiSeatInfo(w.roster, kTank, 2, w.isPlayer);
    REQUIRE(info);
    CHECK(std::string(info->controlType) == "Child");

    CHECK_FALSE(gv::uiSeatInfo(w.roster, kTank, 4, w.isPlayer));
}

TEST_CASE("an indicator's seats are its VehicleUIIndSeat rows in id order (0x00614d90)", "[vehicle]") {
    gv::VehicleTables tables;
    tables.addIndicatorSeat({.id = 9, .indicatorId = 3, .virtualSeatIndex = 1, .x = 0.5f, .y = 0.25f});
    tables.addIndicatorSeat({.id = 4, .indicatorId = 3, .virtualSeatIndex = 0, .x = 0.1f, .y = 0.2f});
    tables.addIndicatorSeat({.id = 5, .indicatorId = 7, .virtualSeatIndex = 0, .x = 0.0f, .y = 0.0f});
    // No VehicleUIIndicator row: no indicator.
    CHECK(tables.indicator(3) == nullptr);
    tables.addIndicator(3, "Interface\\Vehicles\\SeatIndicator\\Tank");
    const gv::UIIndicator* ind = tables.indicator(3);
    REQUIRE(ind);
    CHECK(ind->backgroundTexture == "Interface\\Vehicles\\SeatIndicator\\Tank");
    REQUIRE(ind->seats.size() == 2u);
    CHECK(ind->seats[0].id == 4u);
    CHECK(ind->seats[0].virtualSeatIndex == 0u);
    CHECK(ind->seats[1].x == Catch::Approx(0.5f));
}

TEST_CASE("the aim is held in its row's limits (0x005fb3a0, 0x005f9e60, 0x005f9f10)", "[vehicle]") {
    // Without Flags 0x40, a quarter turn either way; no row, as asked.
    gv::VehicleRecord free;
    CHECK(gv::clampAim(&free, 3.0f) == Catch::Approx(1.5707964f));
    CHECK(gv::clampAim(&free, -3.0f) == Catch::Approx(-1.5707964f));
    CHECK(gv::clampAim(nullptr, 3.0f) == Catch::Approx(3.0f));
    gv::VehicleRecord limited;
    limited.flags = gv::kVehicleFlagPitchLimits | gv::kVehicleFlagAimAngle;
    limited.pitchMin = 0.0f;
    limited.pitchMax = 1.0f;
    CHECK(gv::clampAim(&limited, -0.5f) == Catch::Approx(0.0f));
    CHECK(gv::clampAim(&limited, 0.25f) == Catch::Approx(0.25f));
    CHECK(gv::clampAim(&limited, 1.0f) == Catch::Approx(1.0f));
    CHECK(gv::normalizedAim(&limited, 0.25f) == Catch::Approx(0.25f));
    CHECK(gv::normalizedAim(&free, 0.0f) == Catch::Approx(0.5f));
    // Limits that are one point: 0.
    limited.pitchMax = 0.0f;
    CHECK(gv::normalizedAim(&limited, 0.0f) == Catch::Approx(0.0f));
    // The power: below 0 is 0, 1 or more is 1.
    CHECK(gv::clampAimPower(-0.2f) == 0.0f);
    CHECK(gv::clampAimPower(0.4f) == Catch::Approx(0.4f));
    CHECK(gv::clampAimPower(1.0f) == 1.0f);
}

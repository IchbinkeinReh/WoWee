// The aimed trajectory cast's solver, as 0x006fcd60 works it out.
#include <catch_amalgamated.hpp>

#include <vector>

#include "game/missile_trajectory.hpp"

namespace mt = wowee::game::missile_trajectory;
using Catch::Approx;

namespace {
mt::MissileRow catapult() {
    // SpellMissile 61 in 3.3.5a.
    mt::MissileRow r;
    r.flags = 1;
    r.pitchMin = 0.698f;
    r.pitchMax = 0.873f;
    r.speedMin = 30.0f;
    r.speedMax = 40.0f;
    r.gravity = 19.29f;
    return r;
}
auto constant(float v) {
    return [v] { return v; };
}
}  // namespace

TEST_CASE("without a vehicle row the aim is the middle of the row's", "[trajectory]") {
    const auto row = catapult();
    mt::AimInput in;
    in.unitPitch = 1.2f;
    in.aimPower = 1.0f;
    const auto aim = mt::chooseAim(row, in, constant(0.5f));
    CHECK(aim.pitch == Approx((0.698f + 0.873f) * 0.5f));
    CHECK(aim.speed == Approx(35.0f));
}

TEST_CASE("a vehicle aiming by pitch takes the unit's, held in its limits", "[trajectory]") {
    const auto row = catapult();
    mt::AimInput in;
    in.hasVehicleRow = true;
    in.vehicleFlags = mt::kVehicleFlagAimFromPitch;
    in.unitPitch = 1.2f;
    CHECK(mt::chooseAim(row, in, constant(0.5f), true).pitch == Approx(1.2f));
    in.vehicleFlags |= mt::kVehicleFlagPitchLimits;
    in.vehiclePitchMin = 0.1f;
    in.vehiclePitchMax = 0.9f;
    CHECK(mt::chooseAim(row, in, constant(0.5f), true).pitch == Approx(0.9f));
    in.unitPitch = -0.5f;
    CHECK(mt::chooseAim(row, in, constant(0.5f), true).pitch == Approx(0.1f));
}

TEST_CASE("a vehicle aiming by power sets the speed between the row's", "[trajectory]") {
    const auto row = catapult();
    mt::AimInput in;
    in.hasVehicleRow = true;
    in.vehicleFlags = mt::kVehicleFlagAimPower;
    in.aimPower = 0.25f;
    CHECK(mt::chooseAim(row, in, constant(0.5f), true).speed == Approx(32.5f));
}

TEST_CASE("the spreads and jitter are drawn in the client's order", "[trajectory]") {
    auto row = catapult();
    row.pitchRandMin = -0.1f;
    row.pitchRandMax = 0.1f;
    row.speedRandMin = 0.0f;
    row.speedRandMax = 4.0f;
    row.facingRandMin = 0.0f;
    row.facingRandMax = 1.0f;
    mt::AimInput in;
    in.facing = 2.0f;
    in.launch = {10.0f, 20.0f, 30.0f};
    std::vector<float> draws = {1.0f, 0.5f, 0.25f, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f};
    size_t i = 0;
    const auto aim = mt::chooseAim(row, in, [&] { return draws.at(i++); });
    CHECK(i == 8);
    CHECK(aim.pitch == Approx((0.698f + 0.873f) * 0.5f + 0.1f - 0.01f));
    CHECK(aim.speed == Approx(35.0f + 2.0f + 0.01f));
    CHECK(aim.facing == Approx(2.25f));
    CHECK(aim.launch.x == Approx(9.9f));
    CHECK(aim.launch.y == Approx(20.1f));
    CHECK(aim.launch.z == Approx(30.0f));
}

TEST_CASE("the flight may last MaxDuration's steps, at most 200", "[trajectory]") {
    mt::MissileRow r;
    CHECK(mt::maxSteps(r) == 200);
    r.maxDuration = 5.0f;
    CHECK(mt::maxSteps(r) == 50);
    r.maxDuration = 90.0f;
    CHECK(mt::maxSteps(r) == 200);
}

TEST_CASE("a launch point behind a wall lands at once", "[trajectory]") {
    mt::Aim aim;
    aim.launch = {1.0f, 2.0f, 3.0f};
    aim.speed = 30.0f;
    const auto f = mt::fly(catapult(), aim, {0.0f, 0.0f, 0.0f},
                           [](const glm::vec3&, const glm::vec3&) { return std::optional<float>(0.5f); });
    CHECK(f.impact == aim.launch);
    CHECK(f.seconds == 0.0f);
}

TEST_CASE("a flat shot falls onto the ground plane", "[trajectory]") {
    // Level, 30 yd/s east from 10 yards up over a floor at z = 0.
    mt::MissileRow row;
    row.gravity = 20.0f;
    mt::Aim aim;
    aim.speed = 30.0f;
    aim.launch = {0.0f, 0.0f, 10.0f};
    int calls = 0;
    const auto floor = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<float> {
        ++calls;
        if (calls == 1) return std::nullopt;  // the origin sees the launch point
        if (b.z > 0.0f) return std::nullopt;
        return a.z / (a.z - b.z);
    };
    const auto f = mt::fly(row, aim, {0.0f, 0.0f, 9.0f}, floor);
    CHECK(f.impact.z == Approx(0.0f).margin(1e-4));
    CHECK(f.impact.y == Approx(0.0f));
    // About sqrt(2 * 10 / 20) = 1 s of flight and 30 yards east.
    CHECK(f.seconds == Approx(1.0f).margin(0.06f));
    CHECK(f.impact.x == Approx(30.0f * f.seconds).margin(0.01f));
}

TEST_CASE("a shot meeting nothing stops when its steps run out", "[trajectory]") {
    mt::MissileRow row;
    row.maxDuration = 1.0f;
    mt::Aim aim;
    aim.speed = 10.0f;
    const auto f = mt::fly(row, aim, {}, [](const glm::vec3&, const glm::vec3&) { return std::nullopt; });
    CHECK(f.seconds == Approx(1.0f));
    CHECK(f.impact.x == Approx(10.0f));
}

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

TEST_CASE("the aim state is sent, begun, due once its cast time has passed, and let go", "[trajectory]") {
    mt::AimState s;
    s.sent(100, 5000);
    CHECK_FALSE(s.busy());
    CHECK_FALSE(s.due(99999));
    s.started(200, 5100, 1500);  // another spell: nothing
    CHECK_FALSE(s.busy());
    s.started(100, 5100, 1500);
    CHECK(s.busy());
    CHECK_FALSE(s.due(6499));
    CHECK(s.due(6500));  // from when it was sent, not begun
    s.heldCastCount = 3;
    CHECK(s.holding());
    CHECK_FALSE(s.due(7000));
    s.clear(200);
    CHECK(s.busy());
    s.clear(100);
    CHECK_FALSE(s.busy());
    CHECK(s.spellId == 0);
    CHECK(s.heldCastCount == 0);
    CHECK(s.castMs == 1500);  // 0x006fbe80 leaves it
}

TEST_CASE("the shot flies half the latency and a fifth more, at most 1.3 times", "[trajectory]") {
    CHECK(mt::flightWithLatency(2.0f, 100) == Approx(2.25f));
    CHECK(mt::flightWithLatency(0.5f, 100) == Approx(0.65f));
    CHECK(mt::flightWithLatency(0.0f, 0) == 0.0f);
}


TEST_CASE("the stamp touches only the lowest bits and moves its count on", "[trajectory]") {
    uint32_t count = 0x12345678u;
    const glm::vec3 launch(1234.5f, -567.25f, 88.0f);
    const glm::vec3 impact(1300.0f, -500.0f, 70.5f);
    const auto a = mt::stamp(count, 0.8f, 35.0f, launch, impact);
    CHECK(count == 0x12345679u);
    const auto low = [](float x, float y) { return (mt::floatBits(x) ^ mt::floatBits(y)) & ~0xfu; };
    CHECK(low(a.pitch, 0.8f) == 0u);
    CHECK(low(a.speed, 35.0f) == 0u);
    CHECK(low(a.launch.x, launch.x) == 0u);
    CHECK(low(a.launch.y, launch.y) == 0u);
    CHECK(low(a.launch.z, launch.z) == 0u);
    CHECK(low(a.impact.x, impact.x) == 0u);
    CHECK(low(a.impact.y, impact.y) == 0u);
    CHECK(low(a.impact.z, impact.z) == 0u);
    // The same figures at the same count stamp the same.
    uint32_t again = 0x12345678u;
    const auto b = mt::stamp(again, 0.8f, 35.0f, launch, impact);
    CHECK(mt::floatBits(a.pitch) == mt::floatBits(b.pitch));
    CHECK(mt::floatBits(a.impact.z) == mt::floatBits(b.impact.z));
}

TEST_CASE("the stamp's hash takes the key's bytes as they are", "[trajectory]") {
    namespace cl = wowee::game::chat_language;
    CHECK(cl::stringHash("", false) == 0x7fed7fedu);
    CHECK(cl::stringHash("ab", false) != cl::stringHash("ab"));
    CHECK(cl::stringHash("AB", false) == cl::stringHash("ab"));
}

TEST_CASE("Queued missiles are let go when the cast kit's animation is over, or after two seconds",
          "[missile_trajectory]") {
    using wowee::game::missile_trajectory::releaseDue;
    bool seen = false;
    CHECK_FALSE(releaseDue(1000, 1000, false, seen));  // not started yet
    CHECK_FALSE(releaseDue(1100, 1000, true, seen));
    CHECK(seen);
    CHECK(releaseDue(1200, 1000, false, seen));        // over
    bool never = false;
    CHECK_FALSE(releaseDue(2999, 1000, false, never));
    CHECK(releaseDue(3000, 1000, false, never));       // two seconds
    bool still = false;
    CHECK(releaseDue(3000, 1000, true, still));        // two seconds while it plays
}

// CharProc 16's mount transition (0x007fa6a0, 0x007fb7f0, 0x007fa930,
// 0x0071fbf0): the mount carried from its $STB to the rider through its
// Birth, on the ground and faded in, the rider lifted onto it.
#include <catch_amalgamated.hpp>

#include "rendering/mount_transition.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <optional>

namespace mt = wowee::rendering::mount_transition;

namespace {

mt::SetupInput input() {
    mt::SetupInput in;
    in.riderSequenceMs = 2000;
    in.riderStbMs = 400;   // 0.2
    in.riderSteMs = 1400;  // 0.7
    in.birthStbMs = 200;   // 0.1
    in.birthSteMs = 1200;  // 0.6
    in.mountStbWorld = glm::vec3(0.0f, -4.0f, 0.0f);
    in.riderTranslation = glm::vec3(0.0f, 0.0f, 0.0f);
    in.riderFacing = 0.5f;
    return in;
}

auto noGround = [](const glm::vec3&, const glm::vec3&) -> std::optional<mt::GroundHit> { return std::nullopt; };

}  // namespace

TEST_CASE("the transition is set up in the rider's sequence's units (0x007fa6a0)", "[mount_transition]") {
    mt::State s = mt::begin(1000, glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(mt::setup(s, input()));
    CHECK(s.flags == mt::kSetUp);
    CHECK(s.riderStb == Catch::Approx(0.2f));
    CHECK(s.riderSte == Catch::Approx(0.7f));
    CHECK(s.invRiderSpan == Catch::Approx(2.0f));
    CHECK(s.birthStb == Catch::Approx(0.1f));
    CHECK(s.birthSte == Catch::Approx(0.6f));
    CHECK(s.invBirthSpan == Catch::Approx(2.0f));
    CHECK(s.travel.y == Catch::Approx(4.0f));
    CHECK(s.facing == Catch::Approx(0.5f));

    // A Birth with no span is tried again.
    mt::SetupInput flat = input();
    flat.birthSteMs = flat.birthStbMs;
    mt::State t = mt::begin(0, glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_FALSE(mt::setup(t, flat));
    CHECK((t.flags & mt::kSetUp) == 0);
}

TEST_CASE("the mount is carried through its Birth and the rider after its $STB (0x007fb7f0)", "[mount_transition]") {
    mt::State s = mt::begin(0, glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(mt::setup(s, input()));
    const glm::vec3 rider(0.0f, 0.0f, 0.0f);
    // Before either: nothing moves.
    mt::step(s, 100, 1.0f, rider, glm::vec3(0.0f, 0.0f, 1.0f), noGround);
    CHECK(s.riderProgress == 0.0f);
    CHECK(s.position == glm::vec3(0.0f));
    CHECK(s.facing == Catch::Approx(0.5f));
    // Halfway through the Birth: halfway there; no ground, no fade.
    mt::step(s, 700, 1.0f, rider, glm::vec3(0.0f, 0.0f, 1.0f), noGround);
    CHECK(s.position.y == Catch::Approx(-2.0f));
    CHECK(s.fade == 0.0f);
    // The rider past its $STB: its progress and facing.
    CHECK(s.riderProgress == Catch::Approx(0.3f));
    CHECK(s.facing == Catch::Approx(1.0f));
    // Past its $STE: arrived, held at 1.
    mt::step(s, 1500, 1.0f, rider, glm::vec3(0.0f, 0.0f, 1.0f), noGround);
    CHECK(s.riderProgress == 1.0f);
    CHECK((s.flags & mt::kArrived) != 0);
    // Past the Birth's $STE: at the rider.
    mt::step(s, 1600, 1.0f, glm::vec3(3.0f, 2.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f), noGround);
    CHECK(s.position == glm::vec3(3.0f, 2.0f, 1.0f));
}

TEST_CASE("the mount finds the ground, eases to its normal and fades in", "[mount_transition]") {
    mt::State s = mt::begin(0, glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(mt::setup(s, input()));
    // The ground a yard below, tilted.
    const glm::vec3 slope = glm::normalize(glm::vec3(0.5f, 0.0f, 1.0f));
    auto ground = [&](const glm::vec3& top, const glm::vec3& bottom) -> std::optional<mt::GroundHit> {
        const float hitZ = -1.0f;
        if (hitZ > top.z || hitZ < bottom.z) return std::nullopt;
        return mt::GroundHit{(top.z - hitZ) / (top.z - bottom.z), slope};
    };
    mt::step(s, 300, 0.0f, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), ground);
    CHECK((s.flags & mt::kTouchedGround) != 0);
    CHECK(s.position.z == Catch::Approx(-1.0f));
    CHECK(s.groundOffset == Catch::Approx(-1.0f));
    // A quarter of the way to the slope's normal.
    const glm::vec3 eased = glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f) + (slope - glm::vec3(0.0f, 0.0f, 1.0f)) * 0.25f);
    CHECK(s.normal.x == Catch::Approx(eased.x));
    CHECK(s.normal.z == Catch::Approx(eased.z));
    // Faded in over half a second: the step's 0.3 s twice over.
    CHECK(s.fade == Catch::Approx(0.6f));
    mt::step(s, 600, 0.0f, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), ground);
    CHECK(s.fade == 1.0f);
}

TEST_CASE("the rider is lifted toward the seat by its progress (0x007fa930)", "[mount_transition]") {
    mt::State s;
    s.riderProgress = 0.25f;
    const glm::vec3 lift = mt::riderLift(s, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, 1.0f));
    CHECK(lift == glm::vec3(0.0f, 0.0f, 0.5f));
}

TEST_CASE("the mount faces as the rider and leans with the ground (0x0071fbf0)", "[mount_transition]") {
    mt::State s;
    s.facing = 1.2f;
    s.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 r = mt::mountRotation(s);
    CHECK(r.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(r.y == Catch::Approx(0.0f).margin(1e-6));
    CHECK(r.z == Catch::Approx(1.2f));
    // Facing along x, the ground rising to +x: the mount's up leans back,
    // a roll about its own y.
    s.facing = 0.0f;
    s.normal = glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f));
    r = mt::mountRotation(s);
    CHECK(r.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(std::sin(r.y) == Catch::Approx(s.normal.x));
}

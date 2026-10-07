// Spell missiles: the arithmetic of 3.3.5a's CMissile, apart from the renderer.
//
// A Frostbolt is a model that leaves the caster's hand, chases the target at
// the spell's speed and lands on its chest, where the impact plays. Each of
// those is a small rule read out of the client (see spell_missile.hpp for the
// functions); these pin them, so a missile that stops homing, flies sideways
// or aims at the wrong bone shows up here rather than in a raid.
#include <catch_amalgamated.hpp>

#include "rendering/placement_transform.hpp"
#include "rendering/spell_missile.hpp"

#include <glm/glm.hpp>

using namespace wowee::rendering;
namespace sm = wowee::rendering::spell_missile;

TEST_CASE("missile attachments map through the client's table", "[spell_missile]") {
    // 0x00ADAA20: head, chest, base, left and right spell hand, ...
    CHECK(sm::m2AttachmentFor(0, 0) == 20);   // Head
    CHECK(sm::m2AttachmentFor(1, 0) == 34);   // Chest
    CHECK(sm::m2AttachmentFor(2, 0) == 19);   // Base
    CHECK(sm::m2AttachmentFor(3, 0) == 21);   // SpellLeftHand
    CHECK(sm::m2AttachmentFor(4, 0) == 22);   // SpellRightHand
    CHECK(sm::m2AttachmentFor(12, 0) == 38);  // SpellHandDirected
    // -1 leaves it to the default, and an index past the table is no
    // attachment rather than a read beyond it.
    CHECK(sm::m2AttachmentFor(-1, 0) == -1);
    CHECK(sm::m2AttachmentFor(16, 0) == -1);
    // Flag 0x200: the column is an M2 attachment id already.
    CHECK(sm::m2AttachmentFor(1, sm::kFlagRawAttachmentIds) == 1);
    CHECK(sm::m2AttachmentFor(-1, sm::kFlagRawAttachmentIds) == -1);
}

TEST_CASE("missile offsets are stored with y mirrored", "[spell_missile]") {
    CHECK(sm::attachmentOffset(glm::vec3(1.0f, 2.0f, 3.0f)) == glm::vec3(1.0f, -2.0f, 3.0f));
}

TEST_CASE("a cast at a unit sends a missile to each target", "[spell_missile]") {
    CHECK(sm::chooseTargeting(sm::kTargetFlagUnit, 1, 0) == sm::Targeting::EachTarget);
    CHECK(sm::chooseTargeting(sm::kTargetFlagGameObject, 2, 0) == sm::Targeting::EachTarget);
    // Nothing to fly at.
    CHECK(sm::chooseTargeting(sm::kTargetFlagUnit, 0, 0) == sm::Targeting::None);
    CHECK(sm::chooseTargeting(0, 3, 0) == sm::Targeting::None);
}

TEST_CASE("a cast at the ground sends one missile to the point", "[spell_missile]") {
    CHECK(sm::chooseTargeting(sm::kTargetFlagDestLocation, 4, 0) == sm::Targeting::Location);
    CHECK(sm::chooseTargeting(sm::kTargetFlagDestLocation, 0, 0) == sm::Targeting::Location);
    // Flag 0x100: not when nothing was hit.
    CHECK(sm::chooseTargeting(sm::kTargetFlagDestLocation, 0,
                              sm::kFlagNoLocationMissileWithoutTargets) == sm::Targeting::None);
    // A unit and a point: the point wins unless the visual asks for flag 0x1.
    const uint32_t both = sm::kTargetFlagUnit | sm::kTargetFlagDestLocation;
    CHECK(sm::chooseTargeting(both, 1, 0) == sm::Targeting::Location);
    CHECK(sm::chooseTargeting(both, 1, sm::kFlagMissilePerTargetAtLocation) ==
          sm::Targeting::EachTarget);
}

TEST_CASE("flight time is distance over speed, and no speed is no flight", "[spell_missile]") {
    CHECK(sm::flightTime(30.0f, 20.0f) == Catch::Approx(1.5f));
    CHECK(sm::flightTime(30.0f, 0.0f) == 0.0f);
    CHECK(sm::flightTime(-1.0f, 10.0f) == 0.0f);
}

TEST_CASE("a missile covers speed times the frame toward the target", "[spell_missile]") {
    const auto step = sm::advance(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f), 20.0f, 0.1f);
    CHECK_FALSE(step.arrived);
    CHECK(step.position.x == Catch::Approx(2.0f));
    CHECK(step.position.y == Catch::Approx(0.0f));
}

TEST_CASE("a missile lands when the step would reach the target", "[spell_missile]") {
    const glm::vec3 target(3.0f, 4.0f, 0.0f);  // five yards away
    const auto step = sm::advance(glm::vec3(0.0f), target, 20.0f, 0.25f);
    CHECK(step.arrived);
    CHECK(step.position == target);
    // A missile with no speed never gets there on its own.
    CHECK_FALSE(sm::advance(glm::vec3(0.0f), target, 0.0f, 1.0f).arrived);
}

TEST_CASE("a missile homes on a target that moves", "[spell_missile]") {
    // The target runs sideways; every frame the missile turns to where it is
    // now, and it still gets there, sooner than the flight time doubled.
    glm::vec3 missile(0.0f);
    glm::vec3 target(20.0f, 0.0f, 0.0f);
    const float speed = 24.0f, dt = 1.0f / 60.0f;
    bool arrived = false;
    int frames = 0;
    while (!arrived && frames < 600) {
        target.y += 7.0f * dt;  // a running unit
        const auto step = sm::advance(missile, target, speed, dt);
        missile = step.position;
        arrived = step.arrived;
        ++frames;
    }
    REQUIRE(arrived);
    CHECK(missile == target);
    CHECK(frames * dt < 2.0f * sm::flightTime(20.0f, speed));
}

TEST_CASE("a missile faces along its travel", "[spell_missile]") {
    const glm::vec3 directions[] = {
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f},
        {3.0f, -2.0f, 1.5f}, {0.0f, 0.0f, -1.0f},
    };
    for (const glm::vec3& d : directions) {
        const glm::mat4 m = placementModelMatrix(glm::vec3(0.0f), sm::facingEuler(d), 1.0f);
        const glm::vec3 forward = glm::vec3(m * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        const glm::vec3 want = glm::normalize(d);
        CHECK(forward.x == Catch::Approx(want.x).margin(1e-5));
        CHECK(forward.y == Catch::Approx(want.y).margin(1e-5));
        CHECK(forward.z == Catch::Approx(want.z).margin(1e-5));
    }
    CHECK(sm::facingEuler(glm::vec3(0.0f)) == glm::vec3(0.0f));
}

TEST_CASE("a missile's sound is its SoundEntries row's first file", "[spell_missile][sound]") {
    std::array<std::string, 10> files{};
    CHECK(sm::soundEntryFile("Sound\\Spells", files).empty());
    files[2] = "FireBallMissileLoop.wav";
    files[5] = "Other.wav";
    CHECK(sm::soundEntryFile("Sound\\Spells", files) == "Sound\\Spells\\FireBallMissileLoop.wav");
    CHECK(sm::soundEntryFile("", files) == "FireBallMissileLoop.wav");
    // 0x00703410 fades it out over 0.15 s on arrival.
    CHECK(sm::kMissileSoundFadeSeconds == Catch::Approx(0.15f));
}

// ADJUST_MISSILE (cast flag 0x20000): the arc 0x00700880 plans and 0x007015d0
// flies. A catapult's boulder leaves at the elevation the server names and
// lands when the server says, wherever the target has gone since.
namespace {
constexpr float kPi = 3.14159265f;
}

TEST_CASE("an arc solved at its pitch lands on the target", "[spell_missile][arc]") {
    const glm::vec3 delta(30.0f, -40.0f, 5.0f);
    float speed = 0.0f, time = 0.0f;
    glm::vec3 velocity(0.0f);
    REQUIRE(sm::solveArcAtPitch(kPi / 4.0f, delta, sm::kDefaultMissileGravity, speed, time, velocity) ==
            sm::ArcSolve::Solved);
    // Leaves at the pitch: rise over run is tan(45 degrees).
    CHECK(velocity.z == Catch::Approx(std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y)).epsilon(1e-4));
    CHECK(glm::length(velocity) == Catch::Approx(speed).epsilon(1e-4));
    // Flown for its own time with no scaling, it is on the target.
    const sm::MissileArc arc = sm::planMissileArc(kPi / 4.0f, 0.0f, delta, nullptr, time);
    REQUIRE(arc.onArc);
    CHECK(arc.gravity == Catch::Approx(19.29f));
    CHECK(arc.timeScale == Catch::Approx(1.0f).epsilon(1e-4));
    const glm::vec3 start(100.0f, 200.0f, 10.0f);
    const glm::vec3 landed = sm::arcPosition(start, arc, time);
    CHECK(landed.x == Catch::Approx(start.x + delta.x).margin(1e-2));
    CHECK(landed.y == Catch::Approx(start.y + delta.y).margin(1e-2));
    CHECK(landed.z == Catch::Approx(start.z + delta.z).margin(1e-2));
    // Half way it is above the straight line between the ends.
    CHECK(sm::arcPosition(start, arc, time * 0.5f).z > start.z + delta.z * 0.5f);
}

TEST_CASE("the arc is timed to the server's delay", "[spell_missile][arc]") {
    const glm::vec3 delta(20.0f, 0.0f, 0.0f);
    float speed = 0.0f, time = 0.0f;
    glm::vec3 velocity(0.0f);
    REQUIRE(sm::solveArcAtPitch(0.6f, delta, 19.29f, speed, time, velocity) == sm::ArcSolve::Solved);
    // Twice the arc's own time: flown at half speed, still landing on it.
    const sm::MissileArc slow = sm::planMissileArc(0.6f, 0.0f, delta, nullptr, time * 2.0f);
    CHECK(slow.timeScale == Catch::Approx(0.5f).epsilon(1e-4));
    const glm::vec3 landed = sm::arcPosition(glm::vec3(0.0f), slow, time * 2.0f);
    CHECK(landed.x == Catch::Approx(20.0f).margin(1e-2));
    CHECK(landed.z == Catch::Approx(0.0f).margin(1e-2));
    // The rate is held to [0, 2]: a delay of nothing flies it at twice its time.
    CHECK(sm::planMissileArc(0.6f, 0.0f, delta, nullptr, 0.0f).timeScale == 2.0f);
}

TEST_CASE("the row's gravity shapes the arc", "[spell_missile][arc]") {
    sm::SpellMissileRow row;
    row.gravity = 5.0f;
    const glm::vec3 delta(10.0f, 0.0f, 0.0f);
    const sm::MissileArc light = sm::planMissileArc(0.5f, 0.0f, delta, &row, 1.0f);
    const sm::MissileArc heavy = sm::planMissileArc(0.5f, 0.0f, delta, nullptr, 1.0f);
    REQUIRE(light.onArc);
    REQUIRE(heavy.onArc);
    CHECK(light.gravity == 5.0f);
    // Less gravity at the same pitch: a slower, longer flight.
    CHECK(glm::length(light.velocity) < glm::length(heavy.velocity));
    CHECK(light.timeScale > heavy.timeScale);
}

TEST_CASE("a target above the pitch's line flies straight instead", "[spell_missile][arc]") {
    // Level launch at a target 5 yards up: no arc reaches it, nor at a
    // hundredth of a radian more, so the missile homes at the speed that
    // lands it when the server says.
    const glm::vec3 delta(10.0f, 0.0f, 5.0f);
    float speed = 0.0f, time = 0.0f;
    glm::vec3 velocity(0.0f);
    CHECK(sm::solveArcAtPitch(0.0f, delta, 19.29f, speed, time, velocity) == sm::ArcSolve::NoSolution);
    const sm::MissileArc arc = sm::planMissileArc(0.0f, 0.0f, delta, nullptr, 2.0f);
    CHECK_FALSE(arc.onArc);
    CHECK(arc.straightSpeed == Catch::Approx(glm::length(delta) / 2.0f));
}

TEST_CASE("a target straight below is timed by the fall", "[spell_missile][arc]") {
    const glm::vec3 delta(0.0f, 0.0f, -10.0f);
    float speed = 0.0f, time = 0.0f;
    glm::vec3 velocity(0.0f);
    CHECK(sm::solveArcAtPitch(0.3f, delta, 19.29f, speed, time, velocity) == sm::ArcSolve::Degenerate);
    // No speed of its own: the time to drop 10 yards.
    CHECK(sm::arcTimeAtSpeed(0.0f, 0.0f, delta, 19.29f) == Catch::Approx(std::sqrt(20.0f / 19.29f)));
    // Without a SpellMissile row there is no arc.
    CHECK_FALSE(sm::planMissileArc(0.0f, 0.0f, delta, nullptr, 1.0f).onArc);
    sm::SpellMissileRow row;
    const sm::MissileArc arc = sm::planMissileArc(0.0f, 0.0f, delta, &row, 1.0f);
    REQUIRE(arc.onArc);
    CHECK(arc.velocity.z < 0.0f);
}

TEST_CASE("an elevation past vertical folds back", "[spell_missile][arc]") {
    CHECK(sm::foldElevation(0.3f) == 0.3f);
    CHECK(sm::foldElevation(2.0f) == Catch::Approx(kPi - 2.0f).epsilon(1e-5));
    CHECK(sm::foldElevation(-2.0f) == Catch::Approx(-(kPi - 2.0f)).epsilon(1e-5));
}

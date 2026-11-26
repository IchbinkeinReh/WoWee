// Which transport the player is on, from the ground under its feet, as the
// client decides it after a step (0x006ec7b0 -> 0x006ec400).
#include <catch_amalgamated.hpp>

#include "game/car_ride.hpp"
#include "game/transport_boarding.hpp"

using namespace wowee::game::transport_boarding;

namespace {
constexpr uint64_t kLift = 0xF110000000000ABCull;
constexpr uint64_t kOtherLift = 0xF110000000000ABDull;
constexpr uint64_t kCrate = 0xF110000000000123ull;
}  // namespace

TEST_CASE("only an object flagged a transport is one (0x00712f20)", "[transport_boarding]") {
    CHECK(isTransportObject(0x8));
    CHECK(isTransportObject(0x20 | 0x8));  // a Deeprun tram car's flags
    CHECK_FALSE(isTransportObject(0x20));
    CHECK_FALSE(isTransportObject(0));
}

TEST_CASE("standing on a transport's floor puts the mover on it", "[transport_boarding]") {
    CHECK(afterGroundStep(0, kLift, true, false) == kLift);
    // From one straight onto another.
    CHECK(afterGroundStep(kLift, kOtherLift, true, false) == kOtherLift);
    // An object that is not a transport changes nothing, either way.
    CHECK(afterGroundStep(0, kCrate, false, false) == 0);
    CHECK(afterGroundStep(kLift, kCrate, false, false) == kLift);
}

TEST_CASE("the world's ground takes the mover off unless the transport holds it", "[transport_boarding]") {
    CHECK(afterGroundStep(kLift, 0, false, false) == 0);
    CHECK(afterGroundStep(kLift, 0, false, true) == kLift);
    CHECK(afterGroundStep(0, 0, false, true) == 0);
}

TEST_CASE("an M2 transport holds a passenger inside its collision box (0x0077ffb0)", "[transport_boarding]") {
    const glm::vec3 boxMin(-5.0f, -4.0f, 0.0f);
    const glm::vec3 boxMax(5.0f, 4.0f, 3.0f);
    CHECK(insideCollisionBox(glm::vec3(0.0f, 0.0f, 0.5f), boxMin, boxMax));
    CHECK(insideCollisionBox(glm::vec3(5.0f, -4.0f, 0.0f), boxMin, boxMax));
    // A head's height over the top still counts: someone standing on it.
    CHECK(insideCollisionBox(glm::vec3(0.0f, 0.0f, 4.6f), boxMin, boxMax));
    CHECK_FALSE(insideCollisionBox(glm::vec3(0.0f, 0.0f, 4.7f), boxMin, boxMax));
    CHECK_FALSE(insideCollisionBox(glm::vec3(5.1f, 0.0f, 1.0f), boxMin, boxMax));
    CHECK_FALSE(insideCollisionBox(glm::vec3(0.0f, -4.1f, 1.0f), boxMin, boxMax));
    CHECK_FALSE(insideCollisionBox(glm::vec3(0.0f, 0.0f, -0.1f), boxMin, boxMax));
}

TEST_CASE("a fall keeps the transport only while it holds the mover (0x007618b0, 0x0074b5e0)",
          "[transport_boarding]") {
    CHECK(afterFallStep(kLift, true) == kLift);
    CHECK(afterFallStep(kLift, false) == 0);
    CHECK(afterFallStep(0, true) == 0);
    CHECK(afterFallStep(0, false) == 0);
}

TEST_CASE("a WMO transport holds a passenger inside its MCVP volume (0x007aea10)", "[transport_boarding]") {
    // A box 10 x 6 x 4 about the origin as six planes, outward normals:
    // inside where normal . p + d <= 0.
    const std::vector<glm::vec4> box = {
        {1, 0, 0, -5}, {-1, 0, 0, -5}, {0, 1, 0, -3}, {0, -1, 0, -3}, {0, 0, 1, -4}, {0, 0, -1, 0}};
    CHECK(insideConvexVolume(glm::vec3(0.0f, 0.0f, 1.0f), box));
    CHECK(insideConvexVolume(glm::vec3(5.0f, 3.0f, 4.0f), box));   // on the faces: inside
    CHECK_FALSE(insideConvexVolume(glm::vec3(5.1f, 0.0f, 1.0f), box));   // over the rail
    CHECK_FALSE(insideConvexVolume(glm::vec3(0.0f, -3.2f, 1.0f), box));
    CHECK_FALSE(insideConvexVolume(glm::vec3(0.0f, 0.0f, -0.5f), box));   // under the hull
    CHECK_FALSE(insideConvexVolume(glm::vec3(0.0f, 0.0f, 4.5f), box));    // above its top
    // No planes: holds everyone.
    CHECK(insideConvexVolume(glm::vec3(100.0f, 0.0f, 0.0f), {}));
}

// The Deeprun Tram's car was no obstacle and its riders' height was held: they
// stood 0.7 yards in its deck and could not jump.
TEST_CASE("a rider's own step on a car is kept, up its steps too", "[car_ride]") {
    using namespace wowee::game::car_ride;
    // SubwayCar.m2 at 7.01: the step along its side and the deck.
    const auto floorAt = [](const glm::vec3& at) -> std::optional<float> {
        const float floor = std::abs(at.x) < 4.52f ? -3.46f : -4.14f;
        return floor <= at.z + 1.0f ? std::optional<float>(floor) : std::nullopt;
    };
    Step step;
    step.carried = {4.9f, 0.0f, -4.14f};
    step.moves = true;
    // Part of the way over the edge of the deck.
    step.own = {-0.1f, 0.0f, 0.3f};
    Place at = place(step, floorAt);
    CHECK(at.at.z == Catch::Approx(-3.84f));
    CHECK_FALSE(at.onFloor);
    // It stops there: nothing moves it.
    step.carried = at.at;
    step.moves = false;
    step.own = {0.0f, 0.0f, -0.05f};
    CHECK(place(step, floorAt).at == step.carried);
    // On the deck.
    step.carried = {4.6f, 0.0f, -3.8f};
    step.moves = true;
    step.own = {-0.2f, 0.0f, 0.2f};
    at = place(step, floorAt);
    CHECK(at.at.z == Catch::Approx(-3.46f));
    CHECK(at.onFloor);
}

TEST_CASE("a rider jumps on a car and lands on its floor", "[car_ride]") {
    using namespace wowee::game::car_ride;
    const auto floorAt = [](const glm::vec3&) { return std::optional<float>(-3.46f); };
    Step step;
    step.carried = {0.0f, 0.0f, -3.46f};
    step.moves = step.rising = true;
    step.grounded = false;
    step.own = {0.0f, 0.0f, 0.2f};
    CHECK(place(step, floorAt).at.z == Catch::Approx(-3.26f));
    // Coming down, through the floor in a slow frame.
    step.rising = false;
    step.carried.z = -3.4f;
    step.own.z = -0.3f;
    const Place landed = place(step, floorAt);
    CHECK(landed.at.z == Catch::Approx(-3.46f));
    CHECK(landed.onFloor);
}

TEST_CASE("boarding a car a step above its floor puts the rider on it", "[car_ride]") {
    using namespace wowee::game::car_ride;
    const auto floorAt = [](const glm::vec3&) { return std::optional<float>(10.0f); };
    Step step;
    step.carried = {0.0f, 0.0f, 10.6f};
    step.boarding = true;
    CHECK(place(step, floorAt).at.z == Catch::Approx(10.0f));
    // Not further, and not in the air.
    step.carried.z = 11.5f;
    CHECK(place(step, floorAt).at.z == Catch::Approx(11.5f));
    step.carried.z = 10.6f;
    step.grounded = false;
    CHECK(place(step, floorAt).at.z == Catch::Approx(10.6f));
}

// T323: a relog onto the Deeprun Tram's car while it set off during the load
// put the rider 2.75 yards along the car and 0.4 higher than the realm had
// them. The place the last session had put the rider on the car was kept, and
// the first step in the new world measured the rider's own walk from it.
TEST_CASE("a ride's place from the world left is no step in the next (0x00403b70)", "[car_ride]") {
    using namespace wowee::game::car_ride;
    const auto floorAt = [](const glm::vec3&) { return std::optional<float>(-11.59f); };
    Lock lock;
    // Where the last session put the rider, the car standing at the station.
    lock.put({-8000.0f, 1100.0f, -11.59f});
    lock.worldEntered();
    CHECK_FALSE(lock.held());
    // The new world: the car has moved on, the load left the character pushed
    // along by its wall and off its floor; the server's offset carries it.
    const glm::vec3 carried{-8010.0f, 1100.0f, -11.59f};
    const glm::vec3 walked{-8002.75f, 1100.0f, -11.15f};
    const Step step = lock.step(carried, walked, false, false, false);
    CHECK(step.boarding);
    CHECK(step.own == glm::vec3(0.0f));
    CHECK(place(step, floorAt).at == carried);
    // In one world, a rider put last frame walks from there.
    lock.put(carried);
    const Step next = lock.step(carried, carried + glm::vec3(0.5f, 0.0f, 0.0f), true, false, true);
    CHECK_FALSE(next.boarding);
    CHECK(next.own.x == Catch::Approx(0.5f));
    CHECK(place(next, floorAt).at.x == Catch::Approx(carried.x + 0.5f));
}

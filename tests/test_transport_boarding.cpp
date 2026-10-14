// Which transport the player is on, from the ground under its feet, as the
// client decides it after a step (0x006ec7b0 -> 0x006ec400).
#include <catch_amalgamated.hpp>

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

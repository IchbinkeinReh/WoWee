#include <catch_amalgamated.hpp>
#include "rendering/movement_limits.hpp"
#include "core/coordinates.hpp"

#include <cmath>

TEST_CASE("the step a player climbs is one yard") {
    using namespace wowee::rendering::movement;
    // The client's step for a player at normal size (0x006e9570 sets +0xd0
    // to scale / display scale; 0x00761b00 climbs by it).
    REQUIRE(kMaxStepUp == 1.0f);
    REQUIRE(kMinWalkableNormalZ == Catch::Approx(std::cos(50.0f * 3.14159265f / 180.0f)));
}

// Facing crosses two representations: the renderer holds the character's yaw in
// degrees, the game side holds canonical yaw in radians, and the frame loop
// converts render → game every frame. Both directions of that conversion were
// hand-written at four call sites, two of them inverting the other two from
// memory. If they ever disagree, facing a target writes one value and the next
// frame reads back another - which is how a cast could be accepted and then
// fail the server's arc check a second and a half later.
TEST_CASE("character yaw and canonical yaw convert back to each other") {
    using namespace wowee::core::coords;

    for (float deg = -720.0f; deg <= 720.0f; deg += 7.5f) {
        const float canonical = characterYawDegToCanonical(deg);
        const float back = canonicalToCharacterYawDeg(canonical);
        // Round-trips to the same heading, allowing for full turns.
        float delta = std::fmod(std::fabs(back - deg), 360.0f);
        if (delta > 180.0f) delta = 360.0f - delta;
        INFO("degrees: " << deg << " canonical: " << canonical << " back: " << back);
        CHECK(delta < 0.01f);
        CHECK(canonical >= -PI - 0.001f);
        CHECK(canonical <= PI + 0.001f);
    }

    // Render yaw is canonical + 90: canonicalToRender swaps x and y, and
    // canonical yaw is atan2(-dy, dx), so a heading of canonical 0 (north) has
    // render components (0, 1) and a render yaw of 90.
    CHECK(canonicalToCharacterYawDeg(0.0f) == Catch::Approx(90.0f).margin(1e-4));
    CHECK(characterYawDegToCanonical(90.0f) == Catch::Approx(0.0f).margin(1e-5));

    // And it must agree with taking the angle of the render direction directly,
    // which is how the combat auto-turn and the camera both produce it.
    for (float canon = -3.0f; canon <= 3.0f; canon += 0.25f) {
        const glm::vec3 dirCanonical(std::cos(canon), -std::sin(canon), 0.0f);
        const glm::vec3 dirRender = canonicalToRender(dirCanonical);
        const float renderYawDeg =
            std::atan2(dirRender.y, dirRender.x) * (180.0f / PI);
        float diff = std::fmod(std::fabs(renderYawDeg - canonicalToCharacterYawDeg(canon)), 360.0f);
        if (diff > 180.0f) diff = 360.0f - diff;
        INFO("canonical: " << canon);
        CHECK(diff < 0.01f);
    }
}

TEST_CASE("a teleport lands on the floor at or under the server's height") {
    using wowee::rendering::movement::teleportLandingFloor;
    using wowee::rendering::movement::kTeleportFloorReachUp;
    using wowee::rendering::movement::kTeleportFloorReachDown;
    // The Goldshire inn cellar: .go to z 50.161. Over it are the hall floor
    // (55.99) and the heightfield (56.45); the cellar floor is at 50.16.
    SECTION("a storey or heightfield overhead is not the landing") {
        auto h = teleportLandingFloor({56.45f, 55.99f, std::nullopt}, 50.161f);
        CHECK_FALSE(h.has_value());
        h = teleportLandingFloor({56.45f, 50.16f, std::nullopt}, 50.161f);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(50.16f));
    }
    SECTION("the floor a little under or over the given height is taken") {
        auto h = teleportLandingFloor({std::nullopt, 49.8f, std::nullopt}, 50.161f);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(49.8f));
        h = teleportLandingFloor({51.0f}, 50.161f);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(51.0f));
        CHECK_FALSE(teleportLandingFloor({50.161f + kTeleportFloorReachUp + 0.01f}, 50.161f));
    }
    SECTION("the highest of the floors in reach, so a WMO floor over terrain wins") {
        auto h = teleportLandingFloor({90.0f, 95.3f, 94.0f}, 95.5f);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(95.3f));
    }
    // Darnassus: .tele to z 1341.4 on the temple terrace (1341.39), the
    // terrain 12.5 yards under it (1328.87). Before the terrace is loaded the
    // terrain is all there is, and it is no landing.
    SECTION("the ground far under a building not loaded yet is not the landing") {
        CHECK_FALSE(teleportLandingFloor({1328.87f, std::nullopt, std::nullopt}, 1341.4f));
        auto h = teleportLandingFloor({1328.87f, 1341.39f, std::nullopt}, 1341.4f);
        REQUIRE(h.has_value());
        CHECK(*h == Catch::Approx(1341.39f));
    }
    SECTION("nothing within reach below keeps the server's height") {
        CHECK_FALSE(teleportLandingFloor({100.0f - kTeleportFloorReachDown - 0.5f}, 100.0f));
        CHECK_FALSE(teleportLandingFloor({std::nullopt, std::nullopt}, 100.0f));
    }
}

// Silvermoon's Court of the Sun: the wall push-out carried a character eight
// yards sideways in one frame, past the gold railing at the platform's edge.
TEST_CASE("walls hold a step back but never throw the body past it", "[movement]") {
    using wowee::rendering::movement::holdWithinStep;
    const glm::vec3 from(9919.67f, -7113.83f, 47.85f);
    const glm::vec3 to(9919.43f, -7113.38f, 47.85f);   // a quarter of a yard
    // Slid along a wall, and pushed out a little: kept.
    const glm::vec3 slid(9919.55f, -7113.30f, 47.86f);
    CHECK(holdWithinStep(from, to, slid, 0.5f) == slid);
    // Pushed out of a face by a whole radius on top of the step: kept.
    const glm::vec3 pushed(from.x - 0.7f, from.y, from.z);
    CHECK(holdWithinStep(from, to, pushed, 0.5f) == pushed);
    // The frame that crossed the railing: eight yards away. Stays put.
    const glm::vec3 thrown(9915.01f, -7120.09f, 47.84f);
    const glm::vec3 held = holdWithinStep(from, to, thrown, 0.5f);
    CHECK(held.x == from.x);
    CHECK(held.y == from.y);
    CHECK(held.z == thrown.z);
}


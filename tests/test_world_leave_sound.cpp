// The teleports that unload the world's sound (0x00409550 -> 0x004c85f0):
// its ambience stops at once (AmbientSoundManager::stopAll).
#include <catch_amalgamated.hpp>

#include "core/far_teleport.hpp"

using wowee::core::teleportShowsLoadingScreen;

TEST_CASE("a same-map teleport past 150 yards unloads the world's sound", "[audio]") {
    // 0x006ecf80: the squared distance against 22500, strictly greater.
    const glm::vec3 from(100.0f, 200.0f, 30.0f);
    CHECK_FALSE(teleportShowsLoadingScreen(from, from));
    CHECK_FALSE(teleportShowsLoadingScreen(from, from + glm::vec3(150.0f, 0.0f, 0.0f)));
    CHECK(teleportShowsLoadingScreen(from, from + glm::vec3(150.5f, 0.0f, 0.0f)));
    // Height counts as much as the ground covered.
    CHECK(teleportShowsLoadingScreen(from, from + glm::vec3(0.0f, 0.0f, -151.0f)));
    CHECK(teleportShowsLoadingScreen(from, from + glm::vec3(90.0f, 90.0f, 90.0f)));
}

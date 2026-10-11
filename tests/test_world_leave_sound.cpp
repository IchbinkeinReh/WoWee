// Leaving the world stops its ambience (0x00409550 -> 0x004c85f0).
//
// The ambience loops were fire-and-forget: nothing kept a handle, so a logout
// let a minute-long zone loop play on over the login screen. They are kept
// now, and leaving the world takes every one of them to stop.
#include <catch_amalgamated.hpp>

#include "audio/ambience_loops.hpp"
#include "core/far_teleport.hpp"

using wowee::audio::AmbienceLoops;
using wowee::core::teleportShowsLoadingScreen;

TEST_CASE("leaving the world takes every running ambience loop", "[audio]") {
    AmbienceLoops loops;
    loops.add(7);
    loops.add(0);  // nothing was started
    loops.add(9);
    CHECK(loops.size() == 2);
    const auto stopped = loops.takeAll();
    CHECK(stopped == std::vector<uint32_t>{7, 9});
    // And none is left for the next world.
    CHECK(loops.size() == 0);
    CHECK(loops.takeAll().empty());
}

TEST_CASE("the loops kept are the newest", "[audio]") {
    AmbienceLoops loops;
    for (uint32_t h = 1; h <= AmbienceLoops::kMaxTracked + 3; ++h) loops.add(h);
    const auto kept = loops.takeAll();
    REQUIRE(kept.size() == AmbienceLoops::kMaxTracked);
    CHECK(kept.front() == 4);
    CHECK(kept.back() == AmbienceLoops::kMaxTracked + 3);
}

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

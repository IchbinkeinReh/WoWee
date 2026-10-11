// Leaving the world stops its ambience (0x00409550 -> 0x004c85f0).
//
// The ambience loops were fire-and-forget: nothing kept a handle, so a logout
// let a minute-long zone loop play on over the login screen. They are kept
// now, and leaving the world takes every one of them to stop.
#include <catch_amalgamated.hpp>

#include "audio/ambience_loops.hpp"

using wowee::audio::AmbienceLoops;

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

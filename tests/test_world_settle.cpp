// When the loading screen kept over the world's first frames may go.
#include <catch_amalgamated.hpp>

#include "core/world_settle.hpp"

using namespace wowee::core::world_settle;

TEST_CASE("the loading screen stays until the frames settle", "[world_settle]") {
    CHECK(verdict(0.0f, 0) == nullptr);
    CHECK(verdict(2.0f, kCalmFramesNeeded - 1) == nullptr);
    CHECK(verdict(2.0f, kCalmFramesNeeded) != nullptr);
}

TEST_CASE("a lucky early run of frames is not settled", "[world_settle]") {
    CHECK(verdict(kMinSeconds * 0.5f, kCalmFramesNeeded * 3) == nullptr);
}

TEST_CASE("the screen goes at the cap whatever the frames do", "[world_settle]") {
    CHECK(verdict(kMaxSeconds, 0) != nullptr);
    CHECK(verdict(kMaxSeconds - 0.01f, 0) == nullptr);
}

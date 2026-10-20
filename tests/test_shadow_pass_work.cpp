// What the shadow pass does with dynamic shadows on and off
// (rendering/shadow_pass_work.hpp). Off used to draw every cascade and its
// casters anyway, for a map no shader read: 3 to 13 ms a frame on a phone.
#include <catch_amalgamated.hpp>

#include "rendering/shadow_pass_work.hpp"

using wowee::rendering::ShadowPassWork;
using wowee::rendering::shadowPassWork;

TEST_CASE("shadows on draw the casters whatever the slot's layout", "[shadow_pass]") {
    CHECK(shadowPassWork(true, true, true) == ShadowPassWork::DrawCasters);
    CHECK(shadowPassWork(true, true, false) == ShadowPassWork::DrawCasters);
}

TEST_CASE("shadows off draw no casters", "[shadow_pass]") {
    for (bool readable : {false, true}) {
        CHECK(shadowPassWork(true, false, readable) != ShadowPassWork::DrawCasters);
    }
}

TEST_CASE("shadows off clear a slot once, then skip the pass", "[shadow_pass]") {
    // Never drawn or just rebuilt: cleared so it can be bound for sampling.
    CHECK(shadowPassWork(true, false, false) == ShadowPassWork::ClearOnly);
    // Readable already: nothing at all is recorded.
    CHECK(shadowPassWork(true, false, true) == ShadowPassWork::None);
}

TEST_CASE("a map that cannot be drawn this frame records nothing", "[shadow_pass]") {
    for (bool on : {false, true}) {
        for (bool readable : {false, true}) {
            CHECK(shadowPassWork(false, on, readable) == ShadowPassWork::None);
        }
    }
}

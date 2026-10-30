// IsFalling() (0x00612430): FALLING without ROOT.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"

using wowee::game::isFallingForInterface;

TEST_CASE("IsFalling: the falling flag, not falling-far, and not while rooted", "[movement]") {
    CHECK(isFallingForInterface(0x1000));            // a jump, at once
    CHECK(isFallingForInterface(0x1000 | 0x2000));   // a long fall
    CHECK(isFallingForInterface(0x1000 | 0x1));      // jumping forward
    CHECK_FALSE(isFallingForInterface(0x2000));      // FALLINGFAR alone
    CHECK_FALSE(isFallingForInterface(0x1000 | 0x800));  // rooted
    CHECK_FALSE(isFallingForInterface(0));
    CHECK_FALSE(isFallingForInterface(0x200000));    // swimming
}

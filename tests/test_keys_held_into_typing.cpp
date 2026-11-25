// A key held as a line is begun (0x004943c0, 0x00494490).
#include <catch_amalgamated.hpp>

#include "rendering/keys_held_into_typing.hpp"

using wowee::rendering::KeysHeldIntoTyping;

TEST_CASE("a key held before a line is typed is the game's until it is let go", "[input]") {
    KeysHeldIntoTyping keys;
    CHECK(keys.down(0, true, false));
    CHECK(keys.down(0, true, true));
    CHECK(keys.down(0, true, true));
    // Let go of and pressed again: the line's.
    CHECK_FALSE(keys.down(0, false, true));
    CHECK_FALSE(keys.down(0, true, true));
    // Not typed any more, and still down: whether the line took its press is told elsewhere
    // (Input::setKeySpent).
    CHECK(keys.down(0, true, false));
    // One pressed as it is typed is none of the game's.
    CHECK_FALSE(keys.down(1, false, false));
    CHECK_FALSE(keys.down(1, true, true));
    CHECK(keys.down(0, true, true));
}

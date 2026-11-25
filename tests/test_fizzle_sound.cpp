// The fizzle of a spell that failed (game/fizzle_sound.hpp).
#include <catch_amalgamated.hpp>

#include "game/fizzle_sound.hpp"

using namespace wowee::game;

// 0x004cff30, Resistances.dbc
TEST_CASE("a spell fizzles by the first school of its mask with a row", "[fizzle]") {
    const ResistanceSounds sounds{0u, 1428u, 1424u, 1426u, 1425u, 1427u, 1428u};
    CHECK(fizzleSound(0x20, sounds) == 1427);
    CHECK(fizzleSound(0x04, sounds) == 1424);
    CHECK(fizzleSound(0x14, sounds) == 1424);
    CHECK(fizzleSound(0x01, sounds) == 0);
    CHECK(fizzleSound(0x21, sounds) == 0);
    CHECK(fizzleSound(0x00, sounds) == 0);
    ResistanceSounds without = sounds;
    without[0].reset();
    CHECK(fizzleSound(0x21, without) == 1427);
}

// 0x00808200
TEST_CASE("every failure but one fizzles", "[fizzle]") {
    CHECK(castFailureFizzles(0x0b));
    CHECK_FALSE(castFailureFizzles(0x1b));
}

// The world's sound behind a loading screen (audio/world_sound_hold.hpp):
// the client steps nothing of the world while its screen is up (0x0040a920),
// so nothing of it starts a sound until the screen goes.
#include <catch_amalgamated.hpp>

#include "audio/world_sound_hold.hpp"

namespace hold = wowee::audio::world_sound_hold;

TEST_CASE("with no loading screen up a sound plays", "[audio]") {
    CHECK(hold::start(false, false) == hold::Start::Now);
    CHECK(hold::start(false, true) == hold::Start::Now);
}

TEST_CASE("behind a loading screen a one-shot is not played and a loop waits", "[audio]") {
    // A footstep, an NPC's greeting, a spell's impact: their moment is gone
    // by the time the world is shown.
    CHECK(hold::start(true, false) == hold::Start::Refused);
    // A doodad's fire, the ambience, a stream: the world's first step would
    // start them, so they start when the screen goes.
    CHECK(hold::start(true, true) == hold::Start::Deferred);
}

TEST_CASE("the hold ends with the last loading screen", "[audio]") {
    hold::Count count;
    CHECK_FALSE(count.held());
    count.begin();  // the world's load
    count.begin();  // a difficulty change's screen over it
    CHECK(count.held());
    CHECK_FALSE(count.end());  // one still up
    CHECK(count.held());
    CHECK(count.end());  // the last: what waited starts
    CHECK_FALSE(count.held());
    // A screen let go twice does not let go of another's hold.
    CHECK_FALSE(count.end());
    CHECK_FALSE(count.held());
    count.begin();
    CHECK(count.held());
}

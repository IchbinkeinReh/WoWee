// How far a 3D sound is heard (audio/sound_distance.hpp): to its
// SoundEntries DistanceCutoff and no further.
#include <catch_amalgamated.hpp>

#include "audio/sound_distance.hpp"

namespace sound_distance = wowee::audio::sound_distance;
using sound_distance::heard;

TEST_CASE("a 3D sound is heard out to its cutoff", "[audio]") {
    const glm::vec3 listener(-9455.0f, 20.0f, 66.0f);
    // A torch's 40 yards: at it, and not a step past it.
    CHECK(heard(listener, listener, 40.0f));
    CHECK(heard(listener + glm::vec3(40.0f, 0.0f, 0.0f), listener, 40.0f));
    CHECK_FALSE(heard(listener + glm::vec3(40.5f, 0.0f, 0.0f), listener, 40.0f));
    // Height counts as much as the ground between.
    CHECK_FALSE(heard(listener + glm::vec3(30.0f, 0.0f, 30.0f), listener, 40.0f));
    // A camp fire in the Burning Steppes, heard from Goldshire before.
    CHECK_FALSE(heard(glm::vec3(-7501.0f, -1331.9f, 303.5f), listener, 40.0f));
}

TEST_CASE("a sound with no cutoff is heard at any distance", "[audio]") {
    const glm::vec3 listener(0.0f);
    CHECK(heard(glm::vec3(5000.0f, 0.0f, 0.0f), listener, 0.0f));
    CHECK(heard(glm::vec3(5000.0f, 0.0f, 0.0f), listener, -1.0f));
}

TEST_CASE("a SoundEntries row says how far its sound carries", "[audio]") {
    float min = 0.0f, max = 0.0f;
    // The universal flying mount's takeoff and landing pair: MinDistance 8,
    // DistanceCutoff 45 - a play of a hundred yards hears it twice as far.
    sound_distance::entryDistance(8.0f, 45.0f, min, max);
    CHECK(min == 8.0f);
    CHECK(max == 45.0f);
    // A row naming neither keeps the reach these plays had anyway.
    sound_distance::entryDistance(0.0f, 0.0f, min, max);
    CHECK(min == 1.0f);
    CHECK(max == 100.0f);
    // A full-volume core wider than the cutoff is pulled back to it, or the
    // gain would rise as the listener walks away.
    sound_distance::entryDistance(120.0f, 45.0f, min, max);
    CHECK(min == 45.0f);
    CHECK(max == 45.0f);
    // A cutoff with no core: a wide quiet reach, full volume only at the ear.
    sound_distance::entryDistance(0.0f, 200.0f, min, max);
    CHECK(min == 1.0f);
    CHECK(max == 200.0f);
}

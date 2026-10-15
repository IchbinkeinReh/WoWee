// How far a 3D sound is heard (audio/sound_distance.hpp): to its
// SoundEntries DistanceCutoff and no further.
#include <catch_amalgamated.hpp>

#include "audio/sound_distance.hpp"

using wowee::audio::sound_distance::heard;

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

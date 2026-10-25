// Which sounds are heard past the real voices (audio/voice_priority.hpp).
#include <catch_amalgamated.hpp>

#include "audio/voice_priority.hpp"

namespace vp = wowee::audio::voice_priority;

TEST_CASE("Sound_NumChannels is held to the client's range", "[audio]") {
    CHECK(vp::clampRealVoices(vp::kDefaultRealVoices) == 32);
    CHECK(vp::clampRealVoices(4) == 12);
    CHECK(vp::clampRealVoices(500) == 128);
}

TEST_CASE("the active mover's plays are heard over a crowd's", "[audio]") {
    // Three voices for four sounds: the quiet one with the mover's priority
    // keeps its voice, the quietest of the rest loses it.
    const std::vector<vp::Voice> voices = {
        {vp::kDefault, 0.9f}, {vp::kDefault, 0.2f}, {vp::kActiveMover, 0.1f}, {vp::kDefault, 0.5f}};
    const auto heard = vp::heard(voices, 3);
    CHECK(heard[0]);
    CHECK_FALSE(heard[1]);
    CHECK(heard[2]);
    CHECK(heard[3]);
}

TEST_CASE("among equals the first to start keeps its voice", "[audio]") {
    const std::vector<vp::Voice> voices = {{vp::kDefault, 0.5f}, {vp::kDefault, 0.5f}};
    const auto heard = vp::heard(voices, 1);
    CHECK(heard[0]);
    CHECK_FALSE(heard[1]);
    CHECK(vp::heard(voices, 0) == std::vector<bool>{false, false});
}

TEST_CASE("a 3D sound's reach by distance", "[audio]") {
    CHECK(vp::distanceGain(2.0f, 5.0f, 40.0f) == 1.0f);
    CHECK(vp::distanceGain(10.0f, 5.0f, 40.0f) == 0.5f);
    CHECK(vp::distanceGain(50.0f, 5.0f, 40.0f) == 0.0f);
}

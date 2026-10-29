// What a SoundEntries row asks of a play (sound_entry.hpp): the file a row
// names is its folder plus its name, and the swings its flags ask for.
#include <catch_amalgamated.hpp>

#include "audio/sound_entry.hpp"

namespace se = wowee::audio::sound_entry;

TEST_CASE("a row's file is its folder and its name", "[audio][sound_entry]") {
    // NPCSounds row 33 -> SoundEntries 5916: the name alone in column 3, the
    // folder in column 23.
    CHECK(se::filePath(R"(Sound\Creature\DwarfFemaleGuardNPC)", "DwarfFemaleGuardNPCGreeting01.wav") ==
          R"(Sound\Creature\DwarfFemaleGuardNPC\DwarfFemaleGuardNPCGreeting01.wav)");
    // A folder that already ends in a separator is not given a second.
    CHECK(se::filePath(R"(Sound\Spells\)", "Fire.wav") == R"(Sound\Spells\Fire.wav)");
    // No folder: the name is the path.
    CHECK(se::filePath("", R"(Sound\Interface\iMiniMapOpen.wav)") == R"(Sound\Interface\iMiniMapOpen.wav)");
}

TEST_CASE("a path's leaf, lowered, is what a row is found by", "[audio][sound_entry]") {
    CHECK(se::leafLower(R"(Sound\Creature\Kodo\KodoAggro.WAV)") == "kodoaggro.wav");
    CHECK(se::leafLower("Sound/Interface/iMiniMapOpen.wav") == "iminimapopen.wav");
    CHECK(se::leafLower("Bare.wav") == "bare.wav");
}

TEST_CASE("the swings follow the row's flags", "[audio][sound_entry]") {
    CHECK(se::pitchForFlags(0, 0.0f) == 1.0f);
    CHECK(se::pitchForFlags(se::kFlagPitchSwing, 0.0f) == Catch::Approx(0.85f));
    CHECK(se::pitchForFlags(se::kFlagPitchSwing, 1.0f) == Catch::Approx(1.15f));
}

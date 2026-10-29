#pragma once

// How far a 3D sound is heard.
//
// A SoundEntries row gives a 3D sound its MinDistance and DistanceCutoff,
// which the client hands its sound system as the 3D min and max (0x004c6a40
// -> 0x00879390). Inside the min it plays at full; past it, it falls off;
// past the cutoff it is not heard - a torch, a forge's bellows or a camp
// fire is a sound of the few yards round it.
//
// miniaudio's inverse model stops falling off at the max distance and holds
// the gain it had there - min / max, a fifth for a torch's 8 and 40 - however
// far off the sound is. With every looping doodad of the loaded map playing,
// hundreds of torches, fires, forges and bubbling flasks miles away were all
// heard at that fifth, and summed into a steady hiss under everything. So a
// sound past its cutoff is taken off the mix: a looping one paused where it
// is, to go on when the listener comes back in range, and one played once not
// played at all.

#include <glm/glm.hpp>

#include <algorithm>

namespace wowee::audio::sound_distance {

/// What a SoundEntries row asks of a 3D play, from its MinDistance (26) and
/// its DistanceCutoff (27). A row naming no cutoff keeps the reach every play
/// here had before the row was read - a hundred yards, which is what the sound
/// system's model needs to fall off over, not a rule of the row's - and one
/// naming no min has no full-volume core. A min past the cutoff is pulled back
/// to it, or the gain would rise with distance.
inline void entryDistance(float rowMin, float rowCutoff, float& outMin, float& outMax) {
    outMax = rowCutoff > 0.0f ? rowCutoff : 100.0f;
    outMin = rowMin > 0.0f ? std::min(rowMin, outMax) : 1.0f;
}

/// Whether a sound at `at` is heard from `listener`, with its `cutoff`.
/// A cutoff of 0 or less is none: a sound heard at any distance.
inline bool heard(const glm::vec3& at, const glm::vec3& listener, float cutoff) {
    if (!(cutoff > 0.0f)) return true;
    const glm::vec3 d = at - listener;
    return glm::dot(d, d) <= cutoff * cutoff;
}

}  // namespace wowee::audio::sound_distance

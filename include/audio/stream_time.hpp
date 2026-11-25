#pragma once

/// How far a streamed sound has played, without a word to its decoder.
///
/// The audio thread reads the decoder the whole time. Asked for the length of
/// an MP3, miniaudio goes to its beginning, decodes all of it and comes back
/// (ma_mp3_get_length_in_pcm_frames) - on the same decoder, from the thread
/// that asks: the two decoded into each other's buffers and the client went
/// down. What the engine has mixed of a sound is counted on the sound's node,
/// atomically, and that is all this reads.

#include <cstdint>

namespace wowee::audio {

/// Seconds of it played so far, in the engine's own time; a looping one's
/// rounds count on.
template <typename Sound>
[[nodiscard]] double streamSeconds(const Sound* sound, uint32_t engineRate) {
    if (!sound || engineRate == 0) return 0.0;
    return static_cast<double>(ma_sound_get_time_in_pcm_frames(sound)) / engineRate;
}

}  // namespace wowee::audio

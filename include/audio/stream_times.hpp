#pragma once

#include "../../extern/miniaudio.h"

#include "audio/stream_time.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

// How far in a sound is and how long, for who shows it.
namespace wowee::audio {

/// `streamed` is the file a sound is decoded from as it plays, none for one
/// held decoded. The length of a stream is worked out on a decoder of its own:
/// an MP3 is run through to its end for it, and on the playing decoder that is
/// the one the audio thread is reading - it took the decoder from under it.
/// How far in it is is what the engine has mixed of it (stream_time.hpp), in a
/// looping one within its round.
inline void readSoundTimes(const ma_sound* sound, const std::vector<uint8_t>* streamed, double& seconds,
                           double& length) {
    ma_uint64 cursor = 0, frames = 0;
    if (!streamed) {
        ma_uint32 rate = 0;
        if (ma_sound_get_data_format(sound, nullptr, nullptr, &rate, nullptr, 0) != MA_SUCCESS || rate == 0) return;
        if (ma_sound_get_cursor_in_pcm_frames(sound, &cursor) == MA_SUCCESS)
            seconds = static_cast<double>(cursor) / rate;
        if (ma_sound_get_length_in_pcm_frames(sound, &frames) == MA_SUCCESS)
            length = static_cast<double>(frames) / rate;
        return;
    }
    ma_engine* engine = ma_sound_get_engine(sound);
    seconds = streamSeconds(sound, engine ? ma_engine_get_sample_rate(engine) : 0);
    ma_decoder decoder;
    const ma_decoder_config config = ma_decoder_config_init_default();
    if (ma_decoder_init_memory(streamed->data(), streamed->size(), &config, &decoder) != MA_SUCCESS) return;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && decoder.outputSampleRate != 0)
        length = static_cast<double>(frames) / decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);
    if (length > 0.0 && ma_sound_is_looping(sound) == MA_TRUE) seconds = std::fmod(seconds, length);
}

}  // namespace wowee::audio

// A streamed sound's time is read off its node: nothing is asked of its data
// source, which the audio thread reads at the same time.
#include <catch_amalgamated.hpp>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#include <miniaudio.h>

#include "audio/stream_time.hpp"

#include <cstring>
#include <vector>

namespace {

constexpr ma_uint32 kRate = 48000;

// A stream that notes what is asked of it.
struct Counting {
    ma_data_source_base base;
    int reads = 0, seeks = 0, cursors = 0, lengths = 0;
};

ma_result onRead(ma_data_source* source, void* out, ma_uint64 count, ma_uint64* read) {
    ++static_cast<Counting*>(source)->reads;
    if (out) std::memset(out, 0, static_cast<size_t>(count) * 2 * sizeof(float));
    if (read) *read = count;
    return MA_SUCCESS;
}
ma_result onSeek(ma_data_source* source, ma_uint64) {
    ++static_cast<Counting*>(source)->seeks;
    return MA_SUCCESS;
}
ma_result onFormat(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel* map, size_t cap) {
    if (format) *format = ma_format_f32;
    if (channels) *channels = 2;
    if (rate) *rate = kRate;
    if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, cap, 2);
    return MA_SUCCESS;
}
ma_result onCursor(ma_data_source* source, ma_uint64* cursor) {
    ++static_cast<Counting*>(source)->cursors;
    *cursor = 0;
    return MA_SUCCESS;
}
ma_result onLength(ma_data_source* source, ma_uint64* length) {
    ++static_cast<Counting*>(source)->lengths;
    *length = 100 * kRate;
    return MA_SUCCESS;
}

ma_data_source_vtable gStream = {onRead, onSeek, onFormat, onCursor, onLength, nullptr, 0};

struct Playing {
    ma_engine engine{};
    ma_sound sound{};
    Counting stream{};
    Playing() {
        ma_engine_config config = ma_engine_config_init();
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = kRate;
        REQUIRE(ma_engine_init(&config, &engine) == MA_SUCCESS);
        ma_data_source_config source = ma_data_source_config_init();
        source.vtable = &gStream;
        REQUIRE(ma_data_source_init(&source, &stream.base) == MA_SUCCESS);
        REQUIRE(ma_sound_init_from_data_source(&engine, &stream, MA_SOUND_FLAG_STREAM, nullptr, &sound) == MA_SUCCESS);
        REQUIRE(ma_sound_start(&sound) == MA_SUCCESS);
    }
    ~Playing() {
        ma_sound_uninit(&sound);
        ma_engine_uninit(&engine);
        ma_data_source_uninit(&stream.base);
    }
    void play(ma_uint64 frames) {
        std::vector<float> out(static_cast<size_t>(frames) * 2);
        REQUIRE(ma_engine_read_pcm_frames(&engine, out.data(), frames, nullptr) == MA_SUCCESS);
    }
};

}  // namespace

TEST_CASE("a stream's time asks nothing of its decoder", "[audio]") {
    Playing p;
    CHECK(wowee::audio::streamSeconds(&p.sound, kRate) == 0.0);
    p.play(kRate / 2);
    const int reads = p.stream.reads, seeks = p.stream.seeks, cursors = p.stream.cursors, lengths = p.stream.lengths;
    REQUIRE(reads > 0);
    CHECK(wowee::audio::streamSeconds(&p.sound, kRate) == Catch::Approx(0.5).margin(0.02));
    CHECK(p.stream.reads == reads);
    CHECK(p.stream.seeks == seeks);
    CHECK(p.stream.cursors == cursors);
    CHECK(p.stream.lengths == lengths);
    p.play(kRate);
    CHECK(wowee::audio::streamSeconds(&p.sound, kRate) == Catch::Approx(1.5).margin(0.02));
}

TEST_CASE("there is no time without a sound or a rate", "[audio]") {
    CHECK(wowee::audio::streamSeconds(static_cast<const ma_sound*>(nullptr), kRate) == 0.0);
    Playing p;
    CHECK(wowee::audio::streamSeconds(&p.sound, 0) == 0.0);
}

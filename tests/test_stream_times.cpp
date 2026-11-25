// What is said of a streamed sound's length never asks the decoder the audio
// thread is reading (audio/stream_times.hpp).
#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "audio/stream_times.hpp"

#include <catch_amalgamated.hpp>

namespace {

// A source that plays silence and counts who asks what of it.
struct Source {
    ma_data_source_base base;
    int lengthAsked = 0;
    int sought = 0;
    ma_uint64 cursor = 0;
};

ma_result onRead(ma_data_source* s, void* out, ma_uint64 frames, ma_uint64* read) {
    auto* src = reinterpret_cast<Source*>(s);
    if (out) ma_silence_pcm_frames(out, frames, ma_format_f32, 1);
    src->cursor += frames;
    if (read) *read = frames;
    return MA_SUCCESS;
}
ma_result onSeek(ma_data_source* s, ma_uint64 frame) {
    auto* src = reinterpret_cast<Source*>(s);
    ++src->sought;
    src->cursor = frame;
    return MA_SUCCESS;
}
ma_result onFormat(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* rate, ma_channel*, size_t) {
    if (format) *format = ma_format_f32;
    if (channels) *channels = 1;
    if (rate) *rate = 8000;
    return MA_SUCCESS;
}
ma_result onCursor(ma_data_source* s, ma_uint64* cursor) {
    *cursor = reinterpret_cast<Source*>(s)->cursor;
    return MA_SUCCESS;
}
ma_result onLength(ma_data_source* s, ma_uint64* length) {
    ++reinterpret_cast<Source*>(s)->lengthAsked;
    *length = 8000 * 9;
    return MA_SUCCESS;
}

// Half a second of 16-bit mono at 8000 Hz.
std::vector<uint8_t> wav() {
    std::vector<uint8_t> out;
    const auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    const auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    const auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(t[i])); };
    const uint32_t bytes = 4000 * 2;
    tag("RIFF"); u32(36 + bytes); tag("WAVE");
    tag("fmt "); u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    tag("data"); u32(bytes);
    out.resize(out.size() + bytes, 0);
    return out;
}

}  // namespace

TEST_CASE("a stream's length is not asked of the decoder that plays it", "[audio]") {
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 1;
    config.sampleRate = 8000;
    ma_engine engine;
    REQUIRE(ma_engine_init(&config, &engine) == MA_SUCCESS);
    static ma_data_source_vtable vtable{onRead, onSeek, onFormat, onCursor, onLength, nullptr, 0};
    Source source{};
    ma_data_source_config sourceConfig = ma_data_source_config_init();
    sourceConfig.vtable = &vtable;
    REQUIRE(ma_data_source_init(&sourceConfig, &source.base) == MA_SUCCESS);
    ma_sound sound;
    REQUIRE(ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                           nullptr, &sound) == MA_SUCCESS);
    REQUIRE(ma_sound_start(&sound) == MA_SUCCESS);
    std::vector<float> mixed(2000);
    REQUIRE(ma_engine_read_pcm_frames(&engine, mixed.data(), mixed.size(), nullptr) == MA_SUCCESS);
    source.lengthAsked = 0;
    source.sought = 0;

    double seconds = 0.0, length = 0.0;
    const auto file = wav();
    wowee::audio::readSoundTimes(&sound, &file, seconds, length);
    CHECK(source.lengthAsked == 0);
    CHECK(source.sought == 0);
    CHECK(seconds == Catch::Approx(0.25));
    CHECK(length == Catch::Approx(0.5));

    // A looping one's time is the one in its round.
    ma_sound_set_looping(&sound, MA_TRUE);
    mixed.resize(3000);
    REQUIRE(ma_engine_read_pcm_frames(&engine, mixed.data(), mixed.size(), nullptr) == MA_SUCCESS);
    source.lengthAsked = 0;
    wowee::audio::readSoundTimes(&sound, &file, seconds, length);
    CHECK(source.lengthAsked == 0);
    CHECK(seconds == Catch::Approx(0.125));

    // One held decoded has its length at hand.
    wowee::audio::readSoundTimes(&sound, nullptr, seconds, length);
    CHECK(source.lengthAsked == 1);
    CHECK(length == Catch::Approx(9.0));

    ma_sound_uninit(&sound);
    ma_data_source_uninit(&source.base);
    ma_engine_uninit(&engine);
}

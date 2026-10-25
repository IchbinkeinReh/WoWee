#define MINIAUDIO_IMPLEMENTATION
#include "audio/audio_engine.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/sound_distance.hpp"


#include "../../extern/miniaudio.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace wowee {
namespace audio {

namespace {

/// Owns malloc'd storage for a miniaudio object and unwinds it correctly on
/// any early return.
///
/// miniaudio initialises into caller-provided storage, so teardown is two
/// steps -- uninit the object, then free the storage -- and the uninit must
/// not run on storage that was never initialised. That is the distinction the
/// hand-written cleanup here used to encode by having a different free()
/// sequence at each of five early returns per function, duplicated across two
/// functions. Every one of them had to be right, and nothing checked that.
///
/// Call markInitialised() once the matching ma_*_init has succeeded, and
/// release() to hand the pointer to activeSounds_, which owns it from then on.
template <typename T, void (*Uninit)(T*)>
class MaStorage {
public:
    MaStorage() : ptr_(static_cast<T*>(std::malloc(sizeof(T)))) {}
    ~MaStorage() {
        if (ptr_ != nullptr) {
            if (initialised_) {
                Uninit(ptr_);
            }
            std::free(ptr_);
        }
    }

    MaStorage(const MaStorage&) = delete;
    MaStorage& operator=(const MaStorage&) = delete;

    explicit operator bool() const { return ptr_ != nullptr; }
    T* get() const { return ptr_; }
    void markInitialised() { initialised_ = true; }

    /// Give up ownership. The caller frees it from here on.
    T* release() {
        T* p = ptr_;
        ptr_ = nullptr;
        initialised_ = false;
        return p;
    }

private:
    T* ptr_ = nullptr;
    bool initialised_ = false;
};

using AudioBufferStorage = MaStorage<ma_audio_buffer, ma_audio_buffer_uninit>;
using SoundStorage = MaStorage<ma_sound, ma_sound_uninit>;

struct DecodedWavCacheEntry {
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_uint64 frames = 0;
    std::shared_ptr<std::vector<uint8_t>> pcmData;
};

static std::unordered_map<uint64_t, DecodedWavCacheEntry> gDecodedWavCache;
// Protects gDecodedWavCache - shared_lock for reads, unique_lock for writes.
// Required because playSound2D() can be called from multiple threads
// (main thread, async loaders, animation callbacks).
static std::shared_mutex gDecodedWavCacheMutex;

static uint64_t makeWavCacheKey(const std::vector<uint8_t>& wavData) {
    // FNV-1a over the first 256 bytes + last 256 bytes + total size.
    // Full-content hash would be correct but slow for large files; sampling the
    // edges catches virtually all distinct files while keeping cost O(1).
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    auto mix = [&](uint8_t b) { h ^= b; h *= FNV_PRIME; };

    const size_t sz = wavData.size();
    const size_t head = std::min(sz, size_t(256));
    for (size_t i = 0; i < head; ++i) mix(wavData[i]);
    if (sz > 256) {
        const size_t tail_start = sz > 512 ? sz - 256 : 256;
        for (size_t i = tail_start; i < sz; ++i) mix(wavData[i]);
    }
    // Mix in the total size so files with identical head/tail but different
    // lengths still produce different keys.
    for (int s = 0; s < 8; ++s) mix(static_cast<uint8_t>(sz >> (s * 8)));
    return h;
}

static bool decodeWavCached(const std::vector<uint8_t>& wavData, DecodedWavCacheEntry& out) {
    if (wavData.empty()) return false;

    const uint64_t key = makeWavCacheKey(wavData);

    // Fast path: shared (read) lock for cache hits - allows concurrent lookups.
    {
        std::shared_lock<std::shared_mutex> readLock(gDecodedWavCacheMutex);
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
    }

    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        wavData.data(),
        wavData.size(),
        &decoderConfig,
        &decoder
    );
    if (result != MA_SUCCESS) {
        LOG_ERROR("AudioEngine: Failed to decode WAV data (", wavData.size(), " bytes): error ", result);
        return false;
    }

    ma_uint64 totalFrames = 0;
    result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (result != MA_SUCCESS) totalFrames = 0;

    ma_format format = decoder.outputFormat;
    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 sampleRate = decoder.outputSampleRate;
    ma_uint64 maxFrames = sampleRate * 60;
    if (totalFrames == 0 || totalFrames > maxFrames) totalFrames = maxFrames;

    size_t bufferSize = totalFrames * channels * ma_get_bytes_per_sample(format);
    auto pcmData = std::make_shared<std::vector<uint8_t>>(bufferSize);
    ma_uint64 framesRead = 0;
    result = ma_decoder_read_pcm_frames(&decoder, pcmData->data(), totalFrames, &framesRead);
    ma_decoder_uninit(&decoder);
    if (result != MA_SUCCESS || framesRead == 0) {
        LOG_ERROR("AudioEngine: Failed to read frames from WAV: error ", result, ", framesRead=", framesRead);
        return false;
    }

    pcmData->resize(framesRead * channels * ma_get_bytes_per_sample(format));

    DecodedWavCacheEntry entry;
    entry.format = format;
    entry.channels = channels;
    entry.sampleRate = sampleRate;
    entry.frames = framesRead;
    entry.pcmData = pcmData;
    // Evict oldest half when cache grows too large. 256 entries ≈ 50-100 MB of decoded
    // PCM data depending on file lengths; halving keeps memory bounded while retaining
    // recently-heard sounds (footsteps, UI clicks, combat hits) for instant replay.
    // Exclusive (write) lock - only one thread can evict + insert.
    {
        std::lock_guard<std::shared_mutex> writeLock(gDecodedWavCacheMutex);
        // Re-check in case another thread inserted while we were decoding.
        if (auto it = gDecodedWavCache.find(key); it != gDecodedWavCache.end()) {
            out = it->second;
            return true;
        }
        constexpr size_t kMaxCachedSounds = 256;
        if (gDecodedWavCache.size() >= kMaxCachedSounds) {
            auto it = gDecodedWavCache.begin();
            std::advance(it, gDecodedWavCache.size() / 2);
            gDecodedWavCache.erase(gDecodedWavCache.begin(), it);
        }
        gDecodedWavCache.emplace(key, entry);
    }
    out = entry;
    return true;
}

} // namespace

AudioEngine& AudioEngine::instance() {
    static AudioEngine instance;
    return instance;
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
    if (initialized_) {
        LOG_WARNING("AudioEngine already initialized");
        return true;
    }

    // Allocate miniaudio engine
    engine_ = new ma_engine();

    // The default config, but with the device's callback our own: it mixes the
    // same way miniaudio's does and also feeds the screen recorder's tap.
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.dataCallback = &AudioEngine::deviceDataCallback;
    ma_result result = ma_engine_init(&engineConfig, engine_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to initialize miniaudio engine: ", result);
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    // Set default master volume
    ma_engine_set_volume(engine_, masterVolume_);

    // Log audio backend info
    ma_backend backend = ma_engine_get_device(engine_)->pContext->backend;
    const char* backendName = "unknown";
    switch (backend) {
        case ma_backend_wasapi: backendName = "WASAPI"; break;
        case ma_backend_dsound: backendName = "DirectSound"; break;
        case ma_backend_winmm: backendName = "WinMM"; break;
        case ma_backend_coreaudio: backendName = "CoreAudio"; break;
        case ma_backend_sndio: backendName = "sndio"; break;
        case ma_backend_audio4: backendName = "audio(4)"; break;
        case ma_backend_oss: backendName = "OSS"; break;
        case ma_backend_pulseaudio: backendName = "PulseAudio"; break;
        case ma_backend_alsa: backendName = "ALSA"; break;
        case ma_backend_jack: backendName = "JACK"; break;
        case ma_backend_aaudio: backendName = "AAudio"; break;
        case ma_backend_opensl: backendName = "OpenSL|ES"; break;
        case ma_backend_webaudio: backendName = "WebAudio"; break;
        case ma_backend_custom: backendName = "Custom"; break;
        case ma_backend_null: backendName = "Null (no output)"; break;
        default: break;
    }

    // The Effects channel: a group under the engine, at the effects volume.
    effectsGroup_ = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound_group)));
    if (effectsGroup_ && ma_sound_group_init(engine_, 0, nullptr, effectsGroup_) == MA_SUCCESS) {
        ma_sound_group_set_volume(effectsGroup_, effectsVolume_);
    } else {
        LOG_WARNING("AudioEngine: no effects group - those sounds play at their own volume");
        std::free(effectsGroup_);
        effectsGroup_ = nullptr;
    }

    initialized_ = true;
    LOG_INFO("AudioEngine initialized (miniaudio, backend: ", backendName, ")");
    return true;
}

ma_sound* AudioEngine::groupFor(Channel channel) const {
    return channel == Channel::Effects ? effectsGroup_ : nullptr;
}

void AudioEngine::setEffectsVolume(float volume) {
    effectsVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (effectsGroup_) ma_sound_group_set_volume(effectsGroup_, effectsVolume_);
}

bool AudioEngine::startOrHold(ma_sound* sound, bool loops, bool& held) {
    held = false;
    switch (world_sound_hold::start(isWorldSoundHeld(), loops)) {
    case world_sound_hold::Start::Now:
        return ma_sound_start(sound) == MA_SUCCESS;
    case world_sound_hold::Start::Deferred:
        held = true;
        return true;
    case world_sound_hold::Start::Refused:
        break;
    }
    return false;
}

void AudioEngine::beginWorldSoundHold() {
    worldSoundHolds_.begin();
}

void AudioEngine::endWorldSoundHold() {
    if (!worldSoundHolds_.end()) return;
    // The world is shown: what waited starts now, its fade in from here.
    size_t started = 0;
    for (auto& active : activeSounds_) {
        if (!active.held) continue;
        active.held = false;
        if (active.culled) continue;  // out of range: cullByDistance starts it
        ma_sound_start(active.sound);
        ++started;
    }
    for (auto& stream : activeStreams_) {
        if (!stream.held) continue;
        stream.held = false;
        ma_sound_start(stream.sound);
        ++started;
    }
    LOG_INFO("AudioEngine: the world's sound held for the loading screen let go, ", started,
             " waiting sound(s) started");
}

std::string AudioEngine::getOutputDeviceName() const {
    if (!initialized_ || !engine_) return {};
    const ma_device* device = ma_engine_get_device(const_cast<ma_engine*>(engine_));
    if (!device || device->playback.name[0] == '\0') return {};
    return device->playback.name;
}

void AudioEngine::shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop music
    stopMusic();
    for (auto& stream : activeStreams_) releaseStream(stream);
    activeStreams_.clear();

    // Clean up all active sounds
    for (auto& activeSound : activeSounds_) {
        ma_sound_uninit(activeSound.sound);
        std::free(activeSound.sound);
        ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSound.buffer);
        ma_audio_buffer_uninit(buffer);
        std::free(buffer);
    }
    activeSounds_.clear();

    if (effectsGroup_) {
        ma_sound_group_uninit(effectsGroup_);
        std::free(effectsGroup_);
        effectsGroup_ = nullptr;
    }

    capturing_.store(false, std::memory_order_release);
    if (engine_) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }
    // The device is gone, so nothing writes to the ring any more.
    if (captureRing_) {
        auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
        ma_pcm_rb_uninit(ring);
        delete ring;
        captureRing_ = nullptr;
    }

    initialized_ = false;
    LOG_INFO("AudioEngine shutdown");
}

void AudioEngine::setMasterVolume(float volume) {
    masterVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (engine_) {
        if (!suspended_) ma_engine_set_volume(engine_, masterVolume_);
    }
}
void AudioEngine::setSuspended(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    // The engine's own level is the one thing that changes; masterVolume_ is
    // left alone so the slider and the resume both still read it.
    if (engine_) ma_engine_set_volume(engine_, suspended_ ? 0.0f : masterVolume_);
}


void AudioEngine::setListenerPosition(const glm::vec3& position) {
    listenerPosition_ = position;
    if (engine_) {
        ma_engine_listener_set_position(engine_, 0, position.x, position.y, position.z);
    }
}

void AudioEngine::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    listenerForward_ = forward;
    listenerUp_ = up;
    if (engine_) {
        ma_engine_listener_set_direction(engine_, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(engine_, 0, up.x, up.y, up.z);
    }
}

bool AudioEngine::playSound2D(const std::vector<uint8_t>& wavData, float volume, float pitch, Channel channel,
                              PlayLimit limit, int priority) {
    // Size and volume, because this overload is handed decoded bytes and has
    // no name to report. The sample managers all cache their clips and call
    // this one, so the path-named log above never fires for them - and a sound
    // reported as playing loudly on every world entry produced no sfx: line at
    // all. A byte count identifies the file well enough to find it on disk.
    LOG_INFO("sfx2d: bytes=", wavData.size(), " vol=", volume);
    (void)pitch;
    if (!initialized_ || !engine_ || wavData.empty()) return false;
    if (masterVolume_ <= 0.0f) return false;
    // Played once: not while the loading screen holds the world's sound.
    if (world_sound_hold::start(isWorldSoundHeld(), false) == world_sound_hold::Start::Refused) return false;
    // Not past as many of its type as may play at once (0x004c6a40).
    if (!play_limit::admits(limit, playingOf(limit))) return false;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        return false;
    }

    // Create audio buffer from decoded PCM data (heap allocated to keep alive)
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr  // No custom allocator
    );
    // Must set explicitly - miniaudio defaults to device sample rate, which causes
    // pitch distortion if it differs from the file's native rate (e.g. 22050 vs 44100 Hz).
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return false;
    ma_result result = ma_audio_buffer_init(&bufferConfig, audioBuffer.get());
    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to create audio buffer: ", result);
        return false;
    }
    audioBuffer.markInitialised();

    // Create sound from audio buffer
    SoundStorage sound;
    if (!sound) return false;
    result = ma_sound_init_from_data_source(
        engine_,
        audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        groupFor(channel),
        sound.get()
    );

    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to create sound: ", result);
        return false;
    }
    sound.markInitialised();

    // Set volume (pitch not supported with NO_PITCH flag)
    ma_sound_set_volume(sound.get(), volume);

    // Start playback
    result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS) {
        LOG_WARNING("Failed to start sound: ", result);
        return false;
    }

    // Track this sound for cleanup (decoded PCM shared across plays)
    activeSounds_.push_back({sound.release(), audioBuffer.release(), decoded.pcmData, 0u, false, 0.0f, false, limit});
    noteStarted(priority, volume, channel, false, 0.0f);

    return true;
}

uint32_t AudioEngine::playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume, bool loop,
                                           float fadeInSeconds) {
    if (!initialized_ || !engine_ || wavData.empty()) return 0;
    if (masterVolume_ <= 0.0f) return 0;
    if (world_sound_hold::start(isWorldSoundHeld(), loop) == world_sound_hold::Start::Refused) return 0;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) return 0;

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format, decoded.channels, decoded.frames, decoded.pcmData->data(), nullptr);
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return 0;
    if (ma_audio_buffer_init(&bufferConfig, audioBuffer.get()) != MA_SUCCESS) {
        return 0;
    }
    audioBuffer.markInitialised();

    SoundStorage sound;
    if (!sound) return 0;
    ma_result result = ma_sound_init_from_data_source(
        engine_, audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr, sound.get());
    if (result != MA_SUCCESS) {
        return 0;
    }
    sound.markInitialised();

    ma_sound_set_volume(sound.get(), volume);
    if (loop) ma_sound_set_looping(sound.get(), MA_TRUE);
    if (fadeInSeconds > 0.0f)
        ma_sound_set_fade_in_milliseconds(sound.get(), 0.0f, 1.0f,
                                          static_cast<ma_uint64>(fadeInSeconds * 1000.0f));
    bool held = false;
    if (!startOrHold(sound.get(), loop, held)) {
        return 0;
    }

    uint32_t id = nextSoundId_++;
    if (nextSoundId_ == 0) nextSoundId_ = 1;  // Skip 0 (sentinel)
    activeSounds_.push_back({sound.release(), audioBuffer.release(), decoded.pcmData, id, held});
    noteStarted(voice_priority::kDefault, volume, Channel::Own, false, 0.0f);
    return id;
}

void AudioEngine::stopSound(uint32_t id) {
    if (id == 0) return;
    for (auto it = activeSounds_.begin(); it != activeSounds_.end(); ++it) {
        if (it->id == id) {
            ma_sound_stop(it->sound);
            ma_sound_uninit(it->sound);
            std::free(it->sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(it->buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_.erase(it);
            return;
        }
    }
}

bool AudioEngine::playSound2D(const std::string& mpqPath, float volume, float pitch, Channel channel) {
    // Which file, and how loud, for the one-shots that name a path.
    //
    // A sound reported as playing loudly on every world entry cannot be found
    // by reading: a dozen managers reach this and none of them is obviously the
    // one. This says so in a line, and the timestamp beside the world-entry
    // lines in the same log is what pins it.
    LOG_INFO("sfx: ", mpqPath, " vol=", volume);
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound2D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound2D: failed to load '", mpqPath, "'");
        return false;
    }
    return playSound2D(data, volume, pitch, channel);
}

bool AudioEngine::playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, float minDistance, Channel channel,
                              PlayLimit limit, int priority) {
    if (!initialized_ || !engine_ || wavData.empty()) return false;
    if (masterVolume_ <= 0.0f) return false;
    // Past its cutoff it is not heard, so not played (audio/sound_distance.hpp).
    if (!sound_distance::heard(position, listenerPosition_, maxDistance)) return false;
    // Played once: not while the loading screen holds the world's sound.
    if (world_sound_hold::start(isWorldSoundHeld(), false) == world_sound_hold::Start::Refused) return false;
    // Not past as many of its type as may play at once (0x004c6a40).
    if (!play_limit::admits(limit, playingOf(limit))) return false;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) {
        return false;
    }

    LOG_DEBUG("playSound3D: cached WAV - format:", decoded.format,
              " channels:", decoded.channels, " sampleRate:", decoded.sampleRate,
              " pitch:", pitch);

    // Create audio buffer with correct sample rate
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format,
        decoded.channels,
        decoded.frames,
        decoded.pcmData->data(),
        nullptr
    );
    // Must set explicitly - miniaudio defaults to device sample rate, which causes
    // pitch distortion if it differs from the file's native rate (e.g. 22050 vs 44100 Hz).
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return false;
    ma_result result = ma_audio_buffer_init(&bufferConfig, audioBuffer.get());
    if (result != MA_SUCCESS) {
        return false;
    }
    audioBuffer.markInitialised();

    // Create 3D sound (spatialization enabled, pitch enabled)
    SoundStorage sound;
    if (!sound) return false;
    result = ma_sound_init_from_data_source(
        engine_,
        audioBuffer.get(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC,  // Removed NO_PITCH flag
        groupFor(channel),
        sound.get()
    );

    if (result != MA_SUCCESS) {
        LOG_WARNING("playSound3D: Failed to create sound, error: ", result);
        return false;
    }
    sound.markInitialised();

    // Set 3D position and attenuation
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), volume);
    ma_sound_set_pitch(sound.get(), pitch);  // Enable pitch variation
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);
    ma_sound_set_min_gain(sound.get(), 0.0f);
    ma_sound_set_max_gain(sound.get(), 1.0f);
    ma_sound_set_min_distance(sound.get(), std::max(minDistance, 0.0f));
    ma_sound_set_max_distance(sound.get(), maxDistance);
    ma_sound_set_rolloff(sound.get(), 1.0f);

    result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS) {
        return false;
    }

    // Track for cleanup
    activeSounds_.push_back(
        {sound.release(), audioBuffer.release(), decoded.pcmData, 0u, false, maxDistance, false, limit});
    noteStarted(priority, volume, channel, true, std::max(minDistance, 0.0f));

    return true;
}

bool AudioEngine::playSound3D(const std::string& mpqPath, const glm::vec3& position,
                              float volume, float pitch, float maxDistance, Channel channel) {
    if (!assetManager_) {
        LOG_WARNING("AudioEngine::playSound3D(path): no AssetManager set");
        return false;
    }
    auto data = assetManager_->readFile(mpqPath);
    if (data.empty()) {
        LOG_WARNING("AudioEngine::playSound3D: failed to load '", mpqPath, "'");
        return false;
    }
    return playSound3D(data, position, volume, pitch, maxDistance, 1.0f, channel);
}

uint32_t AudioEngine::playSound3DLooping(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                         float volume, float maxDistance, float minDistance,
                                         float fadeInSeconds, Channel channel) {
    if (!initialized_ || !engine_ || wavData.empty()) return 0;
    if (masterVolume_ <= 0.0f) return 0;

    DecodedWavCacheEntry decoded;
    if (!decodeWavCached(wavData, decoded) || !decoded.pcmData || decoded.frames == 0) return 0;

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        decoded.format, decoded.channels, decoded.frames, decoded.pcmData->data(), nullptr);
    bufferConfig.sampleRate = decoded.sampleRate;

    AudioBufferStorage audioBuffer;
    if (!audioBuffer) return 0;
    if (ma_audio_buffer_init(&bufferConfig, audioBuffer.get()) != MA_SUCCESS) return 0;
    audioBuffer.markInitialised();

    SoundStorage sound;
    if (!sound) return 0;
    if (ma_sound_init_from_data_source(engine_, audioBuffer.get(),
                                       MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_PITCH,
                                       groupFor(channel), sound.get()) != MA_SUCCESS) {
        return 0;
    }
    sound.markInitialised();

    ma_sound_set_looping(sound.get(), MA_TRUE);
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_volume(sound.get(), volume);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);
    ma_sound_set_min_gain(sound.get(), 0.0f);
    ma_sound_set_max_gain(sound.get(), 1.0f);
    ma_sound_set_min_distance(sound.get(), std::max(minDistance, 0.0f));
    ma_sound_set_max_distance(sound.get(), maxDistance);
    ma_sound_set_rolloff(sound.get(), 1.0f);
    if (fadeInSeconds > 0.0f)
        ma_sound_set_fade_in_milliseconds(sound.get(), 0.0f, 1.0f,
                                          static_cast<ma_uint64>(fadeInSeconds * 1000.0f));
    // Out past its cutoff it waits, paused, for the listener to come in.
    const bool culled = !sound_distance::heard(position, listenerPosition_, maxDistance);
    bool held = false;
    if (culled) {
        held = world_sound_hold::start(isWorldSoundHeld(), true) == world_sound_hold::Start::Deferred;
    } else if (!startOrHold(sound.get(), true, held)) {
        return 0;
    }

    uint32_t id = nextSoundId_++;
    if (nextSoundId_ == 0) nextSoundId_ = 1;  // Skip 0 (sentinel)
    activeSounds_.push_back(
        {sound.release(), audioBuffer.release(), decoded.pcmData, id, held, maxDistance, culled});
    noteStarted(voice_priority::kDefault, volume, channel, true, std::max(minDistance, 0.0f));
    return id;
}

void AudioEngine::setSoundPosition(uint32_t id, const glm::vec3& position) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id) {
            ma_sound_set_position(active.sound, position.x, position.y, position.z);
            return;
        }
    }
}

void AudioEngine::setSoundVolume(uint32_t id, float volume) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id) {
            // One without a real voice keeps it for when it gets one back.
            active.volume = volume;
            if (!active.virtualVoice) ma_sound_set_volume(active.sound, volume);
            return;
        }
    }
}

void AudioEngine::stopSoundWithFade(uint32_t id, float seconds) {
    if (id == 0) return;
    for (auto& active : activeSounds_) {
        if (active.id == id) {
            // One still waiting for the hold's end, or paused out of range,
            // is not heard: gone now.
            if (active.held || active.culled) {
                stopSound(id);
                return;
            }
            // update() takes it away once it has stopped playing.
            ma_sound_stop_with_fade_in_milliseconds(
                active.sound, static_cast<ma_uint64>(std::max(seconds, 0.0f) * 1000.0f));
            active.id = 0;
            return;
        }
    }
}

void AudioEngine::releaseStream(ActiveStream& stream) {
    if (stream.sound) {
        ma_sound_uninit(stream.sound);
        std::free(stream.sound);
        stream.sound = nullptr;
    }
    if (stream.decoder) {
        auto* decoder = static_cast<ma_decoder*>(stream.decoder);
        ma_decoder_uninit(decoder);
        delete decoder;
        stream.decoder = nullptr;
    }
    stream.data.reset();
}

uint32_t AudioEngine::playStream(std::shared_ptr<const std::vector<uint8_t>> data, float volume, bool loop) {
    if (!initialized_ || !engine_ || !data || data->empty()) return 0;
    ActiveStream stream;
    stream.data = std::move(data);
    auto* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    if (ma_decoder_init_memory(stream.data->data(), stream.data->size(), &decoderConfig, decoder) != MA_SUCCESS) {
        delete decoder;
        return 0;
    }
    stream.decoder = decoder;
    stream.sound = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!stream.sound) {
        releaseStream(stream);
        return 0;
    }
    if (ma_sound_init_from_data_source(engine_, decoder,
                                       MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_PITCH |
                                           MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       nullptr, stream.sound) != MA_SUCCESS) {
        std::free(stream.sound);
        stream.sound = nullptr;
        releaseStream(stream);
        return 0;
    }
    ma_sound_set_volume(stream.sound, volume);
    ma_sound_set_looping(stream.sound, loop ? MA_TRUE : MA_FALSE);
    // A stream waits out the hold whether it loops or not: it is the zone's
    // music, which the world's first step would start.
    if (!startOrHold(stream.sound, true, stream.held)) {
        releaseStream(stream);
        return 0;
    }
    stream.id = nextSoundId_++;
    if (nextSoundId_ == 0) nextSoundId_ = 1;
    activeStreams_.push_back(std::move(stream));
    return activeStreams_.back().id;
}

bool AudioEngine::isStreamPlaying(uint32_t id) const {
    if (id == 0) return false;
    for (const auto& stream : activeStreams_)
        if (stream.id == id) return stream.held || ma_sound_is_playing(stream.sound) == MA_TRUE;
    return false;
}

void AudioEngine::setStreamVolume(uint32_t id, float volume) {
    if (id == 0) return;
    for (auto& stream : activeStreams_) {
        if (stream.id == id) {
            ma_sound_set_volume(stream.sound, volume);
            return;
        }
    }
}

void AudioEngine::stopStream(uint32_t id) {
    if (id == 0) return;
    for (size_t i = 0; i < activeStreams_.size(); ++i) {
        if (activeStreams_[i].id != id) continue;
        releaseStream(activeStreams_[i]);
        activeStreams_[i] = std::move(activeStreams_.back());
        activeStreams_.pop_back();
        return;
    }
}

bool AudioEngine::playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                            float volume, bool loop) {
    if (!initialized_ || !engine_ || !musicData || musicData->empty()) {
        return false;
    }

    LOG_INFO("AudioEngine::playMusic - data size: ", musicData->size(), " bytes, volume: ", volume);

    // Stop any currently playing music
    stopMusic();

    // Keep the encoded bytes alive for as long as miniaudio's decoder streams them.
    musicData_ = std::move(musicData);
    musicVolume_ = volume;

    // Create decoder from memory (for streaming MP3/OGG)
    ma_decoder* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_memory(
        musicData_->data(),
        musicData_->size(),
        &decoderConfig,
        decoder
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music decoder: ", result);
        delete decoder;
        musicData_.reset();
        return false;
    }

    LOG_INFO("Decoder created - format: ", decoder->outputFormat,
             ", channels: ", decoder->outputChannels,
             ", sampleRate: ", decoder->outputSampleRate);

    musicDecoder_ = decoder;

    // Create streaming sound from decoder
    musicSound_ = static_cast<ma_sound*>(std::malloc(sizeof(ma_sound)));
    if (!musicSound_) {
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        musicData_.reset();
        return false;
    }
    result = ma_sound_init_from_data_source(
        engine_,
        decoder,
        MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr,
        musicSound_
    );

    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to create music sound: ", result);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        std::free(musicSound_);
        musicSound_ = nullptr;
        musicData_.reset();
        return false;
    }

    // Set volume and looping
    ma_sound_set_volume(musicSound_, volume);
    ma_sound_set_looping(musicSound_, loop ? MA_TRUE : MA_FALSE);

    // Start playback
    result = ma_sound_start(musicSound_);
    if (result != MA_SUCCESS) {
        LOG_ERROR("Failed to start music playback: ", result);
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
        // The two failure paths above release this and this one did not, so a
        // failed start pinned the whole encoded file until the next playMusic
        // reassigned the member.
        musicData_.reset();
        return false;
    }

    LOG_INFO("Music playback started successfully - volume: ", volume,
             ", loop: ", loop,
             ", is_playing: ", ma_sound_is_playing(musicSound_));

    return true;
}

void AudioEngine::stopMusic() {
    if (musicSound_) {
        ma_sound_uninit(musicSound_);
        std::free(musicSound_);
        musicSound_ = nullptr;
    }
    if (musicDecoder_) {
        ma_decoder* decoder = static_cast<ma_decoder*>(musicDecoder_);
        ma_decoder_uninit(decoder);
        delete decoder;
        musicDecoder_ = nullptr;
    }
    musicData_.reset();
}

bool AudioEngine::isMusicPlaying() const {
    if (!musicSound_) {
        return false;
    }
    return ma_sound_is_playing(musicSound_) == MA_TRUE;
}

void AudioEngine::setMusicVolume(float volume) {
    musicVolume_ = glm::clamp(volume, 0.0f, 1.0f);
    if (musicSound_) {
        ma_sound_set_volume(musicSound_, musicVolume_);
    }
}

void AudioEngine::update(float deltaTime) {
    (void)deltaTime;

    if (!initialized_ || !engine_) {
        return;
    }
    cullByDistance();
    assignVoices();

    // Clean up finished sounds - swap-and-pop avoids the O(N) shift that
    // vector::erase does for each removal (and the ref-count atomics in
    // ActiveSound's shared_ptr made that shift noticeably more expensive).
    for (size_t i = 0; i < activeSounds_.size(); ) {
        if (!activeSounds_[i].held && !activeSounds_[i].culled && !ma_sound_is_playing(activeSounds_[i].sound)) {
            ma_sound_uninit(activeSounds_[i].sound);
            std::free(activeSounds_[i].sound);
            ma_audio_buffer* buffer = static_cast<ma_audio_buffer*>(activeSounds_[i].buffer);
            ma_audio_buffer_uninit(buffer);
            std::free(buffer);
            activeSounds_[i] = std::move(activeSounds_.back());
            activeSounds_.pop_back();
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < activeStreams_.size();) {
        if (!activeStreams_[i].held && !ma_sound_is_playing(activeStreams_[i].sound)) {
            releaseStream(activeStreams_[i]);
            activeStreams_[i] = std::move(activeStreams_.back());
            activeStreams_.pop_back();
        } else {
            ++i;
        }
    }
}

bool AudioEngine::decodeToFloat(const std::vector<uint8_t>& data, std::vector<float>& samples, uint32_t& channels,
                                uint32_t& sampleRate) {
    if (data.empty()) return false;
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(data.data(), data.size(), &config, &decoder) != MA_SUCCESS) return false;
    channels = decoder.outputChannels;
    sampleRate = decoder.outputSampleRate;
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0 ||
        frames > static_cast<ma_uint64>(sampleRate) * 60) {
        frames = static_cast<ma_uint64>(sampleRate) * 60;
    }
    samples.assign(static_cast<size_t>(frames) * channels, 0.0f);
    ma_uint64 read = 0;
    const ma_result result = ma_decoder_read_pcm_frames(&decoder, samples.data(), frames, &read);
    ma_decoder_uninit(&decoder);
    samples.resize(static_cast<size_t>(read) * channels);
    return (result == MA_SUCCESS || result == MA_AT_END) && read > 0 && channels > 0;
}

uint32_t AudioEngine::playingOf(PlayLimit limit) const {
    if (limit == PlayLimit::None) return 0;
    uint32_t playing = 0;
    for (const auto& active : activeSounds_) {
        if (active.limit == limit && ma_sound_is_playing(active.sound)) ++playing;
    }
    return playing;
}

void AudioEngine::cullByDistance() {
    for (auto& active : activeSounds_) {
        if (active.held || !(active.cutoff > 0.0f)) continue;
        const ma_vec3f p = ma_sound_get_position(active.sound);
        const bool heard = sound_distance::heard(glm::vec3(p.x, p.y, p.z), listenerPosition_, active.cutoff);
        if (heard == !active.culled) continue;
        if (!heard) {
            // A looping one pauses where it is; one played once, or one
            // already let go to fade, is over.
            ma_sound_stop(active.sound);
            active.culled = active.id != 0 && ma_sound_is_looping(active.sound) == MA_TRUE;
        } else {
            ma_sound_start(active.sound);
            active.culled = false;
        }
    }
}

void AudioEngine::noteStarted(int priority, float volume, Channel channel, bool spatial, float minDistance) {
    ActiveSound& active = activeSounds_.back();
    active.priority = priority;
    active.volume = volume;
    active.channel = channel;
    active.spatial = spatial;
    active.minDistance = minDistance;
    active.serial = nextSerial_++;
    assignVoices();
}

void AudioEngine::assignVoices() {
    // The sounds that would be heard: started and in range, by how loud
    // they reach the listener.
    std::vector<size_t> playing;
    playing.reserve(activeSounds_.size());
    for (size_t i = 0; i < activeSounds_.size(); ++i) {
        const ActiveSound& active = activeSounds_[i];
        if (!active.held && !active.culled && ma_sound_is_playing(active.sound)) playing.push_back(i);
    }
    // The music and the zone's streams hold real voices of their own.
    int reserved = musicSound_ && ma_sound_is_playing(musicSound_) ? 1 : 0;
    for (const auto& stream : activeStreams_) {
        if (!stream.held && stream.sound && ma_sound_is_playing(stream.sound)) ++reserved;
    }
    const auto real = static_cast<uint32_t>(std::max(0, maxRealVoices_ - reserved));
    std::vector<bool> heard(activeSounds_.size(), true);
    if (playing.size() > real) {
        std::sort(playing.begin(), playing.end(),
                  [&](size_t a, size_t b) { return activeSounds_[a].serial < activeSounds_[b].serial; });
        std::vector<voice_priority::Voice> voices;
        voices.reserve(playing.size());
        for (const size_t i : playing) {
            const ActiveSound& active = activeSounds_[i];
            float audibility = active.volume * (active.channel == Channel::Effects ? effectsVolume_ : 1.0f);
            if (active.spatial) {
                const ma_vec3f p = ma_sound_get_position(active.sound);
                const float distance = glm::length(glm::vec3(p.x, p.y, p.z) - listenerPosition_);
                audibility *= voice_priority::distanceGain(distance, active.minDistance, active.cutoff);
            }
            voices.push_back({active.priority, audibility});
        }
        const std::vector<bool> given = voice_priority::heard(voices, real);
        for (size_t k = 0; k < playing.size(); ++k) heard[playing[k]] = given[k];
    }
    for (size_t i = 0; i < activeSounds_.size(); ++i) {
        ActiveSound& active = activeSounds_[i];
        if (heard[i] && active.virtualVoice) {
            ma_sound_set_volume(active.sound, active.volume);
            active.virtualVoice = false;
        } else if (!heard[i] && !active.virtualVoice) {
            ma_sound_set_volume(active.sound, 0.0f);
            active.virtualVoice = true;
        }
    }
}

void AudioEngine::deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames) {
    (void)in;
    // What miniaudio's own callback does, less an Emscripten-only job pump.
    auto* engine = static_cast<ma_engine*>(device->pUserData);
    ma_engine_read_pcm_frames(engine, out, frames, nullptr);

    AudioEngine& self = instance();
    if (!self.capturing_.load(std::memory_order_acquire)) return;
    auto* ring = static_cast<ma_pcm_rb*>(self.captureRing_);
    const ma_uint32 channels = ma_engine_get_channels(engine);
    const auto* src = static_cast<const float*>(out);
    // Whatever does not fit is dropped: the recorder has fallen a second
    // behind, and the audio thread is the one thing that must never wait.
    ma_uint32 remaining = frames;
    while (remaining > 0) {
        ma_uint32 chunk = remaining;
        void* dst = nullptr;
        if (ma_pcm_rb_acquire_write(ring, &chunk, &dst) != MA_SUCCESS || chunk == 0) break;
        std::memcpy(dst, src, static_cast<size_t>(chunk) * channels * sizeof(float));
        ma_pcm_rb_commit_write(ring, chunk);
        src += static_cast<size_t>(chunk) * channels;
        remaining -= chunk;
    }
}

bool AudioEngine::beginOutputCapture() {
    if (!initialized_ || !engine_) return false;
    if (!captureRing_) {
        // A second of the device's output: the recorder drains it every few
        // tens of milliseconds, so this is room for a long stall.
        auto* ring = new ma_pcm_rb();
        if (ma_pcm_rb_init(ma_format_f32, ma_engine_get_channels(engine_),
                           ma_engine_get_sample_rate(engine_), nullptr, nullptr, ring) != MA_SUCCESS) {
            delete ring;
            LOG_WARNING("AudioEngine: could not make the capture buffer - recording without sound");
            return false;
        }
        captureRing_ = ring;
    }
    // Drop what an earlier capture left unread. The reader's side, so it is
    // safe with the audio thread writing.
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 stale = ma_pcm_rb_available_read(ring);
    if (stale > 0) ma_pcm_rb_seek_read(ring, stale);
    capturing_.store(true, std::memory_order_release);
    return true;
}

void AudioEngine::endOutputCapture() {
    capturing_.store(false, std::memory_order_release);
}

uint32_t AudioEngine::readCapturedOutput(float* dst, uint32_t maxFrames) {
    if (!captureRing_ || !dst || maxFrames == 0) return 0;
    auto* ring = static_cast<ma_pcm_rb*>(captureRing_);
    const ma_uint32 channels = ma_pcm_rb_get_channels(ring);
    // Frames are interleaved: one float per channel each.
    uint32_t framesRead = 0;
    while (framesRead < maxFrames) {
        ma_uint32 chunkFrames = maxFrames - framesRead;
        void* ringFrames = nullptr;
        if (ma_pcm_rb_acquire_read(ring, &chunkFrames, &ringFrames) != MA_SUCCESS || chunkFrames == 0) break;
        float* nextFrame = dst + static_cast<size_t>(framesRead) * channels;
        std::memcpy(nextFrame, ringFrames, static_cast<size_t>(chunkFrames) * channels * sizeof(float));
        ma_pcm_rb_commit_read(ring, chunkFrames);
        framesRead += chunkFrames;
    }
    return framesRead;
}

uint32_t AudioEngine::capturedOutputAvailable() const {
    if (!captureRing_) return 0;
    return ma_pcm_rb_available_read(static_cast<ma_pcm_rb*>(captureRing_));
}

uint32_t AudioEngine::getOutputChannels() const {
    return engine_ ? ma_engine_get_channels(engine_) : 0;
}

uint32_t AudioEngine::getOutputSampleRate() const {
    return engine_ ? ma_engine_get_sample_rate(engine_) : 0;
}

} // namespace audio
} // namespace wowee

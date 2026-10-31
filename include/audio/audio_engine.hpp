#pragma once

#include <glm/glm.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "audio/play_limit.hpp"
#include "audio/voice_priority.hpp"
#include "audio/world_sound_hold.hpp"

// Forward declare miniaudio types to avoid exposing implementation in header
struct ma_engine;
struct ma_sound;
struct ma_device;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

/// Which of the client's volume channels a sound is heard on (0x004c6a40
/// checks the play's type against 0x009f2010's names: "SFX", "MUSIC",
/// "AMBIENCE", ...). `Own` is a caller that scales its sounds itself - the
/// sound managers, each with its slider times the effects volume, and the
/// ambience and music with theirs. `Effects` is an SFX sound from a caller
/// that keeps no volume of its own - a model's events, a spell kit's, the
/// server's SMSG_PLAY_SOUND - heard at the Sound Effects volume and not at
/// all with Sound_EnableSFX off (0x004c6a40 returns 9 for an SFX play then).
enum class Channel : uint8_t { Own, Effects };

/**
 * AudioEngine: Singleton managing miniaudio device and playback.
 * Replaces process-spawning audio system with proper non-blocking library.
 */
class AudioEngine {
public:
    static AudioEngine& instance();

    /// The decoded sounds kept for replay, for /mem.
    struct DecodedCacheStats {
        size_t sounds = 0;
        uint64_t bytes = 0;
    };
    static DecodedCacheStats decodedCacheStats();

    ~AudioEngine();

    // Initialization
    [[nodiscard]] bool initialize();
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized_; }

    /// The playback device miniaudio actually opened, by name.
    ///
    /// For the Sound panel's output dropdown, which lists the drivers the
    /// client can use. This client opens whichever device the system offers
    /// and does not switch between them, so the list is this one name - the
    /// truth rather than a guess, and the same shape the video panel's
    /// resolution list already takes. Empty if the engine is not up.
    [[nodiscard]] std::string getOutputDeviceName() const;

    // Master volume (0.0 = silent, 1.0 = full)
    void setMasterVolume(float volume);
    [[nodiscard]] float getMasterVolume() const { return masterVolume_; }

    /// Silence the output without forgetting the volume the player chose.
    ///
    /// For the window losing focus, where the client should go quiet and come
    /// back at the same level. Winding masterVolume_ down to zero instead
    /// would work once and then answer the volume slider wrongly, and would
    /// trip the masterVolume_ <= 0 early-outs that stop a sound being started
    /// at all - so a suspended client would come back to silence where a
    /// looping track used to be.
    void setSuspended(bool suspended);
    [[nodiscard]] bool isSuspended() const { return suspended_; }

    // Asset manager (enables sound loading by MPQ path)
    void setAssetManager(pipeline::AssetManager* am) { assetManager_ = am; }

    // 3D listener position (for positional audio)
    void setListenerPosition(const glm::vec3& position);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);
    [[nodiscard]] const glm::vec3& getListenerPosition() const { return listenerPosition_; }

    // Simple 2D sound playback (non-blocking)
    /// A `limit` play is refused while as many of its type as the client
    /// allows are playing (audio/play_limit.hpp). `priority` is FMOD's:
    /// past the real voices the least important play on silent
    /// (audio/voice_priority.hpp). `outHandle`, where given, takes the id to
    /// ask about with isSoundPlaying(): the handle a play went into, which the
    /// client hands the caller's own slot (0x004c6a40's third argument). It is
    /// 0 where nothing started.
    bool playSound2D(const std::vector<uint8_t>& wavData, float volume = 1.0f, float pitch = 1.0f,
                     Channel channel = Channel::Own, PlayLimit limit = PlayLimit::None,
                     int priority = voice_priority::kDefault, uint32_t* outHandle = nullptr);
    bool playSound2D(const std::string& mpqPath, float volume = 1.0f, float pitch = 1.0f,
                     Channel channel = Channel::Own, int priority = voice_priority::kDefault);

    // Stoppable 2D sound - returns a non-zero handle, or 0 on failure.
    /// `loop` plays it until stopped; it rises from silence over
    /// `fadeInSeconds` (the client's fade-in, 0x004c6a40 -> 0x008785f0).
    uint32_t playSound2DStoppable(const std::vector<uint8_t>& wavData, float volume = 1.0f, bool loop = false,
                                  float fadeInSeconds = 0.0f);
    // Stop a sound started with playSound2DStoppable (no-op if already finished)
    void stopSound(uint32_t id);

    // 3D positional sound playback
    /// `minDistance` is how far out it plays at full volume, `maxDistance`
    /// where it stops getting quieter - a SoundEntries row's MinDistance and
    /// DistanceCutoff, the client's 3D min and max (0x004c6a40 -> 0x00879390).
    bool playSound3D(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                     float minDistance = 1.0f, Channel channel = Channel::Own,
                     PlayLimit limit = PlayLimit::None, int priority = voice_priority::kDefault,
                     uint32_t* outHandle = nullptr);
    /// Whether the play `handle` was given is still going. False once it has
    /// ended and update() has taken it away, and for handle 0 - the same
    /// question the client asks of the slot a unit's line went into
    /// (0x004c5be0), which is what keeps a unit saying one line at a time.
    [[nodiscard]] bool isSoundPlaying(uint32_t handle) const;
    bool playSound3D(const std::string& mpqPath, const glm::vec3& position,
                     float volume = 1.0f, float pitch = 1.0f, float maxDistance = 100.0f,
                     Channel channel = Channel::Own);

    /// A looping 3D sound that stays until stopped, for one that follows
    /// something across the world - a spell missile's (0x007022d0). Returns
    /// a non-zero handle, or 0 on failure. It rises from silence over
    /// `fadeInSeconds` (the client's fade-in, 0x004c6a40 -> 0x008785f0).
    uint32_t playSound3DLooping(const std::vector<uint8_t>& wavData, const glm::vec3& position,
                                float volume = 1.0f, float maxDistance = 100.0f,
                                float minDistance = 1.0f, float fadeInSeconds = 0.0f,
                                Channel channel = Channel::Own, int priority = voice_priority::kDefault);
    /// Move a sound started with a handle (no-op once it has finished).
    void setSoundPosition(uint32_t id, const glm::vec3& position);
    /// Change the volume of a sound started with a handle (no-op once it has
    /// finished or is fading out).
    void setSoundVolume(uint32_t id, float volume);
    /// Stop a sound started with a handle over `seconds`, after which it is
    /// cleaned up like a sound that ended (no-op once it has finished).
    void stopSoundWithFade(uint32_t id, float seconds);

    /// A whole clip decoded to interleaved float samples, for a caller that
    /// works on them before playing (audio/sound_filter.hpp). False when it
    /// will not decode.
    static bool decodeToFloat(const std::vector<uint8_t>& data, std::vector<float>& samples,
                              uint32_t& channels, uint32_t& sampleRate);

    // Music streaming (for background music)
    // Retains shared ownership: the decoder streams directly from encoded tracks that
    // run to several MB, so cached music must not be copied for every playback.
    bool playMusic(std::shared_ptr<const std::vector<uint8_t>> musicData,
                   float volume = 1.0f, bool loop = true);
    void stopMusic();
    [[nodiscard]] bool isMusicPlaying() const;
    void setMusicVolume(float volume);

    /// A track streamed from its encoded bytes, any number at once - the
    /// zone music's channel pair, whose two tracks cross (0x004c7140).
    /// Returns a non-zero handle, or 0 on failure. It plays once unless
    /// `loop`, and is taken away by update() once it has ended.
    uint32_t playStream(std::shared_ptr<const std::vector<uint8_t>> data, float volume, bool loop);
    [[nodiscard]] bool isStreamPlaying(uint32_t id) const;
    void setStreamVolume(uint32_t id, float volume);
    /// Stops it at once (no-op once it has ended).
    void stopStream(uint32_t id);

    /// The Sound Effects volume, 0 with Sound_EnableSFX off: what the
    /// Effects channel is heard at.
    void setEffectsVolume(float volume);

    /// How many sounds are heard at once - Sound_NumChannels, held to
    /// 12..128 (0x0087c710); the rest play on silent until a voice is free,
    /// the least important first (audio/voice_priority.hpp).
    void setMaxRealVoices(int voices) { maxRealVoices_ = voice_priority::clampRealVoices(voices); }
    [[nodiscard]] int getMaxRealVoices() const { return maxRealVoices_; }
    [[nodiscard]] float getEffectsVolume() const { return effectsVolume_; }

    /// The world's sound held while a loading screen is up. The client does
    /// not step the world behind its loading screen - its loop only pumps
    /// the sound system and draws (0x0040a920) - so nothing in the world
    /// starts a sound until the screen goes. Here the world is stepped
    /// behind it, so the hold does what not stepping does: a one-shot asked
    /// for is not played, a looping sound or a stream waits silent and
    /// starts once the last hold is let go (audio/world_sound_hold.hpp).
    /// What was playing before the screen came up plays on. Counted, so
    /// two loading screens at once hold until both are gone.
    void beginWorldSoundHold();
    void endWorldSoundHold();
    [[nodiscard]] bool isWorldSoundHeld() const { return worldSoundHolds_.held(); }

    // Update (call once per frame for cleanup/position sync)
    void update(float deltaTime);

    /// Keep a copy of what the speakers play, for the screen recorder.
    ///
    /// Taken in the device's own callback after the mix and the master volume,
    /// so a recording sounds as the session did - muted or suspended included.
    /// Anything captured before this call is discarded. False when there is no
    /// device to listen to, in which case a recording goes without sound.
    bool beginOutputCapture();
    void endOutputCapture();
    /// Up to maxFrames interleaved float frames captured since the last read,
    /// oldest first. One reader at a time; any thread.
    uint32_t readCapturedOutput(float* dst, uint32_t maxFrames);
    /// Frames captured and not yet read.
    [[nodiscard]] uint32_t capturedOutputAvailable() const;
    [[nodiscard]] uint32_t getOutputChannels() const;
    [[nodiscard]] uint32_t getOutputSampleRate() const;

private:
    AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Track active one-shot sounds for cleanup
    struct ActiveSound {
        ma_sound* sound;
        void* buffer;  // ma_audio_buffer* - Keep audio buffer alive
        std::shared_ptr<const std::vector<uint8_t>> pcmDataRef;  // Keep decoded PCM alive
        uint32_t id = 0;  // 0 = anonymous (not stoppable)
        bool held = false;  // waiting, not started, for the world sound hold to end
        float cutoff = 0.0f;  // a 3D sound's DistanceCutoff: not heard past it (0: none)
        bool culled = false;  // a looping one paused out past its cutoff
        PlayLimit limit = PlayLimit::None;  // the counted type it was played as
        int priority = voice_priority::kDefault;  // FMOD's: lower is more important
        float volume = 1.0f;          // what it was asked to play at
        bool virtualVoice = false;    // playing on silent, without a real voice
        bool spatial = false;         // a 3D sound: heard less with distance
        float minDistance = 1.0f;     // a 3D sound's full-volume reach
        Channel channel = Channel::Own;
        uint64_t serial = 0;          // the order it started in
    };
    std::vector<ActiveSound> activeSounds_;
    /// How many of a counted type are still playing - one that has ended is
    /// not, though update() has yet to take it away (0x004c62b0 counts it
    /// off as it ends).
    [[nodiscard]] uint32_t playingOf(PlayLimit limit) const;
    /// Pauses what is out past its cutoff and resumes what has come back in
    /// (audio/sound_distance.hpp), once a frame.
    void cullByDistance();
    /// Gives the real voices to the most important sounds playing and
    /// silences the rest (audio/voice_priority.hpp), each frame and as a
    /// sound starts.
    void assignVoices();
    /// The sound just tracked: its priority, volume and reach, then the
    /// voices given out again.
    void noteStarted(int priority, float volume, Channel channel, bool spatial, float minDistance);
    int maxRealVoices_ = voice_priority::kDefaultRealVoices;
    uint64_t nextSerial_ = 1;
    uint32_t nextSoundId_ = 1;

    // Music track state
    ma_sound* musicSound_ = nullptr;
    void* musicDecoder_ = nullptr;  // ma_decoder* - Keep decoder alive for streaming
    std::shared_ptr<const std::vector<uint8_t>> musicData_;  // Keep encoded music data alive
    float musicVolume_ = 1.0f;

    struct ActiveStream {
        ma_sound* sound = nullptr;
        void* decoder = nullptr;  // ma_decoder*
        std::shared_ptr<const std::vector<uint8_t>> data;
        uint32_t id = 0;
        bool held = false;  // waiting, not started, for the world sound hold to end
    };
    std::vector<ActiveStream> activeStreams_;
    static void releaseStream(ActiveStream& stream);

    bool initialized_ = false;
    float masterVolume_ = 1.0f;
    float effectsVolume_ = 1.0f;
    world_sound_hold::Count worldSoundHolds_;
    /// Starts a sound just made, or leaves it waiting under the hold.
    bool startOrHold(ma_sound* sound, bool loops, bool& held);
    /// The Effects channel's group (an ma_sound_group is an ma_sound).
    ma_sound* effectsGroup_ = nullptr;
    ma_sound* groupFor(Channel channel) const;
    bool suspended_ = false;
    glm::vec3 listenerPosition_{0.0f, 0.0f, 0.0f};
    glm::vec3 listenerForward_{0.0f, 0.0f, -1.0f};
    glm::vec3 listenerUp_{0.0f, 1.0f, 0.0f};

    pipeline::AssetManager* assetManager_ = nullptr;

    // miniaudio engine (opaque pointer)
    ma_engine* engine_ = nullptr;

    /// Replaces miniaudio's own device callback: it mixes exactly as that one
    /// does, then hands the result to the capture ring when one is open.
    static void deviceDataCallback(ma_device* device, void* out, const void* in, uint32_t frames);
    // ma_pcm_rb, which miniaudio declares anonymously and so cannot be named
    // here. Made on the first capture and kept until shutdown: freeing it while
    // the audio thread might still be writing is the one race not worth having.
    void* captureRing_ = nullptr;
    std::atomic<bool> capturing_{false};
};

} // namespace audio
} // namespace wowee

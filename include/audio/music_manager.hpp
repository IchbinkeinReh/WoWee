#pragma once

#include <string>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/zone_music.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

/// The music: the login screen's track, and the zone music's channel pair
/// (0xb4ac90, 0x004c7140) the slots pick for (audio/zone_music.hpp). Both
/// are heard at the music volume (Sound_MusicVolume).
class MusicManager {
public:
    MusicManager();
    ~MusicManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void playMusic(const std::string& mpqPath, bool loop = true, float fadeInMs = 0.0f);
    void playFilePath(const std::string& filePath, bool loop = true, float fadeInMs = 0.0f);
    void stopMusic(float fadeMs = 2000.0f);

    void update(float deltaTime);
    void setVolume(int volume);
    [[nodiscard]] int getVolume() const { return volumePercent; }

    [[nodiscard]] bool isPlaying() const { return playing; }
    /// True while a track is being read on the worker but has not started yet.
    /// Callers must treat this as "in progress", not as failure.
    [[nodiscard]] bool isLoading() const { return pendingFileLoad_.has_value(); }
    [[nodiscard]] bool isInitialized() const { return assetManager != nullptr; }

    // --- The zone music's channel pair (0xb4ac90) ---

    /// What a track hands back at its end: what it was started with
    /// (0x004c9910 -> 0x004c5d20).
    struct ZoneTrackEnd {
        int slot = zone_music::kNoSlot;
        int32_t sound = 0;
        uint32_t silenceMin = 0;
        uint32_t silenceMax = 0;
    };
    /// 0x004c7140: the channel given `sound` for `slot`. One of the sound's
    /// files rises over `fadeIn` on the record not current, which becomes
    /// current; the one current falls over its own fall, the other's at once
    /// or over its own. What the current record plays already is left alone;
    /// what the other is still falling with rises again over `fadeIn`. True
    /// when a track was started: `end` comes back when it ends.
    bool playZoneTrack(int32_t sound, int slot, float fadeIn, float fadeOut, const ZoneTrackEnd& end);
    /// 0x004c7140 given nothing (a slot past 10): the current track falls
    /// over its own fall and its record holds `slot` and no sound.
    void releaseZoneTrack(int slot);
    /// 0x004c6450: both records' tracks, at once or over their own falls.
    /// Their ends are not handed back.
    void stopZoneMusic(bool atOnce);
    /// The current record as the slots see it (0x004c5d00, 0x004c6410,
    /// 0x004c5ce0).
    [[nodiscard]] zone_music::Channel zoneChannel() const;
    /// The tracks that have ended since the last call.
    std::vector<ZoneTrackEnd> takeEndedZoneTracks();

private:
    [[nodiscard]] float effectiveMusicVolume() const;

    // Tracks run to several MB, so reading one on the calling thread stalls whatever
    // frame asked for it - starting the login music was costing ~200ms of render time.
    // The read happens on a worker; update() hands the bytes to the AudioEngine once
    // they land, keeping every miniaudio call on the main thread.
    struct PendingFileLoad {
        std::future<std::shared_ptr<const std::vector<uint8_t>>> future;
        std::string path;
        bool loop = true;
        float fadeInMs = 0.0f;
    };
    std::optional<PendingFileLoad> pendingFileLoad_;
    void cancelPendingFileLoad();
    void pollPendingFileLoad();

    pipeline::AssetManager* assetManager = nullptr;
    std::string currentTrack;
    bool playing = false;
    int volumePercent = 30;

    bool fadingIn = false;
    float fadeInTimer = 0.0f;
    float fadeInDuration = 0.0f;
    float fadeInTargetVolume = 0.0f;
    // Fade-out state (for stopMusic with fadeMs > 0)
    bool fadingOut = false;
    float fadeOutTimer = 0.0f;
    float fadeOutDuration = 0.0f;
    float fadeOutStartVolume = 0.0f;

    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> musicDataCache_;

    /// A SoundEntries row as the music plays it (0x004c6a40): its files
    /// (DirectoryBase and File), their Freq, VolumeFloat, and whether Flags
    /// loops it; and the file played last (0x004cbac0 keeps it).
    struct ZoneKit {
        std::vector<std::string> paths;
        std::vector<uint32_t> frequency;
        float volume = 1.0f;
        bool loop = false;
        std::optional<size_t> last;
    };
    ZoneKit* zoneKit(int32_t sound);
    std::unordered_map<int32_t, ZoneKit> zoneKits_;
    /// Kit files that were read and would not play: asked for again, they
    /// fail at once, as the client's open of a missing file does each frame.
    std::unordered_set<std::string> unplayableZoneFiles_;

    /// A track of the pair: read on a worker, then streamed. Its level rises
    /// or falls linearly; one fallen to nothing stops.
    struct ZoneTrack {
        std::future<std::shared_ptr<const std::vector<uint8_t>>> pending;
        bool loading = false;
        uint32_t stream = 0;
        bool loop = false;
        float kitVolume = 1.0f;
        float level = 0.0f;
        float rate = 0.0f;  ///< level per second, + rising, - falling
        float fall = 0.0f;  ///< the seconds it falls over (0x00878610)
        ZoneTrackEnd end;
        std::string path;
    };
    struct ZoneRecord {
        int32_t sound = 0;
        int slot = zone_music::kNoSlot;
        uint32_t track = 0;
    };
    [[nodiscard]] bool trackPlaying(uint32_t id) const;
    /// 0x00879410: the track falls over its own fall, at once without one.
    void fallTrack(uint32_t id);
    void stopTrack(uint32_t id, bool report);
    /// 0x004c6390: the record lets its track go - at once, or falling.
    void detach(ZoneRecord& record, bool atOnce);
    void updateZoneTracks(float deltaTime);
    std::unordered_map<uint32_t, ZoneTrack> zoneTracks_;
    /// The reads of tracks stopped while still loading. Each holds the
    /// AssetManager by pointer, so shutdown() waits them out before the
    /// assets go; update() lets those finished go.
    std::vector<std::future<std::shared_ptr<const std::vector<uint8_t>>>> abandonedZoneReads_;
    uint32_t nextZoneTrack_ = 1;
    ZoneRecord zoneRecords_[2];
    int zoneCurrent_ = 0;  ///< +0x38; the other is +0x3c
    std::vector<ZoneTrackEnd> endedZoneTracks_;
};

} // namespace audio
} // namespace wowee

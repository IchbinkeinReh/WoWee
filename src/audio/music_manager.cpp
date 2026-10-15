#include "audio/music_manager.hpp"

#include <chrono>
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/thread_pool.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace wowee {
namespace audio {

MusicManager::MusicManager() = default;

MusicManager::~MusicManager() {
    shutdown();
}

bool MusicManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    LOG_INFO("Music manager initialized");
    return true;
}

float MusicManager::effectiveMusicVolume() const {
    return volumePercent / 100.0f;
}

void MusicManager::shutdown() {
    cancelPendingFileLoad();
    // A zone track's read holds the AssetManager by pointer, and dropping a
    // pool future does not join its worker - the application frees the
    // assets right after this. Wait the reads out first, as the spawner does
    // its loads (EntitySpawner::resetAllState).
    for (auto& [id, track] : zoneTracks_) {
        if (track.pending.valid()) track.pending.wait();
    }
    for (auto& read : abandonedZoneReads_) {
        if (read.valid()) read.wait();
    }
    abandonedZoneReads_.clear();
    AudioEngine::instance().stopMusic();
    playing = false;
    fadingIn = false;
    fadingOut = false;
    fadeInTimer = 0.0f;
    fadeInDuration = 0.0f;
    fadeInTargetVolume = 0.0f;
    currentTrack.clear();
    musicDataCache_.clear();
    stopZoneMusic(true);
    zoneKits_.clear();
    endedZoneTracks_.clear();
    assetManager = nullptr;
}

void MusicManager::playMusic(const std::string& mpqPath, bool loop, float fadeInMs) {
    if (!assetManager) return;
    if (mpqPath == currentTrack && playing) return;

    // Check if AudioEngine is ready
    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    // A newer request always wins, regardless of whether it comes from an archive
    // or the filesystem. Pool-backed futures can be discarded without joining.
    cancelPendingFileLoad();

    // Read music file from cache or MPQ
    auto cacheIt = musicDataCache_.find(mpqPath);
    if (cacheIt == musicDataCache_.end()) {
        auto data = assetManager->readFile(mpqPath);
        if (!data.empty())
            musicDataCache_[mpqPath] = std::make_shared<const std::vector<uint8_t>>(std::move(data));
        cacheIt = musicDataCache_.find(mpqPath);
    }
    if (cacheIt == musicDataCache_.end() || !cacheIt->second || cacheIt->second->empty()) {
        LOG_WARNING("Music: Could not read: ", mpqPath);
        return;
    }

    // Play with AudioEngine (non-blocking, streams from memory)
    float targetVolume = effectiveMusicVolume();
    float startVolume = (fadeInMs > 0.0f) ? 0.0f : targetVolume;
    if (AudioEngine::instance().playMusic(cacheIt->second, startVolume, loop)) {
        playing = true;
        fadingIn = false;
        if (fadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, fadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = mpqPath;
        // A track started ends any fade of the last: the engine has one.
        fadingOut = false;
        LOG_INFO("Music: Playing ", mpqPath);
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::playFilePath(const std::string& filePath, bool loop, float fadeInMs) {
    if (filePath.empty()) return;
    if (filePath == currentTrack && playing) return;
    if (!std::filesystem::exists(filePath)) {
        LOG_WARNING("Music: file not found: ", filePath);
        return;
    }

    // Check if AudioEngine is ready
    if (!AudioEngine::instance().isInitialized()) {
        LOG_WARNING("Music: AudioEngine not initialized");
        return;
    }

    // Reuse an in-flight read of the same file while honoring the newest playback
    // options. A different request replaces it without waiting for the old read.
    if (pendingFileLoad_ && pendingFileLoad_->path == filePath) {
        pendingFileLoad_->loop = loop;
        pendingFileLoad_->fadeInMs = fadeInMs;
        return;
    }

    // Read the track on a worker. A login track is ~6MB, and reading it here stalled
    // the render frame that asked for it. pollPendingFileLoad() picks it up from
    // update(), so every AudioEngine call still happens on the main thread.
    pendingFileLoad_ = PendingFileLoad{
        core::ThreadPool::ioWorkers().submit([filePath]()
                -> std::shared_ptr<const std::vector<uint8_t>> {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file) return nullptr;
            std::streamsize size = file.tellg();
            if (size <= 0) return nullptr;
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }),
        filePath, loop, fadeInMs
    };
}

void MusicManager::cancelPendingFileLoad() {
    // Futures returned by ThreadPool own only a shared packaged task state; unlike
    // std::async futures, destroying one never joins the worker.
    pendingFileLoad_.reset();
}

void MusicManager::pollPendingFileLoad() {
    if (!pendingFileLoad_) return;
    auto& pending = *pendingFileLoad_;
    if (!pending.future.valid()) { pendingFileLoad_.reset(); return; }
    if (pending.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    auto data = pending.future.get();
    const std::string filePath = pending.path;
    const bool loop = pending.loop;
    const float fadeInMs = pending.fadeInMs;
    pendingFileLoad_.reset();

    if (!data || data->empty()) {
        LOG_ERROR("Music: Could not read file: ", filePath);
        return;
    }

    // Play with AudioEngine
    float targetVolume = effectiveMusicVolume();
    float startVolume = (fadeInMs > 0.0f) ? 0.0f : targetVolume;
    if (AudioEngine::instance().playMusic(std::move(data), startVolume, loop)) {
        playing = true;
        fadingIn = false;
        if (fadeInMs > 0.0f) {
            fadingIn = true;
            fadeInTimer = 0.0f;
            fadeInDuration = std::max(0.05f, fadeInMs / 1000.0f);
            fadeInTargetVolume = targetVolume;
            AudioEngine::instance().setMusicVolume(0.0f);
        }
        currentTrack = filePath;
        fadingOut = false;
        LOG_INFO("Music: Playing file ", filePath);
    } else {
        LOG_ERROR("Music: Failed to play music via AudioEngine");
    }
}

void MusicManager::stopMusic(float fadeMs) {
    cancelPendingFileLoad();
    fadingIn = false;
    if (!playing) return;

    if (fadeMs > 0.0f) {
        // Begin fade-out; actual stop happens once volume reaches zero in update()
        fadingOut = true;
        fadeOutTimer = 0.0f;
        fadeOutDuration = fadeMs / 1000.0f;
        fadeOutStartVolume = effectiveMusicVolume();
    } else {
        AudioEngine::instance().stopMusic();
        // A fade-out cut short leaves the volume where it got to; the next
        // track starts from the player's own.
        if (fadingOut) AudioEngine::instance().setMusicVolume(effectiveMusicVolume());
        playing = false;
        fadingOut = false;
        currentTrack.clear();
    }
}

void MusicManager::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volumePercent == volume) return;
    volumePercent = volume;

    // Update AudioEngine music volume directly (no restart needed!)
    float vol = effectiveMusicVolume();
    if (fadingIn) {
        fadeInTargetVolume = vol;
        float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeInTargetVolume * t);
    } else {
        AudioEngine::instance().setMusicVolume(vol);
    }
}

void MusicManager::update(float deltaTime) {
    updateZoneTracks(deltaTime);
    // Hand off any background-loaded track before touching playback state.
    pollPendingFileLoad();

    // Check if music is still playing
    if (playing && !AudioEngine::instance().isMusicPlaying()) {
        playing = false;
    }

    if (fadingOut) {
        fadeOutTimer += deltaTime;
        float t = std::clamp(1.0f - fadeOutTimer / std::max(fadeOutDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeOutStartVolume * t);
        if (t <= 0.0f) {
            // Fade complete - stop playback and restore volume for next track
            fadingOut = false;
            AudioEngine::instance().stopMusic();
            AudioEngine::instance().setMusicVolume(effectiveMusicVolume());
            playing = false;
            currentTrack.clear();
        }
        return;  // Don't process other fade logic while fading out
    }

    if (fadingIn) {
        fadeInTimer += deltaTime;
        float t = std::clamp(fadeInTimer / std::max(fadeInDuration, 0.001f), 0.0f, 1.0f);
        AudioEngine::instance().setMusicVolume(fadeInTargetVolume * t);
        if (t >= 1.0f) {
            fadingIn = false;
        }
    }
}

// ---------------------------------------------------------------------------
// The zone music's channel pair (0xb4ac90).

MusicManager::ZoneKit* MusicManager::zoneKit(int32_t sound) {
    if (sound <= 0 || !assetManager) return nullptr;
    auto it = zoneKits_.find(sound);
    if (it == zoneKits_.end()) {
        ZoneKit kit;
        // SoundEntries.dbc: 3..12 File, 13..22 Freq, 23 DirectoryBase,
        // 24 VolumeFloat, 25 Flags.
        auto dbc = assetManager->loadDBCOptional("SoundEntries.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() > 25
                                ? dbc->findRecordById(static_cast<uint32_t>(sound))
                                : -1;
        if (idx >= 0) {
            const auto row = static_cast<uint32_t>(idx);
            const std::string dir = dbc->getString(row, 23);
            for (uint32_t f = 0; f < 10; ++f) {
                const std::string name = dbc->getString(row, 3 + f);
                if (name.empty()) continue;
                kit.paths.push_back(dir.empty() ? name : dir + "\\" + name);
                kit.frequency.push_back(dbc->getUInt32(row, 13 + f));
            }
            const float volume = dbc->getFloat(row, 24);
            if (volume > 0.0f) kit.volume = volume;
            kit.loop = (dbc->getUInt32(row, 25) & zone_music::kSoundLoops) != 0;
        }
        // Remembered either way, so a row without a file is read once.
        it = zoneKits_.emplace(sound, std::move(kit)).first;
    }
    return it->second.paths.empty() ? nullptr : &it->second;
}

bool MusicManager::trackPlaying(uint32_t id) const {
    const auto it = zoneTracks_.find(id);
    if (it == zoneTracks_.end()) return false;
    return it->second.loading || AudioEngine::instance().isStreamPlaying(it->second.stream);
}

void MusicManager::fallTrack(uint32_t id) {
    const auto it = zoneTracks_.find(id);
    if (it == zoneTracks_.end()) return;
    if (it->second.fall <= 0.0f) {
        stopTrack(id, true);
        return;
    }
    it->second.rate = -1.0f / it->second.fall;
}

void MusicManager::stopTrack(uint32_t id, bool report) {
    const auto it = zoneTracks_.find(id);
    if (it == zoneTracks_.end()) return;
    AudioEngine::instance().stopStream(it->second.stream);
    if (report) endedZoneTracks_.push_back(it->second.end);
    if (it->second.pending.valid()) abandonedZoneReads_.push_back(std::move(it->second.pending));
    zoneTracks_.erase(it);
}

void MusicManager::detach(ZoneRecord& record, bool atOnce) {
    // 0x00879840: at once when told to or without a fall of its own.
    if (atOnce) stopTrack(record.track, true);
    else fallTrack(record.track);
    record.track = 0;
}

bool MusicManager::playZoneTrack(int32_t sound, int slot, float fadeIn, float fadeOut, const ZoneTrackEnd& end) {
    ZoneRecord& current = zoneRecords_[zoneCurrent_];
    ZoneRecord& other = zoneRecords_[1 - zoneCurrent_];
    if (sound == current.sound && trackPlaying(current.track)) return false;
    if (sound == other.sound && trackPlaying(other.track)) {
        // Still falling: it rises again and the current falls.
        fallTrack(current.track);
        if (auto it = zoneTracks_.find(other.track); it != zoneTracks_.end())
            it->second.rate = fadeIn > 0.0f ? 1.0f / fadeIn : 1000.0f;
        zoneCurrent_ = 1 - zoneCurrent_;
        return false;
    }
    detach(other, false);
    fallTrack(current.track);

    // 0x004c6a40 on the other record: a file of the sound, if it has one.
    ZoneKit* kit = zoneKit(sound);
    if (!kit || !assetManager) return false;
    static std::mt19937 rng{std::random_device{}()};
    const auto file = zone_music::pickFile(kit->frequency, kit->last, static_cast<uint32_t>(rng()));
    if (!file) return false;
    kit->last = file;

    ZoneTrack track;
    track.loading = true;
    track.loop = kit->loop;
    track.kitVolume = kit->volume;
    track.level = fadeIn > 0.0f ? 0.0f : 1.0f;
    track.rate = fadeIn > 0.0f ? 1.0f / fadeIn : 0.0f;
    track.fall = fadeOut;
    track.end = end;
    auto* assets = assetManager;
    track.pending = core::ThreadPool::ioWorkers().submit(
        [assets, path = kit->paths[*file]]() -> std::shared_ptr<const std::vector<uint8_t>> {
            auto bytes = assets->readFile(path);
            if (bytes.empty()) return nullptr;
            return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        });
    const uint32_t id = nextZoneTrack_++;
    if (nextZoneTrack_ == 0) nextZoneTrack_ = 1;
    zoneTracks_.emplace(id, std::move(track));
    other = ZoneRecord{sound, slot, id};
    zoneCurrent_ = 1 - zoneCurrent_;
    return true;
}

void MusicManager::releaseZoneTrack(int slot) {
    ZoneRecord& current = zoneRecords_[zoneCurrent_];
    detach(current, false);
    current.slot = slot;
    current.sound = 0;
}

void MusicManager::stopZoneMusic(bool atOnce) {
    for (auto& record : zoneRecords_) {
        if (atOnce) {
            stopTrack(record.track, false);
            record.track = 0;
        } else {
            detach(record, false);
        }
        record = ZoneRecord{};
    }
    if (atOnce) {
        // What fell before goes with them.
        for (auto& [id, track] : zoneTracks_) {
            AudioEngine::instance().stopStream(track.stream);
            if (track.pending.valid()) abandonedZoneReads_.push_back(std::move(track.pending));
        }
        zoneTracks_.clear();
    }
}

zone_music::Channel MusicManager::zoneChannel() const {
    const ZoneRecord& current = zoneRecords_[zoneCurrent_];
    return zone_music::Channel{trackPlaying(current.track), current.sound, current.slot};
}

std::vector<MusicManager::ZoneTrackEnd> MusicManager::takeEndedZoneTracks() {
    std::vector<ZoneTrackEnd> out;
    out.swap(endedZoneTracks_);
    return out;
}

void MusicManager::updateZoneTracks(float deltaTime) {
    std::erase_if(abandonedZoneReads_, [](const auto& read) {
        return !read.valid() || read.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    auto& engine = AudioEngine::instance();
    std::vector<uint32_t> ended;
    for (auto& [id, track] : zoneTracks_) {
        if (track.loading) {
            if (!track.pending.valid() ||
                track.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto data = track.pending.valid() ? track.pending.get() : nullptr;
                track.loading = false;
                if (data) track.stream = engine.playStream(std::move(data), 0.0f, track.loop);
                if (track.stream == 0) LOG_WARNING("Music: a zone track could not be played");
            }
        }
        track.level = std::clamp(track.level + track.rate * deltaTime, 0.0f, 1.0f);
        if (track.level >= 1.0f && track.rate > 0.0f) track.rate = 0.0f;
        const bool fallen = track.rate < 0.0f && track.level <= 0.0f;
        if (fallen || (!track.loading && !engine.isStreamPlaying(track.stream))) {
            ended.push_back(id);
            continue;
        }
        engine.setStreamVolume(track.stream, track.kitVolume * track.level * effectiveMusicVolume());
    }
    for (uint32_t id : ended) {
        stopTrack(id, true);
        for (auto& record : zoneRecords_)
            if (record.track == id) record.track = 0;
    }
}

} // namespace audio
} // namespace wowee

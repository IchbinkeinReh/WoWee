#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <numeric>
#include <random>

namespace wowee {
namespace audio {

namespace za = zone_ambience;

bool AmbientSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("AmbientSoundManager: AssetManager is null");
        return false;
    }
    assets_ = assets;
    initialized_ = true;
    return true;
}

void AmbientSoundManager::shutdown() {
    stopAll();
    kits_.clear();
    initialized_ = false;
}

void AmbientSoundManager::stopAll() {
    auto& engine = AudioEngine::instance();
    // 0x004c6450(1): both of the channel's sounds, at once.
    engine.stopSound(current_.handle);
    engine.stopSound(fadingHandle_);
    current_ = {};
    fadingHandle_ = 0;
    slots_ = {};
    cameraLiquid_ = 0;
    switchAtOnce_ = false;
}

const AmbientSoundManager::Kit* AmbientSoundManager::kit(int32_t soundId) {
    if (soundId <= 0 || !assets_) return nullptr;
    auto it = kits_.find(soundId);
    if (it == kits_.end()) {
        Kit loaded;
        auto dbc = assets_->loadDBCOptional("SoundEntries.dbc");
        const int32_t idx = dbc && dbc->isLoaded() ? dbc->findRecordById(static_cast<uint32_t>(soundId)) : -1;
        if (idx >= 0 && dbc->getFieldCount() > 27) {
            const auto row = static_cast<uint32_t>(idx);
            const std::string dir = dbc->getString(row, 23);
            for (uint32_t f = 0; f < 10; ++f) {
                const std::string name = dbc->getString(row, 3 + f);
                if (name.empty()) continue;
                auto data = assets_->readFile(dir.empty() ? name : dir + "\\" + name);
                if (data.empty()) continue;
                loaded.files.push_back(std::move(data));
                loaded.frequency.push_back(dbc->getUInt32(row, 13 + f));
            }
            const float volume = dbc->getFloat(row, 24);
            if (volume > 0.0f) loaded.volume = volume;
            loaded.minDistance = dbc->getFloat(row, 26);
            loaded.cutoff = dbc->getFloat(row, 27);
        }
        // Remembered either way, so a row without a file is read once.
        it = kits_.emplace(soundId, std::move(loaded)).first;
    }
    return it->second.files.empty() ? nullptr : &it->second;
}

// One of the kit's files, by their Freq weights (0x004cbac0).
const std::vector<uint8_t>& AmbientSoundManager::pickFile(const Kit& k) {
    static std::mt19937 rng{std::random_device{}()};
    const uint64_t total = std::accumulate(k.frequency.begin(), k.frequency.end(), uint64_t{0});
    if (total == 0) return k.files[std::uniform_int_distribution<size_t>(0, k.files.size() - 1)(rng)];
    uint64_t roll = std::uniform_int_distribution<uint64_t>(0, total - 1)(rng);
    for (size_t i = 0; i < k.files.size(); ++i) {
        if (roll < k.frequency[i]) return k.files[i];
        roll -= k.frequency[i];
    }
    return k.files.back();
}

void AmbientSoundManager::setSlotAmbience(int slot, uint32_t soundAmbienceId) {
    if (slot < 0 || slot >= za::kSlotCount) return;
    za::SlotSound sound;
    if (soundAmbienceId != 0 && assets_) {
        // SoundAmbience.dbc: 0 ID, 1 day, 2 night.
        auto dbc = assets_->loadDBCOptional("SoundAmbience.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() >= 3 ? dbc->findRecordById(soundAmbienceId)
                                                                                 : -1;
        if (idx >= 0) {
            sound.day = dbc->getInt32(static_cast<uint32_t>(idx), 1);
            sound.night = dbc->getInt32(static_cast<uint32_t>(idx), 2);
        }
    }
    slots_[static_cast<size_t>(slot)] = sound;
}

void AmbientSoundManager::setSlotSound(int slot, int32_t soundId) {
    if (slot < 0 || slot >= za::kSlotCount) return;
    slots_[static_cast<size_t>(slot)] = za::SlotSound{soundId, soundId};
}

void AmbientSoundManager::setCameraLiquid(uint32_t liquidType) {
    // 0x004c8630: a change, this frame, makes the next switch at once.
    switchAtOnce_ = liquidType != cameraLiquid_;
    cameraLiquid_ = liquidType;
    setSlotSound(za::kSlotUnderwater, liquidType != 0 ? za::kUnderwaterSound : -1);
}

void AmbientSoundManager::update(bool isDay, bool indoors) {
    if (!initialized_) return;
    auto& engine = AudioEngine::instance();
    const bool atOnce = switchAtOnce_;
    switchAtOnce_ = false;
    const auto want = za::pick(slots_, isDay, indoors);
    if (!want) {
        // Past slot 10 (0x004c7140 with 13): what plays stops, with the
        // fade out it was started with - none.
        engine.stopSound(current_.handle);
        current_ = {};
        return;
    }
    // The channel left alone while it plays what is wanted; one that plays
    // nothing - a sound that would not start - is tried again.
    if (want->sound == current_.sound && (current_.handle != 0 || !kit(want->sound))) return;

    if (atOnce) {
        // Both of the channel's sounds at once, the new one at full.
        engine.stopSound(current_.handle);
        engine.stopSound(fadingHandle_);
        fadingHandle_ = 0;
    } else {
        // The one already falling goes on falling; the one playing falls
        // over five seconds, behind the new one rising over five.
        engine.stopSoundWithFade(current_.handle, za::kCrossfadeSeconds);
        fadingHandle_ = current_.handle;
    }
    current_ = {};
    current_.sound = want->sound;
    if (const Kit* k = kit(want->sound)) {
        current_.volume = k->volume;
        current_.handle = engine.playSound2DStoppable(pickFile(*k), k->volume * volumeScale_, true,
                                                      atOnce ? 0.0f : za::kCrossfadeSeconds);
    }
}

void AmbientSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::clamp(scale, 0.0f, 1.0f);
    auto& engine = AudioEngine::instance();
    engine.setSoundVolume(current_.handle, current_.volume * volumeScale_);
}

} // namespace audio
} // namespace wowee

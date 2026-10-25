#include "audio/player_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <numeric>
#include <string>

namespace wowee {
namespace audio {

namespace {

/// VocalUISounds.dbc: ID, VocalUIEnum, RaceID, NormalSoundID[2] by gender,
/// PissedSoundID[2] (which the error speech does not use).
constexpr uint32_t kVocalEnumColumn = 1;
constexpr uint32_t kVocalRaceColumn = 2;
constexpr uint32_t kVocalNormalSound0 = 3;

/// SoundEntries.dbc: 3..12 File, 13..22 Freq, 23 DirectoryBase, 24 VolumeFloat.
constexpr uint32_t kSoundFile0 = 3;
constexpr uint32_t kSoundFreq0 = 13;
constexpr uint32_t kSoundDirectory = 23;
constexpr uint32_t kSoundVolume = 24;

} // namespace

bool PlayerVoiceManager::initialize(pipeline::AssetManager* assets) {
    assetManager_ = assets;
    return assetManager_ != nullptr;
}

void PlayerVoiceManager::shutdown() {
    sounds_.fill(0);
    kits_.clear();
    loadedRace_ = -1;
    loadedGender_ = -1;
    assetManager_ = nullptr;
}

void PlayerVoiceManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

void PlayerVoiceManager::ensureTable(uint8_t raceId, uint8_t gender) {
    if (loadedRace_ == raceId && loadedGender_ == gender) return;
    loadedRace_ = raceId;
    loadedGender_ = gender;

    // 0x00986c90: every error silent, then each row of the race's - for a
    // gender of 0 or 1 - its sound for the gender.
    sounds_.fill(0);
    if (!assetManager_ || gender > 1) return;
    auto dbc = assetManager_->loadDBCOptional("VocalUISounds.dbc");
    if (!dbc || dbc->getFieldCount() <= kVocalNormalSound0 + 1) return;
    size_t found = 0;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const uint32_t vocal = dbc->getUInt32(i, kVocalEnumColumn);
        if (vocal >= vocal_error::kCount || dbc->getUInt32(i, kVocalRaceColumn) != raceId) continue;
        sounds_[vocal] = dbc->getUInt32(i, kVocalNormalSound0 + gender);
        ++found;
    }
    LOG_INFO("Player voice: ", found, " error speech sounds for race=", static_cast<int>(raceId),
             " gender=", static_cast<int>(gender));
}

const PlayerVoiceManager::Kit* PlayerVoiceManager::kit(uint32_t soundId) {
    // None for 0, nor the -1 a race without the line has (0x004c6a40
    // returns at once for an id below 1).
    if (static_cast<int32_t>(soundId) < 1 || !assetManager_) return nullptr;
    auto it = kits_.find(soundId);
    if (it == kits_.end()) {
        Kit loaded;
        auto dbc = assetManager_->loadDBCOptional("SoundEntries.dbc");
        const int32_t idx = dbc && dbc->getFieldCount() > kSoundVolume ? dbc->findRecordById(soundId) : -1;
        if (idx >= 0) {
            const auto row = static_cast<uint32_t>(idx);
            const std::string dir = dbc->getString(row, kSoundDirectory);
            for (uint32_t f = 0; f < 10; ++f) {
                const std::string name = dbc->getString(row, kSoundFile0 + f);
                if (name.empty()) continue;
                auto data = assetManager_->readFile(dir.empty() ? name : dir + "\\" + name);
                if (data.empty()) continue;
                loaded.files.push_back(std::move(data));
                loaded.frequency.push_back(dbc->getUInt32(row, kSoundFreq0 + f));
            }
            const float volume = dbc->getFloat(row, kSoundVolume);
            if (volume > 0.0f) loaded.volume = volume;
        }
        // Remembered either way, so a row without a file is read once.
        it = kits_.emplace(soundId, std::move(loaded)).first;
    }
    return it->second.files.empty() ? nullptr : &it->second;
}

void PlayerVoiceManager::playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender) {
    if (!enabled_ || !assetManager_) return;
    if (!AudioEngine::instance().isInitialized()) return;

    ensureTable(raceId, gender);
    const auto vocal = static_cast<uint8_t>(type);
    if (vocal >= vocal_error::kCount) return;
    const Kit* k = kit(sounds_[vocal]);
    if (!k) return;

    // One of the kit's files, by their Freq weights (0x004cbac0).
    const std::vector<uint8_t>* file = &k->files.back();
    const uint64_t total = std::accumulate(k->frequency.begin(), k->frequency.end(), uint64_t{0});
    if (total == 0) {
        file = &k->files[std::uniform_int_distribution<size_t>(0, k->files.size() - 1)(rng_)];
    } else {
        uint64_t roll = std::uniform_int_distribution<uint64_t>(0, total - 1)(rng_);
        for (size_t i = 0; i < k->files.size(); ++i) {
            if (roll < k->frequency[i]) {
                file = &k->files[i];
                break;
            }
            roll -= k->frequency[i];
        }
    }
    AudioEngine::instance().playSound2D(*file, k->volume * volumeScale_, 1.0f, Channel::Own,
                                        PlayLimit::ErrorSpeech);
}

} // namespace audio
} // namespace wowee

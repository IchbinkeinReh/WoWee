#include "audio/player_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "addons/lua_api_registrations.hpp"

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

/// SoundFilter.dbc: ID, Name. SoundFilterElem.dbc: ID, SoundFilterID,
/// OrderIndex, FilterType, Params[9].
constexpr uint32_t kFilterElemCount = 13;

/// The client's race names for the filter (0x00b24180).
const char* filterRaceName(uint8_t race) {
    static constexpr const char* kNames[] = {nullptr, "Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren",
                                             "Gnome", "Troll", "Goblin", "BloodElf", "Draenei"};
    return race < std::size(kNames) ? kNames[race] : nullptr;
}

constexpr uint8_t kClassDeathKnight = 6;

} // namespace

bool PlayerVoiceManager::initialize(pipeline::AssetManager* assets) {
    assetManager_ = assets;
    return assetManager_ != nullptr;
}

void PlayerVoiceManager::shutdown() {
    sounds_.fill(0);
    kits_.clear();
    deathKnightFilter_.clear();
    deathKnightFilterFor_ = -1;
    filtered_.clear();
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

std::vector<sound_filter::Element> PlayerVoiceManager::loadFilter(const std::string& name) const {
    std::vector<sound_filter::Element> chain;
    if (!assetManager_) return chain;
    auto filters = assetManager_->loadDBCOptional("SoundFilter.dbc");
    auto elems = assetManager_->loadDBCOptional("SoundFilterElem.dbc");
    if (!filters || !elems || filters->getFieldCount() < 2 || elems->getFieldCount() < kFilterElemCount) return chain;
    uint32_t id = 0;
    for (uint32_t i = 0; i < filters->getRecordCount() && id == 0; ++i) {
        if (filters->getString(i, 1) == name) id = filters->getUInt32(i, 0);
    }
    if (id == 0) return chain;
    std::vector<std::pair<uint32_t, sound_filter::Element>> ordered;
    for (uint32_t i = 0; i < elems->getRecordCount(); ++i) {
        if (elems->getUInt32(i, 1) != id) continue;
        sound_filter::Element e;
        e.type = elems->getUInt32(i, 3);
        for (uint32_t p = 0; p < e.params.size(); ++p) e.params[p] = elems->getFloat(i, 4 + p);
        ordered.emplace_back(elems->getUInt32(i, 2), e);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [order, e] : ordered) chain.push_back(e);
    return chain;
}

const std::vector<uint8_t>* PlayerVoiceManager::filtered(const std::vector<uint8_t>& file) {
    if (auto it = filtered_.find(&file); it != filtered_.end()) return it->second.empty() ? &file : &it->second;
    std::vector<float> samples;
    uint32_t channels = 0, rate = 0;
    std::vector<uint8_t> out;
    if (AudioEngine::decodeToFloat(file, samples, channels, rate)) {
        sound_filter::apply(samples, channels, rate, deathKnightFilter_);
        out = sound_filter::encodeWav(samples, channels, rate);
    }
    // Remembered either way: a file that will not decode plays as it is.
    auto& kept = filtered_[&file];
    kept = std::move(out);
    return kept.empty() ? &file : &kept;
}

void PlayerVoiceManager::playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender, uint8_t classId) {
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
    // A Death Knight's through its race's and gender's filter, with DSP
    // effects on (0x004cdf70 builds none without them; on by default with
    // two processors or more).
    if (classId == kClassDeathKnight && addons::storedCVarValue("Sound_EnableDSPEffects", "1") != "0") {
        const int key = raceId * 2 + (gender != 0 ? 1 : 0);
        if (key != deathKnightFilterFor_) {
            deathKnightFilterFor_ = key;
            filtered_.clear();
            const char* race = filterRaceName(raceId);
            deathKnightFilter_ = race ? loadFilter(std::string("Death Knight ") + race +
                                                   (gender != 0 ? " Female" : " Male"))
                                      : std::vector<sound_filter::Element>{};
        }
        if (!deathKnightFilter_.empty()) file = filtered(*file);
    }
    AudioEngine::instance().playSound2D(*file, k->volume * volumeScale_, 1.0f, Channel::Own,
                                        PlayLimit::ErrorSpeech);
}

} // namespace audio
} // namespace wowee

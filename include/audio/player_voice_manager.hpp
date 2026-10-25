#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "audio/vocal_error.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

/**
 * Plays the player character's spoken errors ("I can't do that", "Not
 * enough mana", ...) as the client does (0x00986d00): the sound
 * VocalUISounds.dbc gives the character's race and gender for the error -
 * a SoundEntries kit, a file of it by its weights at its volume. Gated by
 * the Character Speech setting (Sound_EnableErrorSpeech).
 */
class PlayerVoiceManager {
public:
    PlayerVoiceManager() = default;
    ~PlayerVoiceManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    /// Play a spoken error response for the player's race/gender - none
    /// while the last one still plays, as the client's error speech has one
    /// sound object it asks first (0x00986d00) and plays as type 7, one at a
    /// time (audio/play_limit.hpp).
    /// raceId: game Race enum value (1=Human .. 11=Draenei); gender: 0=male, 1=female
    void playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender);

private:
    /// A SoundEntries row as 0x004c6a40 plays it: its files, their Freq
    /// weights, its VolumeFloat.
    struct Kit {
        std::vector<std::vector<uint8_t>> files;
        std::vector<uint32_t> frequency;
        float volume = 1.0f;
    };

    /// The race's and gender's sound for each error (0x00986c90), read
    /// again when either changes.
    void ensureTable(uint8_t raceId, uint8_t gender);
    const Kit* kit(uint32_t soundId);

    pipeline::AssetManager* assetManager_ = nullptr;
    bool enabled_ = true;
    float volumeScale_ = 1.0f;

    int loadedRace_ = -1;
    int loadedGender_ = -1;
    std::array<uint32_t, vocal_error::kCount> sounds_{};
    std::unordered_map<uint32_t, Kit> kits_;

    std::mt19937 rng_{std::random_device{}()};
};

} // namespace audio
} // namespace wowee

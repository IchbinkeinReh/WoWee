#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/vec3.hpp>
#include "audio/zone_ambience.hpp"

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// The world's ambience as the client plays it (audio/zone_ambience.hpp):
/// the ambience channel and its slots (0xb4ac50, 0x004c9910). The places,
/// the weather and the liquid say what goes in the slots; this plays what
/// they pick.
class AmbientSoundManager {
public:
    AmbientSoundManager() = default;
    ~AmbientSoundManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    /// Leaving the world: the ambience stops at once and forgets the place
    /// it was for (0x00409550 -> 0x004c85f0 -> 0x004c6450(1)), and the
    /// slots the next world fills again (the weather's by 0x00403be9).
    void stopAll();

    /// A slot from a SoundAmbience row (0x004c8c90): its +4 by day, its +8
    /// by night; 0 or a row not there empties it.
    void setSlotAmbience(int slot, uint32_t soundAmbienceId);
    /// A slot from one SoundEntries id for day and night, -1 emptying it
    /// (the weather's, 0x004c8610).
    void setSlotSound(int slot, int32_t soundId);
    /// The liquid the camera is in, 0 for none (0x004c8630): the underwater
    /// sound in slot 9, and the next change of sound made at once.
    void setCameraLiquid(uint32_t liquidType);

    /// The ambience channel, once a frame (0x004c9910).
    void update(bool isDay, bool indoors);

    // Volume control: the ambience volume, over each sound's own.
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    [[nodiscard]] const zone_ambience::Slots& slots() const { return slots_; }

private:
    /// A SoundEntries row as 0x004c6a40 plays it: 3..12 File, 13..22 Freq,
    /// 23 DirectoryBase, 24 VolumeFloat, 26 MinDistance, 27 DistanceCutoff.
    struct Kit {
        std::vector<std::vector<uint8_t>> files;
        std::vector<uint32_t> frequency;
        float volume = 1.0f;
        float minDistance = 0.0f;
        float cutoff = 0.0f;
    };
    const Kit* kit(int32_t soundId);
    const std::vector<uint8_t>& pickFile(const Kit& k);
    pipeline::AssetManager* assets_ = nullptr;
    std::unordered_map<int32_t, Kit> kits_;

    zone_ambience::Slots slots_{};
    uint32_t cameraLiquid_ = 0;
    bool switchAtOnce_ = false;

    /// What the channel plays, and the one falling out behind it.
    struct Channel {
        uint32_t handle = 0;
        int32_t sound = -1;
        float volume = 1.0f;  ///< the sound's own
    };
    Channel current_;
    uint32_t fadingHandle_ = 0;

    float volumeScale_ = 1.0f;
    bool initialized_ = false;
};

} // namespace audio
} // namespace wowee

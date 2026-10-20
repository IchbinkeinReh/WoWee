#pragma once

#include <cstdint>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

enum class FootstepSurface : uint8_t {
    STONE = 0,
    DIRT,
    GRASS,
    WOOD,
    METAL,
    WATER,
    SNOW
};

/// The footstep volume (the settings' Footsteps slider over the Sound
/// Effects volume). The steps themselves are the models' $FSD events, every
/// unit's alike (EntitySpawner::playUnitModelEvents, 0x00747240), which
/// scale by it.
class FootstepManager {
public:
    FootstepManager() = default;
    ~FootstepManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void update(float deltaTime);
    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

    [[nodiscard]] bool isInitialized() const { return assetManager != nullptr; }

private:
    pipeline::AssetManager* assetManager = nullptr;
    float volumeScale = 1.0f;
};

} // namespace audio
} // namespace wowee

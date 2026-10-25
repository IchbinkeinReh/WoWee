#pragma once

#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

/// The combat sounds' volume - the Sound panel's combat slider over the
/// effects volume. What plays is the client's own: a melee swing's whoosh
/// and impact from WeaponSwingSounds2 and WeaponImpactSounds, timed by the
/// attacker's model (rendering/melee_sound.hpp, EntitySpawner); they are
/// heard at this scale's share of the effects volume.
class CombatSoundManager {
public:
    CombatSoundManager() = default;
    ~CombatSoundManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

private:
    float volumeScale_ = 1.0f;
};

} // namespace audio
} // namespace wowee

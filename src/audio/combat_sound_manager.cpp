#include "audio/combat_sound_manager.hpp"

#include <algorithm>

namespace wowee {
namespace audio {

bool CombatSoundManager::initialize(pipeline::AssetManager* assets) { return assets != nullptr; }

void CombatSoundManager::shutdown() {}

void CombatSoundManager::setVolumeScale(float scale) { volumeScale_ = std::clamp(scale, 0.0f, 1.0f); }

} // namespace audio
} // namespace wowee

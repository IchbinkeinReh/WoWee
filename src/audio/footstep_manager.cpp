#include "audio/footstep_manager.hpp"

namespace wowee {
namespace audio {

bool FootstepManager::initialize(pipeline::AssetManager* assets) {
    assetManager = assets;
    return assetManager != nullptr;
}

void FootstepManager::shutdown() { assetManager = nullptr; }

void FootstepManager::update(float) {}

} // namespace audio
} // namespace wowee

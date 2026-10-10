#pragma once

/// Item models hung on a character's attachment points by the client's
/// character component: the shoulders (0x004ef840).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterRenderer; }
namespace core {

class AssetPrefetch;

/// 0x004ef840 with 0x004ef4b0: a shoulder display's first model
/// (ItemDisplayInfo ModelName[0], ModelTexture[0]) on the left shoulder, 6,
/// its second on the right, 5, from Item\ObjectComponents\Shoulder\ as the
/// row names them. Whatever was on 5 and 6 comes off first; a display of 0
/// leaves them empty. `nextModelId` hands out the renderer's model ids.
void attachShoulders(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                     uint32_t instanceId, uint32_t displayInfoId,
                     const std::function<uint32_t()>& nextModelId,
                     const AssetPrefetch* prefetch = nullptr);

/// 0x004ef0d0: a head display's model, the race's cut of it where there is
/// one, on the helm point, 11. A display of 0 leaves it empty.
void attachHelm(rendering::CharacterRenderer& renderer, pipeline::AssetManager& assets,
                uint32_t instanceId, uint32_t displayInfoId, uint8_t raceId, uint8_t genderId,
                const std::function<uint32_t()>& nextModelId,
                const AssetPrefetch* prefetch = nullptr);

/// The files attachHelm and attachShoulders would read for these displays -
/// model and texture paths - so they can be prepared before the unit spawns.
/// A display of 0 adds nothing.
void itemAttachmentFiles(pipeline::AssetManager& assets, uint32_t helmDisplayId, uint8_t raceId,
                         uint8_t genderId, uint32_t shoulderDisplayId,
                         std::vector<std::string>& modelPaths, std::vector<std::string>& texturePaths);

}  // namespace core
}  // namespace wowee

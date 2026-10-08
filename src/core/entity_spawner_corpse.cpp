// A CORPSE object drawn as CGCorpse_C draws it (0x00705670, 0x00705b20,
// 0x00706120): bones as the race's death skeleton, otherwise its display's
// model - dressed as a character when that is a character model.
#include "core/entity_spawner.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "game/corpse_look.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"

#include <glm/gtc/constants.hpp>

namespace wowee::core {

std::optional<uint32_t> EntitySpawner::creatureModelFlags(uint32_t displayId) const {
    if (!assetManager_) return std::nullopt;
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    if (!displays || !models) return std::nullopt;
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* displayLayout = layouts ? layouts->getLayout("CreatureDisplayInfo") : nullptr;
    const int32_t displayRow = displays->findRecordById(displayId);
    if (displayRow < 0) return std::nullopt;
    const uint32_t modelId =
        displays->getUInt32(static_cast<uint32_t>(displayRow), displayLayout ? (*displayLayout)["ModelID"] : 1);
    const int32_t modelRow = models->findRecordById(modelId);
    if (modelRow < 0) return std::nullopt;
    // CreatureModelData +4, its Flags.
    return models->getUInt32(static_cast<uint32_t>(modelRow), 1);
}

std::optional<bool> EntitySpawner::corpseDisplayIsCharacter(uint32_t displayId) const {
    // Flag 4 is a model a character component dresses.
    const auto flags = creatureModelFlags(displayId);
    if (!flags) return std::nullopt;
    return (*flags & 4u) != 0;
}

void EntitySpawner::spawnCorpse(uint64_t guid, const game::CorpseLook& look, float x, float y, float z,
                                float orientation) {
    despawnCorpse(guid);
    if (look.bones()) {
        spawnCorpseBones(guid, game::corpseBonesModelPath(look.race, look.gender), x, y, z, orientation);
        return;
    }
    // 0x00705670: a display or model record that is not there draws nothing.
    const std::optional<bool> character = corpseDisplayIsCharacter(look.displayId);
    if (!character) {
        LOG_WARNING("Corpse 0x", std::hex, guid, std::dec, ": no model for display ", look.displayId);
        return;
    }
    corpseGuids_.insert(guid);
    // Laid down dead (0x00705b20 plays Dead, 6).
    markCreatureDead(guid);
    if (!*character) {
        corpseCreatureGuids_.insert(guid);
        queueCreatureSpawn(guid, look.displayId, x, y, z, orientation);
        return;
    }
    queuePlayerSpawn(guid, look.race, look.gender, look.appearanceBytes(), look.facialHair, x, y, z, orientation);
    queuePlayerEquipment(guid, look.displayIds, look.inventoryTypes);
}

void EntitySpawner::spawnCorpseBones(uint64_t guid, const std::string& modelPath, float x, float y, float z,
                                     float orientation) {
    auto* m2Renderer = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2Renderer || !assetManager_) return;
    uint32_t modelId = 0;
    if (auto it = corpseBonesModelIds_.find(modelPath);
        it != corpseBonesModelIds_.end() && m2Renderer->hasModel(it->second)) {
        modelId = it->second;
    } else {
        auto data = assetManager_->readFile(modelPath);
        if (data.empty()) {
            LOG_WARNING("Corpse bones model missing: ", modelPath);
            return;
        }
        pipeline::M2Model model = pipeline::M2Loader::load(data);
        model.name = modelPath;
        if (model.version >= 264) {
            auto skin = assetManager_->readFile(pipeline::skinPathForM2(modelPath));
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
        }
        modelId = nextGameObjectModelId_++;
        if (model.vertices.empty() || !m2Renderer->loadModel(model, modelId)) {
            LOG_WARNING("Corpse bones model failed to load: ", modelPath);
            return;
        }
        corpseBonesModelIds_[modelPath] = modelId;
    }
    const glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    // M2 models face +renderX: the same quarter turn game objects take.
    const uint32_t instanceId = m2Renderer->createInstance(
        modelId, renderPos, glm::vec3(0.0f, 0.0f, orientation + glm::half_pi<float>()), 1.0f);
    if (instanceId == 0) return;
    m2Renderer->setInstanceIsGameObject(instanceId, true);
    m2Renderer->setSkipCollision(instanceId, true);
    corpseBonesInstances_[guid] = instanceId;
}

void EntitySpawner::despawnCorpse(uint64_t guid) {
    if (auto it = corpseBonesInstances_.find(guid); it != corpseBonesInstances_.end()) {
        if (auto* m2Renderer = renderer_ ? renderer_->getM2Renderer() : nullptr) {
            m2Renderer->removeInstance(it->second);
        }
        corpseBonesInstances_.erase(it);
    }
    if (corpseCreatureGuids_.erase(guid) > 0) despawnCreature(guid);
    corpseGuids_.erase(guid);
}

}  // namespace wowee::core

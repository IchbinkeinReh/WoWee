// entity_spawner_artkit.cpp - a game object's art kit (GameObjectArtKit.dbc).
//
// GAMEOBJECT_BYTES_1's third byte names a GameObjectArtKit row, and the client
// dresses the object's model with it whenever the model loads or the byte
// changes (0x00713da0, reached from 0x00713f50 and 0x00713ed0): up to three
// textures into the model's replaceable slots 11-13, and up to four models,
// model i hung on the object model's attachment point i (0x00831630 with no
// fallback, so a point the model lacks hangs nothing). The 3.3.5a rows use the
// models only - the faction flag a capture tower flies in the Plaguelands and
// Hellfire, a Midsummer bonfire lit or out - and no row fills a texture.
#include "core/entity_spawner.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/renderer.hpp"
#include "rendering/m2_renderer.hpp"

#include <algorithm>
#include <cctype>

namespace wowee::core {

namespace {

/// The model file a DBC names, as the archive holds it: .mdx/.mdl are .m2.
std::string m2PathOf(std::string path) {
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".mdx" || ext == ".mdl") path = path.substr(0, dot) + ".m2";
    }
    return path;
}

/// The model cache is keyed by display id; an art kit's models take keys no
/// display id reaches.
uint32_t artKitModelKey(uint32_t artKit, uint32_t index) {
    return 0x80000000u | (artKit << 2) | (index & 3u);
}

}  // namespace

void EntitySpawner::clearGameObjectArtKit(uint64_t guid) {
    auto it = gameObjectArtKits_.find(guid);
    if (it == gameObjectArtKits_.end()) return;
    if (auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr) {
        for (const auto& [point, instance] : it->second.attached) m2->removeInstance(instance);
    }
    gameObjectArtKits_.erase(it);
}

void EntitySpawner::applyGameObjectArtKit(uint64_t guid) {
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    auto goIt = gameObjectInstances_.find(guid);
    if (!m2 || !gameHandler_ || !assetManager_ || goIt == gameObjectInstances_.end() ||
        goIt->second.isWmo) {
        clearGameObjectArtKit(guid);
        return;
    }
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    const uint16_t bytes1 = game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1);
    if (!entity || bytes1 == 0xFFFF) return;
    const uint32_t artKit = (entity->getField(bytes1) >> 16) & 0xFFu;
    const uint32_t parent = goIt->second.instanceId;

    if (auto cur = gameObjectArtKits_.find(guid); cur != gameObjectArtKits_.end() &&
        cur->second.artKit == artKit && cur->second.parentInstance == parent) {
        return;
    }
    clearGameObjectArtKit(guid);
    if (artKit == 0) return;

    auto dbc = assetManager_->loadDBC("GameObjectArtKit.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 8) return;
    const int32_t row = dbc->findRecordById(artKit);
    if (row < 0) return;

    GameObjectArtKitState state;
    state.artKit = artKit;
    state.parentInstance = parent;
    for (uint32_t i = 0; i < 4; ++i) {
        const std::string path = dbc->getString(static_cast<uint32_t>(row), 4 + i);
        if (path.empty()) continue;
        glm::mat4 frame(1.0f);
        if (!m2->getAttachmentTransform(parent, i, frame)) continue;
        const uint32_t key = artKitModelKey(artKit, i);
        if (gameObjectDisplayIdFailedCache_.count(key)) continue;
        PreparedGameObjectM2 prepared = prepareGameObjectM2(*assetManager_, key, m2PathOf(path));
        const uint32_t modelId = uploadGameObjectM2(prepared);
        if (modelId == 0) continue;
        const uint32_t instance = m2->createInstanceWithMatrix(modelId, frame, glm::vec3(frame[3]));
        if (instance == 0) continue;
        m2->setInstanceIsGameObject(instance, true);
        m2->setSkipCollision(instance, true);
        state.attached.emplace_back(i, instance);
        state.lastFrame.push_back(frame);
    }
    gameObjectArtKits_[guid] = std::move(state);
}

void EntitySpawner::updateGameObjectArtKits() {
    if (gameObjectArtKits_.empty()) return;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2) return;
    for (auto& [guid, state] : gameObjectArtKits_) {
        for (size_t k = 0; k < state.attached.size(); ++k) {
            glm::mat4 frame;
            if (!m2->getAttachmentTransform(state.parentInstance, state.attached[k].first, frame)) continue;
            if (frame == state.lastFrame[k]) continue;
            state.lastFrame[k] = frame;
            m2->setInstanceTransform(state.attached[k].second, frame);
        }
    }
}

}  // namespace wowee::core

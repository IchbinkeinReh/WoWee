// entity_spawner_destructible.cpp - a destructible building (game object
// type 33), as the reference client draws one (0x00710be0, 0x0070e0d0).
//
// The building has four looks: intact, damaged, destroyed and rebuilding.
// Its template names a DestructibleModelData row (data[18], the field the
// client asks for as 0x7c), and that row gives each state after the first a
// GameObjectDisplayInfo - State1Wmo, State2Wmo, State3Wmo - a state without
// one keeping the look of the state before it. The intact look is the
// template's own display. Which look shows is GAMEOBJECT_FLAGS bits 9..11
// ((flags >> 8 & 0xe) >> 1: 0x200 damaged, 0x400 destroyed), so the
// building changes the moment the realm flags it, whatever its display field
// says.
//
// Each look is a WMO, and the row names the doodad sets each state shows:
// its AmbientDoodadSet (fire, smoke, rubble), and for the damaged, destroyed
// and rebuilding states the DestructionDoodadSet / InitDoodadSet; set 0, the
// one every WMO carries, always.
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/wmo_renderer.hpp"

#include <functional>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::core {

namespace {

constexpr uint32_t kDestructibleType = 33;
constexpr uint32_t kDestructibleDataIndex = 18;

// DestructibleModelData.dbc, 3.3.5a: ID, then per state the columns below.
constexpr uint32_t kColState0Ambient = 2;
constexpr uint32_t kColStateWmo[4] = {0, 3, 7, 11};
constexpr uint32_t kColStateFirstSet[4] = {0, 4, 8, 12};   // destruction / init
constexpr uint32_t kColStateAmbient[4] = {kColState0Ambient, 6, 10, 13};

uint32_t stateOfFlags(uint32_t flags) {
    const uint32_t s = ((flags >> 8) & 0xeu) >> 1;
    return s > 3 ? 3 : s;
}

}  // namespace

bool EntitySpawner::destructibleLook(uint64_t guid, uint32_t entry, DestructibleLook& out) const {
    if (!gameHandler_ || !assetManager_) return false;
    const auto* info = gameHandler_->getCachedGameObjectInfo(entry);
    if (!info || info->type != kDestructibleType) return false;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    const uint16_t flagsField = game::fieldIndex(game::UF::GAMEOBJECT_FLAGS);
    out = DestructibleLook{};
    out.state = entity && flagsField != 0xFFFF ? stateOfFlags(entity->getField(flagsField)) : 0;
    out.display = info->displayId;
    const uint32_t rowId = info->hasData ? info->data[kDestructibleDataIndex] : 0;
    auto dbc = rowId ? assetManager_->loadDBCOptional("DestructibleModelData.dbc") : nullptr;
    const int32_t row = dbc && dbc->getFieldCount() > 13 ? dbc->findRecordById(rowId) : -1;
    if (row < 0) return true;   // the intact look and no sets
    const auto r = static_cast<uint32_t>(row);
    // The state's display, else the state before it's (0x00710be0).
    for (uint32_t s = 1; s <= out.state; ++s) {
        const uint32_t d = dbc->getUInt32(r, kColStateWmo[s]);
        if (d != 0) out.display = d;
    }
    out.sets.push_back(0);
    if (const uint32_t ambient = dbc->getUInt32(r, kColStateAmbient[out.state]); ambient != 0)
        out.sets.push_back(ambient);
    if (out.state > 0) {
        if (const uint32_t first = dbc->getUInt32(r, kColStateFirstSet[out.state]); first != 0)
            out.sets.push_back(first);
    }
    return true;
}

void EntitySpawner::spawnDestructibleDoodads(uint64_t guid, const std::string& wmoPath) {
    auto goIt = gameObjectInstances_.find(guid);
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (goIt == gameObjectInstances_.end() || !goIt->second.isWmo || !wmo || !m2) return;
    auto entity = gameHandler_ ? gameHandler_->getEntityManager().getEntity(guid) : nullptr;
    const uint16_t entryField = game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY);
    if (!entity || entryField == 0xFFFF) return;
    DestructibleLook look;
    if (!destructibleLook(guid, entity->getField(entryField), look)) return;

    // The root names the doodads and their sets.
    auto rootIt = destructibleWmoRoots_.find(wmoPath);
    if (rootIt == destructibleWmoRoots_.end()) {
        auto data = assetManager_->readFile(wmoPath);
        rootIt = destructibleWmoRoots_
                     .emplace(wmoPath, std::make_shared<pipeline::WMOModel>(
                                           data.empty() ? pipeline::WMOModel{} : pipeline::WMOLoader::load(data)))
                     .first;
    }
    const pipeline::WMOModel& root = *rootIt->second;
    std::vector<uint32_t>& spawned = destructibleDoodads_[guid];
    for (uint32_t set : look.sets) {
        if (set >= root.doodadSets.size()) continue;
        const auto& ds = root.doodadSets[set];
        for (uint32_t di = 0; di < ds.count; ++di) {
            const uint32_t idx = ds.startIndex + di;
            if (idx >= root.doodads.size()) break;
            const auto& doodad = root.doodads[idx];
            auto name = root.doodadNames.find(doodad.nameIndex);
            if (name == root.doodadNames.end() || name->second.empty()) continue;
            const std::string path = pipeline::modelPathToM2(name->second);
            const uint32_t key = 0x40000000u | (static_cast<uint32_t>(std::hash<std::string>{}(path)) & 0x3FFFFFFFu);
            if (gameObjectDisplayIdFailedCache_.count(key)) continue;
            PreparedGameObjectM2 prepared = prepareGameObjectM2(*assetManager_, key, path);
            const uint32_t modelId = uploadGameObjectM2(prepared);
            if (modelId == 0) continue;
            const uint32_t inst = m2->createInstance(modelId, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f,
                                                     /*allowPositionDedup=*/false);
            if (inst == 0) continue;
            m2->setSkipCollision(inst, true);
            m2->setInstanceEventOwner(inst, rendering::m2_event::Owner::Doodad, 0);
            const glm::quat q(doodad.rotation.w, doodad.rotation.x, doodad.rotation.y, doodad.rotation.z);
            const glm::mat4 local = glm::translate(glm::mat4(1.0f), doodad.position) * glm::mat4_cast(q) *
                                    glm::scale(glm::mat4(1.0f), glm::vec3(doodad.scale));
            wmo->addDoodadToInstance(goIt->second.instanceId, inst, local);
            spawned.push_back(inst);
        }
    }
}

void EntitySpawner::clearDestructible(uint64_t guid) {
    destructibleStates_.erase(guid);
    destructibleDoodads_.erase(guid);   // the WMO instance takes its doodads with it
}

void EntitySpawner::applyDestructibleFlags(uint64_t guid) {
    if (!gameHandler_) return;
    auto state = destructibleStates_.find(guid);
    if (state == destructibleStates_.end()) return;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    const uint16_t flagsField = game::fieldIndex(game::UF::GAMEOBJECT_FLAGS);
    if (!entity || flagsField == 0xFFFF) return;
    const uint32_t now = stateOfFlags(entity->getField(flagsField));
    if (now == state->second) return;
    // Another look (0x0070e0d0): the model the state shows and its sets.
    const auto& go = static_cast<const game::GameObject&>(*entity);
    float scale = 1.0f;
    if (auto it = gameObjectInstances_.find(guid); it != gameObjectInstances_.end()) scale = it->second.scale;
    despawnGameObject(guid);
    queueGameObjectSpawn(guid, go.getEntry(), go.getDisplayId(), go.getX(), go.getY(), go.getZ(),
                         go.getOrientation(), scale);
}

}  // namespace wowee::core

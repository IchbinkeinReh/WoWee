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
//
// A change of state is played, not cut (0x0070e0d0, ticked by 0x0070e7b0's
// tail): the new state's destruction set plays Custom0 once and stands, its
// ambient set plays Custom0 and then loops Custom1 (0x0070b800, 0x0070b870);
// the old state's impact and ambient sets move onto the new look, play
// Custom2 and stand (0x0070b8b0) until the next change. Coming back to whole
// or to rebuilding the new look rises out of the ground - its height over a
// third of HealEffectSpeed (12 when none) per second, eased by HealEffect -
// while the old look sinks, and the row's RepairGroundFx model stands at the
// building's foot meanwhile. HealEffect 4 is no rise at all.
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

#include <chrono>
#include <cmath>
#include <functional>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::core {

namespace {

constexpr uint32_t kDestructibleType = 33;
constexpr uint32_t kDestructibleDataIndex = 18;

// DestructibleModelData.dbc, 3.3.5a: ID, then per state the columns below.
constexpr uint32_t kColStateWmo[4] = {0, 3, 7, 11};
constexpr uint32_t kColStateFirstSet[4] = {0, 4, 8, 12};   // destruction / init
constexpr uint32_t kColStateAmbient[4] = {2, 6, 10, 13};
constexpr uint32_t kColStateImpact[4] = {1, 5, 9, 0};
constexpr uint32_t kColRepairGroundFx = 15;
constexpr uint32_t kColHealEffect = 17;
constexpr uint32_t kColHealEffectSpeed = 18;

constexpr uint32_t kAnimStand = 0, kAnimCustom0 = 153, kAnimCustom1 = 154, kAnimCustom2 = 155;

uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

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
    for (auto& d : out.stateDisplay) d = info->displayId;
    const uint32_t rowId = info->hasData ? info->data[kDestructibleDataIndex] : 0;
    auto dbc = rowId ? assetManager_->loadDBCOptional("DestructibleModelData.dbc") : nullptr;
    const int32_t row = dbc && dbc->getFieldCount() > kColHealEffectSpeed ? dbc->findRecordById(rowId) : -1;
    if (row < 0) return true;   // the intact look and no sets
    const auto r = static_cast<uint32_t>(row);
    out.hasRow = true;
    // Each state's display, else the state before it's (0x00710be0).
    for (uint32_t s = 1; s < 4; ++s) {
        const uint32_t d = dbc->getUInt32(r, kColStateWmo[s]);
        out.stateDisplay[s] = d != 0 ? d : out.stateDisplay[s - 1];
    }
    out.display = out.stateDisplay[out.state];
    for (uint32_t s = 0; s < 4; ++s) {
        out.ambientSet[s] = dbc->getUInt32(r, kColStateAmbient[s]);
        out.firstSet[s] = s == 0 ? 0 : dbc->getUInt32(r, kColStateFirstSet[s]);
        out.impactSet[s] = s == 3 ? 0 : dbc->getUInt32(r, kColStateImpact[s]);
    }
    out.repairFx = dbc->getUInt32(r, kColRepairGroundFx);
    out.healEffect = dbc->getUInt32(r, kColHealEffect);
    out.healSpeed = dbc->getInt32(r, kColHealEffectSpeed);
    out.sets.push_back(0);
    if (out.ambientSet[out.state] != 0) out.sets.push_back(out.ambientSet[out.state]);
    if (out.firstSet[out.state] != 0) out.sets.push_back(out.firstSet[out.state]);
    return true;
}

const pipeline::WMOModel* EntitySpawner::destructibleRoot(const std::string& wmoPath) {
    auto rootIt = destructibleWmoRoots_.find(wmoPath);
    if (rootIt == destructibleWmoRoots_.end()) {
        auto data = assetManager_->readFile(wmoPath);
        rootIt = destructibleWmoRoots_
                     .emplace(wmoPath, std::make_shared<pipeline::WMOModel>(
                                           data.empty() ? pipeline::WMOModel{} : pipeline::WMOLoader::load(data)))
                     .first;
    }
    return rootIt->second.get();
}

std::vector<uint32_t> EntitySpawner::hangDoodadSet(uint32_t wmoInstance, const pipeline::WMOModel& root,
                                                   uint32_t set) {
    std::vector<uint32_t> made;
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!wmo || !m2 || set >= root.doodadSets.size()) return made;
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
        wmo->addDoodadToInstance(wmoInstance, inst, local);
        made.push_back(inst);
    }
    return made;
}

void EntitySpawner::startDoodadChain(uint32_t inst, uint32_t once, uint32_t then) {
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2) return;
    if (!m2->hasAnimation(inst, once)) {
        if (m2->hasAnimation(inst, then)) m2->setInstanceAnimation(inst, then, true);
        return;
    }
    m2->setInstanceAnimation(inst, once, false);
    uint32_t length = 0;
    if (const auto pb = m2->getInstanceAnimPlayback(inst)) length = static_cast<uint32_t>(pb->durationMs);
    destructibleAnimChains_.push_back({inst, nowMs() + length, then});
}

void EntitySpawner::spawnDestructibleDoodads(uint64_t guid, const std::string& wmoPath) {
    auto goIt = gameObjectInstances_.find(guid);
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (goIt == gameObjectInstances_.end() || !wmo || !m2) return;
    if (!goIt->second.isWmo) {
        // A model look has no sets to play; the old look just gives way.
        if (auto runIt = destructibleRuns_.find(guid); runIt != destructibleRuns_.end() && runIt->second.pendingFrom >= 0) {
            auto& run = runIt->second;
            if (run.oldInstance) run.oldIsWmo ? wmo->removeInstance(run.oldInstance) : m2->removeInstance(run.oldInstance);
            run.oldInstance = 0;
            run.pendingFrom = -1;
        }
        return;
    }
    auto entity = gameHandler_ ? gameHandler_->getEntityManager().getEntity(guid) : nullptr;
    const uint16_t entryField = game::fieldIndex(game::UF::OBJECT_FIELD_ENTRY);
    if (!entity || entryField == 0xFFFF) return;
    DestructibleLook look;
    if (!destructibleLook(guid, entity->getField(entryField), look)) return;
    const pipeline::WMOModel* root = destructibleRoot(wmoPath);
    if (!root) return;
    const uint32_t instance = goIt->second.instanceId;

    DestructibleRun& run = destructibleRuns_[guid];
    run.wmoPath = wmoPath;
    const bool changing = run.pendingFrom >= 0;

    // Set 0 stands as the model does.
    hangDoodadSet(instance, *root, 0);
    // The state's own sets: played in on a change, already standing when the
    // building comes into view in that state (0x00710be0 loads them so).
    // (Into whole or rebuilding the change is the rise, and the init set is
    // not played in - 0x0070e0d0 enables it only on a change that does not.)
    const bool risingState = look.state == 0 || look.state == 3;
    if (!(changing && risingState)) {
        for (uint32_t inst : hangDoodadSet(instance, *root, look.firstSet[look.state])) {
            if (changing) startDoodadChain(inst, kAnimCustom0, kAnimStand);
        }
    }
    for (uint32_t inst : hangDoodadSet(instance, *root, look.ambientSet[look.state])) {
        if (changing) startDoodadChain(inst, kAnimCustom0, kAnimCustom1);
        else if (m2->hasAnimation(inst, kAnimCustom1)) m2->setInstanceAnimation(inst, kAnimCustom1, true);
    }
    if (!changing) return;

    const uint32_t from = static_cast<uint32_t>(run.pendingFrom);
    run.pendingFrom = -1;
    // The old state's impact and ambient sets, moved over and played out.
    if (const pipeline::WMOModel* oldRoot = destructibleRoot(run.fromWmoPath)) {
        for (uint32_t set : {look.impactSet[from], look.ambientSet[from]}) {
            if (set == 0) continue;
            for (uint32_t inst : hangDoodadSet(instance, *oldRoot, set)) startDoodadChain(inst, kAnimCustom2, kAnimStand);
        }
    }

    // Back to whole or to rebuilding: the rise.
    const bool rises = (look.state == 0 || look.state == 3) && look.hasRow && look.healEffect != 4;
    const float speed = look.healSpeed > 0 ? static_cast<float>(look.healSpeed) : 12.0f;
    const auto base = wmo->getInstanceModelMatrix(instance);
    if (!rises || !base || root->boundingBoxMax.z <= root->boundingBoxMin.z) {
        if (run.oldInstance) {
            run.oldIsWmo ? wmo->removeInstance(run.oldInstance) : m2->removeInstance(run.oldInstance);
            run.oldInstance = 0;
        }
        return;
    }
    run.rising = true;
    run.healEffect = look.healEffect;
    run.newInstance = instance;
    run.newBase = *base;
    run.height = (root->boundingBoxMax.z - root->boundingBoxMin.z);
    run.riseStartMs = nowMs();
    run.riseMs = static_cast<uint32_t>(std::lround(run.height / (speed / 3.0f) * 1000.0f));
    // The old look stays for the rise (not with HealEffect 3), sinking.
    if (run.oldInstance && run.oldIsWmo && look.healEffect != 3) {
        const pipeline::WMOModel* oldRoot = destructibleRoot(run.fromWmoPath);
        const auto oldBase = wmo->getInstanceModelMatrix(run.oldInstance);
        run.oldBase = oldBase ? *oldBase : *base;
        run.oldHeight = oldRoot ? oldRoot->boundingBoxMax.z - oldRoot->boundingBoxMin.z : 0.0f;
    } else if (run.oldInstance) {
        run.oldIsWmo ? wmo->removeInstance(run.oldInstance) : m2->removeInstance(run.oldInstance);
        run.oldInstance = 0;
    }
    // The repair effect at the building's foot.
    if (look.repairFx != 0) {
        const std::string fxPath = getGameObjectModelPathForDisplayId(look.repairFx);
        if (!fxPath.empty() && !gameObjectDisplayIdFailedCache_.count(look.repairFx)) {
            PreparedGameObjectM2 prepared = prepareGameObjectM2(*assetManager_, look.repairFx, fxPath);
            if (const uint32_t modelId = uploadGameObjectM2(prepared)) {
                run.fxInstance = m2->createInstance(modelId, glm::vec3((*base)[3]), glm::vec3(0.0f), 1.0f,
                                                    /*allowPositionDedup=*/false);
                if (run.fxInstance) m2->setSkipCollision(run.fxInstance, true);
            }
        }
    }
    updateDestructibles();
}

void EntitySpawner::updateDestructibles() {
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!wmo || !m2) return;
    const uint32_t now = nowMs();
    // Doodads whose one-shot has played go on to what follows it.
    for (auto it = destructibleAnimChains_.begin(); it != destructibleAnimChains_.end();) {
        if (static_cast<int32_t>(now - it->endMs) < 0) { ++it; continue; }
        if (m2->hasInstance(it->instance) && m2->hasAnimation(it->instance, it->then))
            m2->setInstanceAnimation(it->instance, it->then, true);
        it = destructibleAnimChains_.erase(it);
    }
    const auto lowered = [](const glm::mat4& base, float by) {
        return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -by)) * base;
    };
    for (auto& [guid, run] : destructibleRuns_) {
        if (!run.rising) continue;
        const uint32_t elapsed = now - run.riseStartMs;
        if (elapsed >= run.riseMs || run.riseMs == 0) {
            // In place; the old look and the effect go (0x0070e7b0's tail).
            wmo->setInstanceTransform(run.newInstance, run.newBase);
            if (run.oldInstance) wmo->removeInstance(run.oldInstance);
            if (run.fxInstance) m2->removeInstance(run.fxInstance);
            run.oldInstance = run.fxInstance = 0;
            run.rising = false;
            continue;
        }
        const float t = static_cast<float>(elapsed) / static_cast<float>(run.riseMs);
        float newDown, oldDown = 0.0f;
        if (run.healEffect == 0) {
            // The old look sinks in the first half, the new rises in the second.
            float f = 0.0f;
            if (t > 0.5f) {
                const float g = 1.0f - 2.0f * (t - 0.5f);
                f = 1.0f - g * g * g;
            }
            newDown = (1.0f - f) * run.height;
            oldDown = (t >= 0.5f ? 1.0f : 8.0f * t * t * t) * run.oldHeight;
        } else {
            const float g = 1.0f - t;
            newDown = g * g * g * run.height;
            if (run.healEffect == 2) oldDown = t * t * t * run.oldHeight;
        }
        wmo->setInstanceTransform(run.newInstance, lowered(run.newBase, newDown));
        if (run.oldInstance) wmo->setInstanceTransform(run.oldInstance, lowered(run.oldBase, oldDown));
    }
}

void EntitySpawner::clearDestructible(uint64_t guid) {
    destructibleStates_.erase(guid);
    auto it = destructibleRuns_.find(guid);
    if (it == destructibleRuns_.end()) return;
    // What a change kept alive past the look it belonged to.
    if (it->second.pendingFrom < 0) {
        auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
        auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
        if (it->second.oldInstance && wmo && m2)
            it->second.oldIsWmo ? wmo->removeInstance(it->second.oldInstance) : m2->removeInstance(it->second.oldInstance);
        if (it->second.fxInstance && m2) m2->removeInstance(it->second.fxInstance);
        destructibleRuns_.erase(it);
    }
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
    const auto& go = static_cast<const game::GameObject&>(*entity);
    auto goIt = gameObjectInstances_.find(guid);
    if (goIt == gameObjectInstances_.end()) return;
    // Another look (0x0070e0d0). The one showing is kept back from the
    // despawn: it stays until the new one is up, and through a rise.
    DestructibleRun& run = destructibleRuns_[guid];
    auto* wmo = renderer_ ? renderer_->getWMORenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (run.oldInstance && wmo && m2)   // a change still running gives way to this one
        run.oldIsWmo ? wmo->removeInstance(run.oldInstance) : m2->removeInstance(run.oldInstance);
    if (run.fxInstance && m2) m2->removeInstance(run.fxInstance);
    if (run.rising && wmo) wmo->setInstanceTransform(run.newInstance, run.newBase);
    run.rising = false;
    run.fxInstance = 0;
    run.oldInstance = goIt->second.instanceId;
    run.oldIsWmo = goIt->second.isWmo;
    run.fromWmoPath = run.wmoPath;
    run.pendingFrom = static_cast<int>(state->second);
    const float scale = goIt->second.scale;
    gameObjectInstances_.erase(goIt);
    despawnGameObject(guid);
    queueGameObjectSpawn(guid, go.getEntry(), go.getDisplayId(), go.getX(), go.getY(), go.getZ(),
                         go.getOrientation(), scale);
}

}  // namespace wowee::core

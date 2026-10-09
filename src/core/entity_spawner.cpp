#include "core/entity_spawner.hpp"
#include "core/character_component.hpp"
#include "core/item_attachments.hpp"
#include "core/weapon_attachment.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "core/appearance_composer.hpp"
#include "pipeline/char_sections.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"
#include "core/helm_visual.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "audio/npc_voice_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/mount_seat.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <bit>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>

#include <set>

namespace wowee {
namespace core {

// The geoset numbers come from appearance_composer.hpp, which this file used to
// keep its own copy of.
//
// They were identical when written and stopped being so the moment one of them
// was corrected: group 20 is the feet, an HD human female carries 2001 where an
// HD human male carries 2002, and the fix that names both went into the header's
// copy. This file kept asking for 2002 alone, so every character it draws - an
// NPC, and another player - lost their feet on exactly the models the header's
// copy had been taught about.
//
// One definition, so the next correction cannot land in only half the client.

// --- Constructor / Destructor ---

EntitySpawner::EntitySpawner(rendering::Renderer* renderer,
                             pipeline::AssetManager* assetManager,
                             game::GameHandler* gameHandler,
                             game::GameServices* gameServices)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , gameServices_(gameServices)
{
}

EntitySpawner::~EntitySpawner() = default;
// --- Lifecycle ---

void EntitySpawner::initialize() {
    buildCharSectionsCache();
    buildCreatureDisplayLookups();
    buildGameObjectDisplayLookups();
}

void EntitySpawner::update() {
    processPlayerSpawnQueue();
    processCreatureSpawnQueue();
    processAsyncNpcCompositeResults();
    processDeferredEquipmentQueue();
    processGameObjectSpawnQueue();
    processPendingTransportRegistrations();
    processPendingTransportDoodads();
    processPendingMount();
    processPendingRemotePlayerMounts();
    syncCreatureStealthVisuals();
    refreshCreatureScales();
    syncCreatureParticleTwins();
    syncGroundTargetModel();
}

void EntitySpawner::syncGroundTargetModel() {
    // A guid no server object has: the client's own query for it runs under
    // a high part of 0x1FE (0x0080cce0).
    constexpr uint64_t kPreviewGuid = 0x1FE0000000000000ull;
    auto drop = [&] {
        if (groundTargetModelEntry_ == 0) return;
        despawnGameObject(kPreviewGuid);
        groundTargetModelEntry_ = 0;
    };
    if (!gameHandler_ || !renderer_) return drop();
    const uint32_t entry = gameHandler_->groundTargetObjectEntry();
    const auto place = gameHandler_->groundTargetCursor();
    const auto* info = entry != 0 ? gameHandler_->getCachedGameObjectInfo(entry) : nullptr;
    if (!place || !info || info->displayId == 0) return drop();
    // A place the spell cannot take hides the model: 0x004f66c0 hands
    // 0x0077f2f0 the placement, which flags the scene object (+0x7c bit 4),
    // and a flagged object's model is left out of the scene's draw list
    // (0x00793060 clears its M2 flags 0x8 and 0x10000), its shadow and the
    // world's collision.
    auto* spells = gameHandler_->getSpellHandler();
    if (spells && spells->groundTargetPlacement(place->canonical) !=
                      static_cast<int>(game::ground_target::Placement::Acceptable))
        return drop();
    // Only a doodad model: a building would be in the way of the very ray
    // that places it.
    std::string path = getGameObjectModelPathForDisplayId(info->displayId);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".wmo") == 0) return drop();

    const float facing = gameHandler_->groundTargetObjectFacing();
    const float size = info->size > 0.0f ? info->size : 1.0f;
    auto* m2 = renderer_->getM2Renderer();
    if (!m2) return;
    auto it = gameObjectInstances_.find(kPreviewGuid);
    if (groundTargetModelEntry_ != entry || it == gameObjectInstances_.end() || !m2->hasInstance(it->second.instanceId)) {
        drop();
        spawnOnlineGameObject(kPreviewGuid, entry, info->displayId, place->canonical.x, place->canonical.y,
                              place->canonical.z, facing, size);
        it = gameObjectInstances_.find(kPreviewGuid);
        if (it == gameObjectInstances_.end() || it->second.isWmo) return;
        groundTargetModelEntry_ = entry;
        m2->setSkipCollision(it->second.instanceId, true);
    }
    // At the place, turned as the player faces, at the template's size
    // (0x004f66c0: 0x007fd7e0, 0x007fff60, 0x004c1bf0 with 0x00d3f4d8).
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), core::coords::canonicalToRender(place->canonical));
    transform = glm::rotate(transform, facing + glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    transform = glm::scale(transform, glm::vec3(size));
    m2->setInstanceTransform(it->second.instanceId, transform);
}

void EntitySpawner::syncCreatureStealthVisuals() {
    if (!renderer_ || !gameHandler_) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;

    // Stealth flags flip rarely, but this scan visits every spawned creature.
    // Doing that each frame with the mutex-locked getEntity() plus a
    // dynamic_pointer_cast per creature was a measurable main-thread cost in
    // crowded areas - a few sweeps per second is visually indistinguishable.
    if (++stealthSyncFrameCounter_ % 15 != 0) return;

    // EntitySpawner::update() runs on the main thread, so the unlocked
    // getEntities() reference is safe and avoids a mutex acquire per creature.
    const auto& entities = gameHandler_->getEntityManager().getEntities();

    // Undetected stealth is culled by the server. Units that are sent with the
    // CREEP visibility flag use the translucent detected-stealth presentation.
    constexpr float kDetectedStealthOpacity = 0.35f;
    for (const auto& [guid, instanceId] : creatureInstances_) {
        auto entIt = entities.find(guid);
        if (entIt == entities.end() || !entIt->second) continue;
        const game::Entity* entity = entIt->second.get();
        if (entity->getType() != game::ObjectType::UNIT &&
            entity->getType() != game::ObjectType::PLAYER) continue;
        const auto* unit = static_cast<const game::Unit*>(entity);

        const bool stealthed = unit->hasCreepVisibility();
        auto [it, inserted] = creatureWasStealthed_.try_emplace(guid, stealthed);
        if (!inserted && it->second == stealthed) continue;

        it->second = stealthed;
        characterRenderer->setInstanceOpacity(
            instanceId, stealthed ? kDetectedStealthOpacity : 1.0f);
    }
}

void EntitySpawner::shutdown() {
    clearAllQueues();
    // Clear all instances
    creatureInstances_.clear();
    creatureParticleTwins_.clear();
    creatureModelIds_.clear();
    creatureDisplayIds_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureRenderPosCache_.clear();
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureActiveEmotes_.clear();
    creatureWasStealthed_.clear();
    unitWeaponsShown_.clear();
    unitSheath_.clear();
    corpseGuids_.clear();
    corpseCreatureGuids_.clear();
    corpseBonesInstances_.clear();
    corpseBonesModelIds_.clear();
    corpseSites_.clear();
    paintedTabards_.clear();
    lootSparkleModelId_ = 0;
    lootSparkleModelTried_ = false;
    animationDataDbc_.reset();
    animationDataLoaded_ = false;
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    remotePlayerMounts_.clear();
    pendingRemotePlayerMounts_.clear();
    gameObjectInstances_.clear();
}

void EntitySpawner::resetAllState() {
    // Wait for in-flight async loads before clearing state
    for (auto& load : asyncCreatureLoads_) {
        if (load.future.valid()) load.future.wait();
    }

    // Despawn all entities (renderer cleanup)
    despawnAllCreatures();
    despawnAllPlayers();
    despawnAllGameObjects();
    clearMountState();

    // Clear all queues and async loads
    clearAllQueues();

    // Clear all instance tracking
    creatureInstances_.clear();
    creatureParticleTwins_.clear();
    creatureModelIds_.clear();
    creatureDisplayIds_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureRenderPosCache_.clear();
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    remotePlayerMounts_.clear();
    pendingRemotePlayerMounts_.clear();
    gameObjectInstances_.clear();

    // Clear animation state maps
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureActiveEmotes_.clear();
    // Carried over, a stale entry matching the creature's current stealth state
    // suppresses the opacity call that would apply it, so a stealthed NPC came
    // back fully opaque after a relog. shutdown() already cleared this.
    creatureWasStealthed_.clear();
    unitWeaponsShown_.clear();
    unitSheath_.clear();
    corpseGuids_.clear();
    corpseCreatureGuids_.clear();
    corpseBonesInstances_.clear();
    corpseBonesModelIds_.clear();
    corpseSites_.clear();
    paintedTabards_.clear();
    lootSparkleModelId_ = 0;
    lootSparkleModelTried_ = false;
    animationDataDbc_.reset();
    animationDataLoaded_ = false;
    modelIdIsWolfLike_.clear();

    // Clear display/spawn caches
    nonRenderableCreatureDisplayIds_.clear();
    displayIdModelCache_.clear();
    displayIdTexturesApplied_.clear();
    charSectionsCache_.clear();
    charSectionsCacheBuilt_ = false;
    // The asset tree may differ next time - a failure is not carried over.
    failedPlayerModelKeys_.clear();

    // Clear GO display caches
    gameObjectDisplayIdModelCache_.clear();
    gameObjectDisplayIdWmoCache_.clear();
    gameObjectDisplayIdFailedCache_.clear();
    // Instance ids in here belong to a renderer that has just been cleared.
    gameObjectServerState_.clear();
}

void EntitySpawner::rebuildLookups() {
    creatureLookupsBuilt_ = false;
    displayDataMap_.clear();
    humanoidExtraMap_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    nonRenderableCreatureDisplayIds_.clear();
    initialize();
}

bool EntitySpawner::hasWorkPending() const {
    return !pendingCreatureSpawns_.empty() || !asyncCreatureLoads_.empty() ||
           !asyncNpcCompositeLoads_.empty() || !pendingPlayerSpawns_.empty() ||
           !asyncPlayerModelLoads_.empty() ||
           !asyncEquipmentLoads_.empty() || !deferredEquipmentQueue_.empty() ||
           !pendingGameObjectSpawns_.empty() || !asyncGameObjectLoads_.empty();
}

void EntitySpawner::clearMountState() {
    if (mountInstanceId_ != 0 && renderer_) {
        if (auto* charRenderer = renderer_->getCharacterRenderer()) {
            charRenderer->removeInstance(mountInstanceId_);
        }
    }
    mountInstanceId_ = 0;
    mountModelId_ = 0;
    mountDisplayId_ = 0;
    pendingMountDisplayId_ = 0;
}

uint32_t EntitySpawner::loadMountModel(uint32_t displayId) {
    uint32_t modelId = 0;
    float riderHeight = 0.0f;
    std::string modelPath;
    return loadRemoteMountModel(displayId, modelId, modelPath, riderHeight) ? modelId : 0u;
}

void EntitySpawner::mountUnitNow(uint64_t guid, uint32_t displayId, bool localPlayer) {
    if (localPlayer) {
        if (displayId == 0) {
            clearMountState();
            if (renderer_)
                if (auto* ac = renderer_->getAnimationController()) ac->clearMount();
            return;
        }
        pendingMountDisplayId_ = displayId;
        processPendingMount();
        return;
    }
    pendingRemotePlayerMounts_.erase(guid);
    if (!applyRemotePlayerMount(guid, displayId)) pendingRemotePlayerMounts_[guid] = displayId;
}

uint32_t EntitySpawner::riderPose(uint64_t guid, uint32_t riderInstance) const {
    // +0xb7c, which 0x0073d5d0 plays on the rider; Mount where the rider's
    // model has not the pose a mount aura's kit gave it.
    uint32_t pose = rendering::mount_seat::kRiderPoseMount;
    if (renderer_)
        if (auto* svs = renderer_->getSpellVisualSystem()) pose = svs->riderPose(guid);
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (pose != rendering::mount_seat::kRiderPoseMount && cr && !cr->hasAnimation(riderInstance, pose))
        pose = rendering::mount_seat::kRiderPoseMount;
    return pose;
}

void EntitySpawner::setRemotePlayerMountDisplayId(uint64_t guid, uint32_t displayId) {
    if (guid == 0) return;
    pendingRemotePlayerMounts_[guid] = displayId;
}

void EntitySpawner::removeRemotePlayerMount(uint64_t guid) {
    auto it = remotePlayerMounts_.find(guid);
    if (it == remotePlayerMounts_.end()) return;
    if (renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer()) {
            if (it->second.instanceId != 0) cr->removeInstance(it->second.instanceId);
            auto playerIt = playerInstances_.find(guid);
            if (playerIt != playerInstances_.end()) {
                cr->playAnimation(playerIt->second, rendering::anim::STAND, true);
            } else if (auto creatureIt = creatureInstances_.find(guid); creatureIt != creatureInstances_.end()) {
                // Down from the seat onto its own feet; the render sync
                // puts it back on the ground.
                cr->playAnimation(creatureIt->second, rendering::anim::STAND, true);
            }
        }
    }
    remotePlayerMounts_.erase(it);
}

void EntitySpawner::queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                                float x, float y, float z, float orientation) {
    pendingTransportRegistrations_.push_back({guid, entry, displayId, x, y, z, orientation});
}

void EntitySpawner::setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation) {
    pendingTransportMoves_[guid] = {x, y, z, orientation};
}

bool EntitySpawner::hasTransportRegistrationPending(uint64_t guid) const {
    return std::any_of(pendingTransportRegistrations_.begin(), pendingTransportRegistrations_.end(),
                       [guid](const PendingTransportRegistration& reg) { return reg.guid == guid; });
}

void EntitySpawner::updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                                 float x, float y, float z, float orientation) {
    for (auto& reg : pendingTransportRegistrations_) {
        if (reg.guid == guid) {
            reg.displayId = displayId;
            reg.x = x; reg.y = y; reg.z = z; reg.orientation = orientation;
            return;
        }
    }
}

// --- Queue API ---

void EntitySpawner::queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                                        float x, float y, float z, float orientation, float scale) {
    if (creatureInstances_.count(guid)) return;
    requestedCreatureDisplayIds_[guid] = displayId;
    if (pendingCreatureSpawnGuids_.count(guid)) {
        // Replace any not-yet-started request. An older async result can still
        // finish later; processAsyncCreatureResults rejects it against the map.
        pendingCreatureSpawns_.erase(
            std::remove_if(pendingCreatureSpawns_.begin(), pendingCreatureSpawns_.end(),
                           [guid](const PendingCreatureSpawn& spawn) {
                               return spawn.guid == guid;
                           }),
            pendingCreatureSpawns_.end());
    }
    pendingCreatureSpawns_.push_back({guid, displayId, x, y, z, orientation, scale});
    pendingCreatureSpawnGuids_.insert(guid);
}

void EntitySpawner::queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                      uint32_t appearanceBytes, uint8_t facialFeatures,
                                      float x, float y, float z, float orientation) {
    if (playerInstances_.count(guid)) return;
    if (pendingPlayerSpawnGuids_.count(guid)) return;
    pendingPlayerSpawns_.push_back({guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation});
    pendingPlayerSpawnGuids_.insert(guid);
}

void EntitySpawner::queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                                          float x, float y, float z, float orientation, float scale) {
    pendingGameObjectSpawns_.push_back({guid, entry, displayId, x, y, z, orientation, scale});
}

void EntitySpawner::queuePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    deferredEquipmentQueue_.push_back({guid, {displayInfoIds, inventoryTypes}});
}

void EntitySpawner::queuePlayerEquipmentFromServer(uint64_t guid,
                                                    const std::array<uint32_t, 19>& displayInfoIds,
                                                    const std::array<uint8_t, 19>& inventoryTypes) {
    serverEquipment_[guid] = {displayInfoIds, inventoryTypes};
    queueDressedEquipment(guid);
}

void EntitySpawner::setPlayerItemOverride(uint64_t guid, int equipSlot, uint32_t displayId, uint8_t inventoryType) {
    if (equipSlot < 0 || equipSlot >= 19) return;
    auto& worn = playerItemOverrides_[guid];
    if (displayId == 0) worn.erase(equipSlot);
    else worn[equipSlot] = {displayId, inventoryType};
    if (worn.empty()) playerItemOverrides_.erase(guid);
    queueDressedEquipment(guid);
}

void EntitySpawner::setNpcHeadItem(uint64_t guid, uint32_t displayId) {
    auto inst = creatureInstances_.find(guid);
    if (inst == creatureInstances_.end() || !renderer_) return;
    if (displayId == 0) npcHeadItems_.erase(inst->second);
    else npcHeadItems_[inst->second] = displayId;
    const auto model = creatureModelIds_.find(guid);
    const auto display = creatureDisplayIds_.find(guid);
    if (model == creatureModelIds_.end() || display == creatureDisplayIds_.end()) return;
    normalizeHumanoidClothingGeosets(inst->second, model->second, display->second);
}

void EntitySpawner::queueDressedEquipment(uint64_t guid) {
    auto base = serverEquipment_.find(guid);
    if (base == serverEquipment_.end()) return;
    ServerEquipment dressed = base->second;
    if (auto worn = playerItemOverrides_.find(guid); worn != playerItemOverrides_.end()) {
        for (const auto& [slot, item] : worn->second) {
            dressed.displayInfoIds[static_cast<size_t>(slot)] = item.first;
            dressed.inventoryTypes[static_cast<size_t>(slot)] = item.second;
        }
    }
    queuePlayerEquipment(guid, dressed.displayInfoIds, dressed.inventoryTypes);
}

// --- Immediate despawn wrappers ---

void EntitySpawner::clearAllQueues() {
    pendingCreatureSpawns_.clear();
    pendingCreatureSpawnGuids_.clear();
    requestedCreatureDisplayIds_.clear();
    creatureSpawnRetryDeadlines_.clear();
    creaturePermanentFailureGuids_.clear();
    deadCreatureGuids_.clear();
    pendingPlayerSpawns_.clear();
    pendingPlayerSpawnGuids_.clear();
    // Waits for a load still running: the worker reads through assetManager_.
    asyncPlayerModelLoads_.clear();
    pendingOnlinePlayerEquipment_.clear();
    deferredEquipmentQueue_.clear();
    pendingGameObjectSpawns_.clear();
    // Including the one that is already partway onto the GPU. An incremental
    // upload survives a map change otherwise: processPendingWmoUploads() only
    // gives up when the renderer pointer is null, and a transition leaves the
    // WMORenderer alive with its contents cleared. The upload then finishes
    // against a renderer that no longer knows the model, calls finishWmoSpawn()
    // on the new map, and puts the old map's building - or a transport, which
    // then registers itself here - into a world it does not belong to.
    pendingWmoUploads_.clear();
    pendingTransportRegistrations_.clear();
    pendingTransportMoves_.clear();
    pendingTransportDoodadBatches_.clear();
    asyncCreatureLoads_.clear();
    asyncCreatureDisplayLoads_.clear();
    asyncEquipmentLoads_.clear();
    asyncNpcCompositeLoads_.clear();
    asyncGameObjectLoads_.clear();
}

void EntitySpawner::despawnAllCreatures() {
    std::vector<uint64_t> guids;
    guids.reserve(creatureInstances_.size());
    for (const auto& [g, _] : creatureInstances_) guids.push_back(g);
    for (auto g : guids) despawnCreature(g);
}

void EntitySpawner::despawnAllPlayers() {
    std::vector<uint64_t> guids;
    guids.reserve(playerInstances_.size());
    for (const auto& [g, _] : playerInstances_) guids.push_back(g);
    for (auto g : guids) despawnPlayer(g);
}

void EntitySpawner::despawnAllGameObjects() {
    std::vector<uint64_t> guids;
    guids.reserve(gameObjectInstances_.size());
    for (const auto& [g, _] : gameObjectInstances_) guids.push_back(g);
    for (auto g : guids) despawnGameObject(g);
}

// --- Methods extracted from Application (with comments preserved) ---

uint32_t EntitySpawner::animationBehavior(uint32_t instanceId) const {
    return animationRecord(instanceId).behavior;
}

EntitySpawner::AnimationRecord EntitySpawner::animationRecord(uint32_t instanceId) const {
    AnimationRecord out;
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    uint32_t animId = 0;
    float time = 0.0f, duration = 0.0f;
    if (!charRenderer || !assetManager_ ||
        !charRenderer->getAnimationState(instanceId, animId, time, duration)) {
        return out;
    }
    out.animId = animId;
    // AnimationData +8 WeaponFlags and +0x18 BehaviorID (0x0071d450); an
    // id not in the table reads as behavior 0x1fa.
    if (!animationDataLoaded_) {
        animationDataDbc_ = assetManager_->loadDBCOptional("AnimationData.dbc");
        animationDataLoaded_ = true;
    }
    const auto& dbc = animationDataDbc_;
    if (!dbc) return out;
    const int32_t row = dbc->findRecordById(animId);
    if (row < 0) return out;
    const auto r = static_cast<uint32_t>(row);
    out.known = true;
    out.weaponFlags = dbc->getFieldCount() > 2 ? dbc->getUInt32(r, 2) : 0;
    out.behavior = dbc->getFieldCount() > 6 ? dbc->getUInt32(r, 6) : animId;
    return out;
}

bool EntitySpawner::resolveUnitWeaponItems(const UnitWeaponEntries& slots, std::array<UnitWeaponItem, 3>& items,
                                           std::array<uint32_t, 3>& displays) const {
    if (!assetManager_ || !gameHandler_) return false;
    auto itemDbc = assetManager_->loadDBCOptional("Item.dbc");
    // WotLK's eight columns: id, class, subclass, sound, material, display,
    // inventory type, sheath. Other layouts wait for the query.
    if (itemDbc && itemDbc->getFieldCount() < 8) itemDbc.reset();

    // Each slot's item: the item query's template, or Item.dbc's row (class,
    // subclass, display, inventory type, sheath) while the query is out.
    items = {};
    displays = {};
    for (size_t i = 0; i < 3; ++i) {
        const uint32_t entry = slots.entries[i];
        if (entry == 0) continue;
        if (slots.byDisplay) {
            // Before WotLK the slot is the display, its INFO pair the rest.
            items[i] = virtualItemInfo(slots.info[i * 2], slots.info[i * 2 + 1]);
            displays[i] = entry;
            continue;
        }
        if (const auto* info = gameHandler_->getItemInfo(entry); info && info->valid) {
            items[i] = {.sheath = info->sheath,
                        .inventoryType = static_cast<uint8_t>(info->inventoryType),
                        .itemClass = info->itemClass,
                        .subClass = info->subClass,
                        .material = static_cast<uint8_t>(info->material)};
            displays[i] = info->displayInfoId;
            continue;
        }
        gameHandler_->ensureItemInfo(entry);
        const int32_t row = itemDbc ? itemDbc->findRecordById(entry) : -1;
        if (row < 0) return false;
        const auto r = static_cast<uint32_t>(row);
        items[i] = {.sheath = itemDbc->getUInt32(r, 7),
                    .inventoryType = static_cast<uint8_t>(itemDbc->getUInt32(r, 6)),
                    .itemClass = itemDbc->getUInt32(r, 1),
                    .subClass = itemDbc->getUInt32(r, 2),
                    .material = static_cast<uint8_t>(itemDbc->getUInt32(r, 4))};
        displays[i] = itemDbc->getUInt32(r, 5);
    }
    return true;
}

bool EntitySpawner::dressUnitWeapons(uint32_t instanceId, const UnitWeaponEntries& slots,
                                     const UnitWeaponDress& dress) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !assetManager_ || !gameHandler_) return false;
    auto displayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayDbc) return false;
    std::array<UnitWeaponItem, 3> items{};
    std::array<uint32_t, 3> displays{};
    if (!resolveUnitWeaponItems(slots, items, displays)) return false;
    const auto& entries = slots.entries;

    // Every point a weapon can be on comes clear first; a change of state
    // moves them between these.
    for (uint32_t point : {attachment::kShield, attachment::kHandRight, attachment::kHandLeft,
                           attachment::kSheathMainHand, attachment::kSheathOffHand,
                           attachment::kSheathShield, attachment::kLargeWeaponLeft,
                           attachment::kLargeWeaponRight, attachment::kHipWeaponLeft,
                           attachment::kHipWeaponRight}) {
        charRenderer->detachWeapon(instanceId, point);
    }
    constexpr WeaponSlot kSlots[3] = {WeaponSlot::MainHand, WeaponSlot::OffHand, WeaponSlot::Ranged};
    UnitWeaponItems present{};
    for (size_t i = 0; i < 3; ++i) present[i] = entries[i] != 0 ? &items[i] : nullptr;
    for (size_t i = 0; i < 3; ++i) {
        if (entries[i] == 0 || displays[i] == 0) continue;
        const uint32_t point = unitWeaponPoint(kSlots[i], present, dress);
        if (point == attachment::kNone) continue;
        const int32_t rec = displayDbc->findRecordById(displays[i]);
        if (rec < 0) continue;
        const auto art = pipeline::readItemDisplayArt(*displayDbc, static_cast<uint32_t>(rec));
        if (art.modelFile.empty()) continue;
        // A shield (inventory type 14) is a shield model; the rest weapons.
        const bool shield = items[i].inventoryType == 14;
        const std::string dir = shield ? "Item\\ObjectComponents\\Shield\\" : "Item\\ObjectComponents\\Weapon\\";
        pipeline::M2Model model;
        if (!loadWeaponM2(dir + art.modelFile, model)) continue;
        std::string texturePath;
        if (!art.textureName.empty()) texturePath = dir + art.textureName + ".blp";
        charRenderer->attachWeapon(instanceId, point, model, nextWeaponModelId_++, texturePath);
    }
    return true;
}

void EntitySpawner::refreshGuildTabards() {
    if (!gameHandler_) return;
    // 0x006e1c60 for every player in view, the active one too:
    // PLAYER_GUILD_TIMESTAMP (UNIT_END + 9) beside PLAYER_GUILDID (+3).
    const uint16_t unitEnd = game::fieldIndex(game::UF::UNIT_END);
    if (unitEnd != 0xFFFF) {
        auto note = [&](uint64_t guid) {
            auto entity = gameHandler_->getEntityManager().getEntity(guid);
            if (!entity || entity->getType() != game::ObjectType::PLAYER) return;
            const uint32_t guildId = entity->getField(static_cast<uint16_t>(unitEnd + 3));
            if (guildId != 0) gameHandler_->noteGuildTimestamp(guildId, entity->getField(static_cast<uint16_t>(unitEnd + 9)));
        };
        note(gameHandler_->getPlayerGuid());
        for (const auto& [guid, instanceId] : playerInstances_) note(guid);
    }

    const uint32_t generation = gameHandler_->guildEmblemGeneration();
    const bool designsArrived = generation != tabardEmblemGeneration_;
    tabardEmblemGeneration_ = generation;
    for (auto it = paintedTabards_.begin(); it != paintedTabards_.end();) {
        const uint64_t guid = it->first;
        PaintedTabard& painted = it->second;
        const auto siteIt = corpseSites_.find(guid);
        if (siteIt == corpseSites_.end() && !playerInstances_.count(guid)) {
            it = paintedTabards_.erase(it);
            continue;
        }
        ++it;
        if (painted.repaintWait > 0) {
            --painted.repaintWait;
            continue;
        }
        // A corpse's guild is CORPSE_FIELD_GUILD; a player's PLAYER_GUILDID.
        const uint32_t guildId =
            siteIt != corpseSites_.end() ? siteIt->second.guildId : gameHandler_->getEntityGuildId(guid);
        bool repaint = guildId != painted.guildId;
        if (!repaint && designsArrived) {
            repaint = game::tabardNeedsRepaint(painted.guildId, painted.emblem, guildId,
                                               gameHandler_->lookupGuildEmblem(guildId));
        }
        if (repaint) {
            painted.repaintWait = 120;
            queuePlayerEquipment(guid, painted.displayIds, painted.inventoryTypes);
        }
    }
}

std::optional<EntitySpawner::UnitWeaponEntries> EntitySpawner::unitWeaponEntries(uint64_t guid,
                                                                             const game::Entity& entity,
                                                                             bool isPlayer) const {
    UnitWeaponEntries out;
    const uint16_t virtualItems = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
    const uint16_t virtualDisplays = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_SLOT_DISPLAY);
    const uint16_t virtualInfoIndex = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_INFO);
    if (isPlayer) {
        // Main hand, off hand, ranged: equipment slots 15 to 17.
        const auto* visible = gameHandler_->getOtherPlayerVisibleEquipment(guid);
        if (!visible) return std::nullopt;
        out.entries = {(*visible)[15], (*visible)[16], (*visible)[17]};
    } else if (virtualItems != 0xFFFF) {
        for (uint16_t i = 0; i < 3; ++i) out.entries[i] = entity.getField(static_cast<uint16_t>(virtualItems + i));
    } else if (virtualDisplays != 0xFFFF && virtualInfoIndex != 0xFFFF) {
        // Classic and TBC: UNIT_VIRTUAL_ITEM_SLOT_DISPLAY and _INFO.
        out.byDisplay = true;
        for (uint16_t i = 0; i < 3; ++i) {
            out.entries[i] = entity.getField(static_cast<uint16_t>(virtualDisplays + i));
        }
        for (uint16_t i = 0; i < 6; ++i) out.info[i] = entity.getField(static_cast<uint16_t>(virtualInfoIndex + i));
    } else {
        return std::nullopt;
    }
    return out;
}

bool EntitySpawner::classMayDrawRanged(uint32_t classId) const {
    if (!assetManager_) return false;
    auto classes = assetManager_->loadDBCOptional("ChrClasses.dbc");
    const int32_t row = classes ? classes->findRecordById(classId) : -1;
    if (row < 0) return false;
    // WotLK's layout: Flags (+0x24) is column 57.
    if (classes->getFieldCount() != 60) return true;
    return (classes->getUInt32(static_cast<uint32_t>(row), 57) & 8u) == 0;
}

void EntitySpawner::setUnitSheathState(uint64_t guid, UnitSheath& sheath, SheathState requested, bool fromServer) {
    if (!gameHandler_) return;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return;
    const bool isPlayer = entity->getType() == game::ObjectType::PLAYER;
    SheathSetInput in{.current = sheath.state, .isPlayer = isPlayer, .fromServer = fromServer,
                      .hasModel = sheath.instanceId != 0};
    std::array<UnitWeaponItem, 3> storage{};
    UnitWeaponItems items{};
    if (const auto slots = unitWeaponEntries(guid, *entity, isPlayer)) {
        std::array<uint32_t, 3> displays{};
        if (resolveUnitWeaponItems(*slots, storage, displays)) {
            for (size_t i = 0; i < 3; ++i) items[i] = slots->entries[i] != 0 ? &storage[i] : nullptr;
        }
    }
    // 0x00721ed0 for the animation the unit plays.
    in.offHandFollowsAnimation = offHandFollowsAnimation(animationBehavior(sheath.instanceId), items[0] != nullptr);
    if (isPlayer) {
        const uint16_t bytes0 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0);
        in.classMayDrawRanged =
            bytes0 != 0xFFFF && classMayDrawRanged((entity->getField(bytes0) >> 8) & 0xFFu);
    } else {
        // The creature cache's row (+0x964) and its type flags (+0xc).
        const auto& creatures = gameHandler_->getCreatureInfoCache();
        const auto it = creatures.find(static_cast<const game::Unit&>(*entity).getEntry());
        in.doNotSheathe = it != creatures.end() && (it->second.typeFlags & kCreatureTypeFlagDoNotSheathe) != 0;
    }
    if (const auto state = sheathStateChange(requested, items, in)) sheath.state = *state;
}

SpellSheathInput EntitySpawner::spellSheathInput(uint32_t spellId, uint32_t displayId) const {
    SpellSheathInput in;
    if (!gameHandler_ || !assetManager_) return in;
    if (const auto attributes = gameHandler_->getSpellAttributes(spellId)) {
        in.known = true;
        in.attributes = *attributes;
    }
    // The visual: SpellVisual +0x1c (HasMissile) with +0x20 (MissileModel),
    // and its precast and cast kits' LeftWeaponEffect and RightWeaponEffect
    // (SpellVisualKit +0x24, +0x28) naming a SpellVisualEffectName record.
    const uint32_t visualId = gameHandler_->getSpellVisualId(spellId);
    const auto* layout = pipeline::getActiveDBCLayout();
    const auto* visualLayout = layout ? layout->getLayout("SpellVisual") : nullptr;
    const auto* kitLayout = layout ? layout->getLayout("SpellVisualKit") : nullptr;
    auto visuals = visualId ? assetManager_->loadDBCOptional("SpellVisual.dbc") : nullptr;
    const int32_t visualRow = visuals ? visuals->findRecordById(visualId) : -1;
    if (visualRow >= 0 && visualLayout) {
        const auto row = static_cast<uint32_t>(visualRow);
        const uint32_t hasMissile = visualLayout->tryField("HasMissile");
        const uint32_t missileModel = visualLayout->tryField("MissileModel");
        if (hasMissile < visuals->getFieldCount() && missileModel < visuals->getFieldCount() &&
            visuals->getUInt32(row, hasMissile) != 0) {
            in.missileModel = static_cast<int32_t>(visuals->getUInt32(row, missileModel));
        }
        auto kits = kitLayout ? assetManager_->loadDBCOptional("SpellVisualKit.dbc") : nullptr;
        auto effects = kits ? assetManager_->loadDBCOptional("SpellVisualEffectName.dbc") : nullptr;
        if (effects) {
            for (const char* kitField : {"PrecastKit", "CastKit"}) {
                const uint32_t kitColumn = visualLayout->tryField(kitField);
                if (kitColumn >= visuals->getFieldCount()) continue;
                const int32_t kitRow = kits->findRecordById(visuals->getUInt32(row, kitColumn));
                if (kitRow < 0) continue;
                for (const char* effectField : {"LeftWeaponEffect", "RightWeaponEffect"}) {
                    const uint32_t column = kitLayout->tryField(effectField);
                    if (column >= kits->getFieldCount()) continue;
                    const uint32_t effect = kits->getUInt32(static_cast<uint32_t>(kitRow), column);
                    if (effect != 0 && effects->findRecordById(effect) >= 0) in.kitWeaponEffect = true;
                }
            }
        }
    }
    // 0x00717a20: the unit's CreatureModelData, whose +4 flag 0x10 keeps
    // the weapons where they are.
    if (in.kitWeaponEffect) {
        if (const auto flags = creatureModelFlags(displayId)) in.modelHoldsEffects = (*flags & 0x10u) == 0;
    }
    return in;
}

std::optional<float> EntitySpawner::kitWeaponEffectHolder(uint32_t renderInstanceId) const {
    if (!gameHandler_ || !assetManager_ || renderInstanceId == 0) return std::nullopt;
    // The unit the instance draws: the player's own, or another's.
    uint64_t guid = 0;
    if (renderer_ && renderer_->getCharacterInstanceId() == renderInstanceId) guid = gameHandler_->getPlayerGuid();
    for (const auto* instances : {&creatureInstances_, &playerInstances_}) {
        for (const auto& [unitGuid, instanceId] : *instances) {
            if (guid == 0 && instanceId == renderInstanceId) guid = unitGuid;
        }
    }
    auto entity = guid ? gameHandler_->getEntityManager().getEntity(guid) : nullptr;
    if (!entity || !entity->isUnit()) return 1.0f;
    const uint32_t displayId = static_cast<const game::Unit&>(*entity).getDisplayId();
    // 0x0073a6c0 and 0x006f8c50: CreatureModelData +4 flag 0x10 holds no
    // weapon effect; +0x60, AttachedEffectScale, sizes one.
    const auto flags = creatureModelFlags(displayId);
    if (flags && (*flags & 0x10u) != 0) return std::nullopt;
    return kitAttachedEffectScale(renderInstanceId);
}

bool EntitySpawner::unarmedKitsShown(uint64_t guid) const {
    // 0x00720400: +0xa30 0x10000, the weapons away (+0xb5c) and no cast (+0xa60).
    auto it = unitSheath_.find(guid);
    if (it == unitSheath_.end()) return false;
    const UnitSheath& sheath = it->second;
    return sheath.kitIdle && sheath.state == SheathState::Unarmed && sheath.castSpellId == 0;
}

float EntitySpawner::kitAttachedEffectScale(uint32_t renderInstanceId) const {
    return kitModelDataScale(renderInstanceId, "AttachedEffectScale");
}

float EntitySpawner::kitWorldEffectScale(uint32_t renderInstanceId) const {
    return kitModelDataScale(renderInstanceId, "WorldEffectScale");
}

uint32_t EntitySpawner::creatureTypeFlags(uint32_t renderInstanceId) const {
    if (!gameHandler_ || renderInstanceId == 0) return 0;
    for (const auto& [guid, instanceId] : creatureInstances_) {
        if (instanceId != renderInstanceId) continue;
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (!entity || entity->getType() != game::ObjectType::UNIT) return 0;
        const auto& creatures = gameHandler_->getCreatureInfoCache();
        const auto it = creatures.find(static_cast<const game::Unit&>(*entity).getEntry());
        return it != creatures.end() ? it->second.typeFlags : 0u;
    }
    return 0;
}

uint64_t EntitySpawner::unitGuidForInstance(uint32_t renderInstanceId) const {
    if (!gameHandler_ || renderInstanceId == 0) return 0;
    if (renderer_ && renderer_->getCharacterInstanceId() == renderInstanceId) return gameHandler_->getPlayerGuid();
    for (const auto* instances : {&creatureInstances_, &playerInstances_}) {
        for (const auto& [unitGuid, instanceId] : *instances) {
            if (instanceId == renderInstanceId) return unitGuid;
        }
    }
    return 0;
}

bool EntitySpawner::unitMeleeDrawn(uint64_t guid) const {
    auto it = unitSheath_.find(guid);
    return it != unitSheath_.end() && it->second.state == SheathState::Melee;
}

float EntitySpawner::unitGeoBoxHeight(uint32_t renderInstanceId) const {
    const auto minZ = modelDataColumn(renderInstanceId, "GeoBoxMinZ");
    const auto maxZ = modelDataColumn(renderInstanceId, "GeoBoxMaxZ");
    return minZ && maxZ ? *maxZ - *minZ : 0.0f;
}

float EntitySpawner::kitModelDataScale(uint32_t renderInstanceId, const char* column) const {
    return modelDataColumn(renderInstanceId, column).value_or(1.0f);
}

std::optional<float> EntitySpawner::modelDataColumn(uint32_t renderInstanceId, const char* column) const {
    if (!gameHandler_ || !assetManager_ || renderInstanceId == 0) return std::nullopt;
    const uint64_t guid = unitGuidForInstance(renderInstanceId);
    auto entity = guid ? gameHandler_->getEntityManager().getEntity(guid) : nullptr;
    if (!entity || !entity->isUnit()) return std::nullopt;
    const uint32_t displayId = static_cast<const game::Unit&>(*entity).getDisplayId();
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* displayLayout = layouts ? layouts->getLayout("CreatureDisplayInfo") : nullptr;
    const auto* modelLayout = layouts ? layouts->getLayout("CreatureModelData") : nullptr;
    const uint32_t scaleField = modelLayout ? modelLayout->tryField(column) : 0xFFFFFFFFu;
    const int32_t displayRow = displays ? displays->findRecordById(displayId) : -1;
    if (displayRow < 0 || !models || scaleField >= models->getFieldCount()) return std::nullopt;
    const int32_t modelRow = models->findRecordById(
        displays->getUInt32(static_cast<uint32_t>(displayRow), displayLayout ? (*displayLayout)["ModelID"] : 1));
    if (modelRow < 0) return std::nullopt;
    return models->getFloat(static_cast<uint32_t>(modelRow), scaleField);
}

void EntitySpawner::onUnitSpellCastBegin(uint64_t guid, uint32_t spellId) {
    auto it = unitSheath_.find(guid);
    if (it == unitSheath_.end() || it->second.instanceId == 0 || !gameHandler_) return;
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return;
    const uint32_t displayId = static_cast<const game::Unit&>(*entity).getDisplayId();
    // 0x007fa2e0 calls 0x00736d30 for each of its reasons; the last stands.
    if (const auto state = spellSheathState(spellSheathInput(spellId, displayId))) {
        setUnitSheathState(guid, it->second, *state, false);
    }
}

void EntitySpawner::onUnitAttackSwing(uint64_t guid) {
    auto it = unitSheath_.find(guid);
    if (it == unitSheath_.end() || it->second.instanceId == 0) return;
    if (it->second.state != SheathState::Melee) setUnitSheathState(guid, it->second, SheathState::Melee, false);
}

void EntitySpawner::updateUnitWeapons() {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer || !gameHandler_ || !assetManager_ || !assetManager_->isInitialized()) return;
    const uint16_t bytes2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
    const uint16_t flagsIndex = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    const uint16_t flags2Index = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
    const uint16_t channelObjectIndex = game::fieldIndex(game::UF::UNIT_FIELD_CHANNEL_OBJECT);
    const uint16_t channelSpellIndex = game::fieldIndex(game::UF::UNIT_CHANNEL_SPELL);
    const uint16_t goBytes1Index = game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1);
    const uint16_t healthIndex = game::fieldIndex(game::UF::UNIT_FIELD_HEALTH);
    const uint64_t localGuid = gameHandler_->getPlayerGuid();
    int budget = MAX_WEAPON_ATTACHES_PER_TICK;

    // The client's own state for a unit, as each of its paths moves it.
    auto updateSheath = [&](uint64_t guid, uint32_t instanceId, const game::Unit& unit) -> UnitSheath& {
        const std::optional<SheathState> field =
            bytes2 != 0xFFFF ? std::optional(static_cast<SheathState>(unit.getField(bytes2) & 0xFFu)) : std::nullopt;
        uint64_t channelObject = 0;
        if (channelObjectIndex != 0xFFFF) {
            channelObject = static_cast<uint64_t>(unit.getField(channelObjectIndex)) |
                            (static_cast<uint64_t>(unit.getField(static_cast<uint16_t>(channelObjectIndex + 1))) << 32);
        }
        auto [it, fresh] = unitSheath_.try_emplace(guid);
        UnitSheath& sheath = it->second;
        if (fresh || sheath.instanceId != instanceId) {
            // 0x0073f660: a new model starts in the field's state; held
            // without the field.
            sheath = {.instanceId = instanceId,
                      .state = field.value_or(SheathState::Melee),
                      .fieldSeen = field,
                      .standSeen = unit.getStandState(),
                      .channelObjectSeen = channelObject};
            return sheath;
        }
        // 0x00737aa0: every unit but the active player takes the field's
        // change, as the server's (CREATURE_TYPEFLAGS 0x10000000 or not).
        if (field && sheath.fieldSeen != field) {
            if (sheath.fieldSeen) {
                if (const auto state = fieldSheathChange(sheath.state, *sheath.fieldSeen, *field, false)) {
                    setUnitSheathState(guid, sheath, *state, true);
                }
            }
            sheath.fieldSeen = field;
        }
        // 0x0073f460 -> 0x0073f060 on a change of its stand state.
        if (unit.getStandState() != sheath.standSeen) {
            sheath.standSeen = unit.getStandState();
            if (standStateSheathes(sheath.state, sheath.standSeen)) {
                setUnitSheathState(guid, sheath, SheathState::Unarmed, false);
            }
        }
        // 0x0073f4f0 -> 0x0073a520 on a change of its channel object.
        if (channelObject != sheath.channelObjectSeen) {
            sheath.channelObjectSeen = channelObject;
            uint32_t objectType = 0;
            if (auto object = channelObject ? gameHandler_->getEntityManager().getEntity(channelObject) : nullptr;
                object && object->getType() == game::ObjectType::GAMEOBJECT) {
                const auto* info = gameHandler_->getCachedGameObjectInfo(
                    static_cast<const game::GameObject&>(*object).getEntry());
                objectType = info ? info->type
                                  : (goBytes1Index != 0xFFFF ? (object->getField(goBytes1Index) >> 8) & 0xFFu : 0);
            }
            const uint32_t channelSpell = channelSpellIndex != 0xFFFF ? unit.getField(channelSpellIndex) : 0;
            if (const auto state = channelSheathState(sheath.state, objectType, channelSpell)) {
                setUnitSheathState(guid, sheath, *state, false);
            }
        }
        // 0x00738180 after each change of animation, cast or attack.
        const auto anim = animationRecord(instanceId);
        const auto* cast = gameHandler_->getUnitCastState(guid);
        const uint32_t castSpellId = cast && cast->casting ? cast->spellId : 0;
        const bool alive = healthIndex == 0xFFFF || unit.getField(healthIndex) > 0;
        const bool attacking = alive && gameHandler_->getUnitMeleeTarget(guid) != 0;
        if (anim.animId != sheath.animId || castSpellId != sheath.castSpellId || attacking != sheath.attacking) {
            sheath.animId = anim.animId;
            sheath.castSpellId = castSpellId;
            sheath.attacking = attacking;
            AnimationSheathInput in{.current = sheath.state,
                                    .animId = anim.animId,
                                    .animKnown = anim.known,
                                    .weaponFlags = anim.weaponFlags,
                                    .behavior = anim.behavior,
                                    .casting = castSpellId != 0,
                                    .attacking = attacking,
                                    .activePlayer = false,
                                    .field = field.value_or(sheath.state)};
            if (castSpellId != 0) {
                const auto attributes = gameHandler_->getSpellAttributes(castSpellId);
                in.castSheathes = attributes && (*attributes & 0x40000u) == 0;
            }
            if (const auto state = animationSheathState(in)) setUnitSheathState(guid, sheath, *state, false);
            if (const auto idle = animationKitIdle(in)) sheath.kitIdle = *idle;
        }
        return sheath;
    };

    auto visit = [&](uint64_t guid, uint32_t instanceId, bool isPlayer) {
        if (instanceId == 0) return;
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        const uint8_t state = static_cast<uint8_t>(updateSheath(guid, instanceId, unit).state);
        if (budget <= 0) return;
        const auto slots = unitWeaponEntries(guid, *entity, isPlayer);
        if (!slots) return;
        const auto& entries = slots->entries;
        const auto& info = slots->info;
        // What else 0x0072dbc0 reads: the disarm bits (0x00718fc0) and
        // whether the animation playing dresses the off hand (0x00721ed0).
        const uint32_t unitFlags = flagsIndex != 0xFFFF ? entity->getField(flagsIndex) & kUnitFlagDisarmed : 0;
        const uint32_t unitFlags2 =
            flags2Index != 0xFFFF ? entity->getField(flags2Index) & kUnitFlag2DisarmOffhand : 0;
        const bool followsAnim = offHandFollowsAnimation(animationBehavior(instanceId), entries[0] != 0);
        const uint32_t modelId = charRenderer->getInstanceModelId(instanceId);
        auto [it, fresh] = unitWeaponsShown_.try_emplace(guid);
        UnitWeaponsShown& shown = it->second;
        if (!fresh && shown.instanceId == instanceId && shown.modelId == modelId && shown.entries == entries &&
            shown.info == info && shown.sheathState == state && shown.unitFlags == unitFlags && shown.unitFlags2 == unitFlags2 &&
            shown.offHandFollowsAnimation == followsAnim) {
            return;
        }
        // 0x00731f40: from ranged to melee the ranged weapon is put away;
        // any other change, or new items, dresses the unit afresh.
        const bool rangedJustPutAway = !fresh && shown.instanceId == instanceId && shown.entries == entries &&
                                       shown.sheathState == static_cast<uint8_t>(SheathState::Ranged) &&
                                       state == static_cast<uint8_t>(SheathState::Melee);
        const UnitWeaponDress dress{.state = static_cast<SheathState>(state),
                                    .rangedJustPutAway = rangedJustPutAway,
                                    .isPlayer = isPlayer,
                                    .unitFlags = unitFlags,
                                    .unitFlags2 = unitFlags2,
                                    .offHandFollowsAnimation = followsAnim};
        if (!dressUnitWeapons(instanceId, *slots, dress)) {
            // An item not known yet: try again once its query is back.
            if (fresh) unitWeaponsShown_.erase(it);
            else shown.instanceId = 0;
            return;
        }
        --budget;
        shown = {.instanceId = instanceId,
                 .modelId = modelId,
                 .entries = entries,
                 .info = info,
                 .sheathState = state,
                 .unitFlags = unitFlags,
                 .unitFlags2 = unitFlags2,
                 .offHandFollowsAnimation = followsAnim};
    };
    for (const auto& [guid, instanceId] : creatureInstances_) visit(guid, instanceId, false);
    for (const auto& [guid, instanceId] : playerInstances_) {
        if (guid != localGuid) visit(guid, instanceId, true);
    }
    // Units gone from the world take their state with them.
    for (auto it = unitSheath_.begin(); it != unitSheath_.end();) {
        if (!creatureInstances_.count(it->first) && !playerInstances_.count(it->first)) it = unitSheath_.erase(it);
        else ++it;
    }
}

void EntitySpawner::buildCharSectionsCache() {
    if (charSectionsCacheBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("CharSections.dbc");
    if (!dbc) return;
    const auto* csL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
    auto csF = pipeline::detectCharSectionsFields(dbc.get(), csL);
    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t race = dbc->getUInt32(r, csF.raceId);
        uint32_t sex = dbc->getUInt32(r, csF.sexId);
        uint32_t section = dbc->getUInt32(r, csF.baseSection);
        uint32_t variation = dbc->getUInt32(r, csF.variationIndex);
        uint32_t color = dbc->getUInt32(r, csF.colorIndex);
        // We only cache sections 0 (skin), 1 (face), 3 (hair), 4 (underwear)
        if (section != 0 && section != 1 && section != 3 && section != 4) continue;
        for (int ti = 0; ti < 3; ti++) {
            std::string tex = dbc->getString(r, csF.texture1 + ti);
            if (tex.empty()) continue;
            charSectionsCache_.emplace(
                charSectionKey(static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                               static_cast<uint8_t>(section), static_cast<uint8_t>(variation),
                               static_cast<uint8_t>(color), ti),
                tex);
        }
    }
    charSectionsCacheBuilt_ = true;
    LOG_INFO("CharSections cache built: ", charSectionsCache_.size(), " entries");
}

std::string EntitySpawner::lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                           uint8_t variation, uint8_t color, int texIndex) const {
    auto it = charSectionsCache_.find(
        charSectionKey(race, sex, section, variation, color, texIndex));
    return (it != charSectionsCache_.end()) ? it->second : std::string();
}

void EntitySpawner::buildCreatureDisplayLookups() {
    if (creatureLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building creature display lookups from DBC files");

    // CreatureDisplayInfo.dbc structure (3.3.5a):
    // Col 0: displayId
    // Col 1: modelId
    // Col 3: extendedDisplayInfoID (link to CreatureDisplayInfoExtra.dbc)
    // Col 6: Skin1 (texture name)
    // Col 7: Skin2
    // Col 8: Skin3
    if (auto cdi = assetManager_->loadDBC("CreatureDisplayInfo.dbc"); cdi && cdi->isLoaded()) {
        const auto* cdiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < cdi->getRecordCount(); i++) {
            CreatureDisplayData data;
            data.modelId = cdi->getUInt32(i, cdiL ? (*cdiL)["ModelID"] : 1);
            data.extraDisplayId = cdi->getUInt32(i, cdiL ? (*cdiL)["ExtraDisplayId"] : 3);
            data.skin1 = cdi->getString(i, cdiL ? (*cdiL)["Skin1"] : 6);
            data.skin2 = cdi->getString(i, cdiL ? (*cdiL)["Skin2"] : 7);
            data.skin3 = cdi->getString(i, cdiL ? (*cdiL)["Skin3"] : 8);
            // How big this display draws its model. One model serves many
            // displays at many sizes - the boar is 0.6 for a piglet and 1.5
            // for a giant across ten of them - and reading none of it drew
            // every one of them at the model's own size. A helboar is one of
            // the reduced ones, which is where it was noticed.
            //
            // Zero occurs in the file (and in more rows on TBC and vanilla)
            // and means unset, not invisible.
            const uint32_t scaleField = cdiL ? (*cdiL)["CreatureModelScale"] : 4;
            if (scaleField != 0xFFFFFFFF && scaleField < cdi->getFieldCount()) {
                const float displayScale = cdi->getFloat(i, scaleField);
                if (displayScale > 0.0f) data.displayScale = displayScale;
            }
            const uint32_t pcField = cdiL ? (*cdiL)["ParticleColorID"] : 0xFFFFFFFFu;
            if (pcField != 0xFFFFFFFFu && pcField < cdi->getFieldCount()) {
                data.particleColorId = cdi->getUInt32(i, pcField);
            }
            displayDataMap_[cdi->getUInt32(i, cdiL ? (*cdiL)["ID"] : 0)] = data;
        }
        LOG_INFO("Loaded ", displayDataMap_.size(), " display→model mappings");
    }

    // CreatureDisplayInfoExtra.dbc structure (3.3.5a):
    // Col 0: ID
    // Col 1: DisplayRaceID
    // Col 2: DisplaySexID
    // Col 3: SkinID
    // Col 4: FaceID
    // Col 5: HairStyleID
    // Col 6: HairColorID
    // Col 7: FacialHairID
    // CreatureDisplayInfoExtra.dbc field layout depends on actual field count:
    //   19 fields: 10 equip slots (8-17), BakeName=18 (no Flags field)
    //   21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
    if (auto cdie = assetManager_->loadDBC("CreatureDisplayInfoExtra.dbc"); cdie && cdie->isLoaded()) {
        const auto* cdieL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfoExtra") : nullptr;
        const uint32_t cdieEquip0 = cdieL ? (*cdieL)["EquipDisplay0"] : 8;
        // Detect actual field count to determine equip slot count and BakeName position
        const uint32_t dbcFieldCount = cdie->getFieldCount();
        int numEquipSlots;
        uint32_t bakeField;
        if (dbcFieldCount <= 19) {
            // 19 fields: 10 equip slots (8-17), BakeName at 18
            numEquipSlots = 10;
            bakeField = 18;
        } else {
            // 21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
            numEquipSlots = 11;
            bakeField = cdieL ? (*cdieL)["BakeName"] : 20;
        }
        uint32_t withBakeName = 0;
        for (uint32_t i = 0; i < cdie->getRecordCount(); i++) {
            HumanoidDisplayExtra extra;
            extra.raceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["RaceID"] : 1));
            extra.sexId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SexID"] : 2));
            extra.skinId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SkinID"] : 3));
            extra.faceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FaceID"] : 4));
            extra.hairStyleId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairStyleID"] : 5));
            extra.hairColorId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairColorID"] : 6));
            extra.facialHairId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FacialHairID"] : 7));
            for (int eq = 0; eq < numEquipSlots; eq++) {
                extra.equipDisplayId[eq] = cdie->getUInt32(i, cdieEquip0 + eq);
            }
            extra.bakeName = cdie->getString(i, bakeField);
            if (!extra.bakeName.empty()) withBakeName++;
            humanoidExtraMap_[cdie->getUInt32(i, cdieL ? (*cdieL)["ID"] : 0)] = extra;
        }
        LOG_DEBUG("Loaded ", humanoidExtraMap_.size(), " humanoid display extra entries (",
                 withBakeName, " with baked textures, ", numEquipSlots, " equip slots, ",
                 dbcFieldCount, " DBC fields, bakeField=", bakeField, ")");
    }

    // CreatureModelData.dbc: modelId (col 0) → modelPath (col 2, .mdx → .m2)
    if (auto cmd = assetManager_->loadDBC("CreatureModelData.dbc"); cmd && cmd->isLoaded()) {
        const auto* cmdL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureModelData") : nullptr;
        for (uint32_t i = 0; i < cmd->getRecordCount(); i++) {
            std::string mdx = cmd->getString(i, cmdL ? (*cmdL)["ModelPath"] : 2);
            if (mdx.empty()) continue;
            if (mdx.size() >= 4) {
                mdx = mdx.substr(0, mdx.size() - 4) + ".m2";
            }
            const uint32_t modelId = cmd->getUInt32(i, cmdL ? (*cmdL)["ID"] : 0);
            modelIdToPath_[modelId] = mdx;
            // The other half of a creature's size. Zero appears in the file
            // and means unset rather than invisible, so it keeps 1.0.
            const uint32_t scaleField = cmdL ? (*cmdL)["ModelScale"] : 4;
            if (scaleField != 0xFFFFFFFF && scaleField < cmd->getFieldCount()) {
                const float modelScale = cmd->getFloat(i, scaleField);
                if (modelScale > 0.0f) modelIdToScale_[modelId] = modelScale;
            }
            // MountHeight (+0x40) and the box (+0x44..+0x58) the client sizes
            // a unit's blob shadow by (0x0071ed80).
            const uint32_t mountField = cmdL ? (*cmdL)["MountHeight"] : 0xFFFFFFFF;
            const uint32_t boxField = cmdL ? (*cmdL)["GeoBoxMinX"] : 0xFFFFFFFF;
            if (mountField != 0xFFFFFFFF && boxField != 0xFFFFFFFF &&
                boxField + 5 < cmd->getFieldCount() && mountField < cmd->getFieldCount()) {
                ModelGeoBox g;
                g.box.min = {cmd->getFloat(i, boxField), cmd->getFloat(i, boxField + 1),
                             cmd->getFloat(i, boxField + 2)};
                g.box.max = {cmd->getFloat(i, boxField + 3), cmd->getFloat(i, boxField + 4),
                             cmd->getFloat(i, boxField + 5)};
                g.mountHeight = cmd->getFloat(i, mountField);
                modelIdToGeoBox_[modelId] = g;
            }
        }
        LOG_INFO("Loaded ", modelIdToPath_.size(), " model→path mappings");
    }

    // ParticleColor.dbc: a display's recolouring of its model's emitters - for
    // each of three slots a start, mid and end colour (client FUN_004ea9e0).
    if (auto pc = assetManager_->loadDBC("ParticleColor.dbc"); pc && pc->isLoaded()) {
        const auto* pcL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ParticleColor") : nullptr;
        if (pcL) {
            const uint32_t fStart = (*pcL)["Start"], fMid = (*pcL)["Mid"], fEnd = (*pcL)["End"];
            if (fEnd != 0xFFFFFFFFu && fEnd + 2 < pc->getFieldCount()) {
                auto rgb = [](uint32_t argb) {
                    return glm::vec3(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                                     (argb & 0xFF) / 255.0f);
                };
                for (uint32_t i = 0; i < pc->getRecordCount(); i++) {
                    std::array<glm::vec3, 9> c{};
                    for (uint32_t slot = 0; slot < 3; ++slot) {
                        c[slot * 3 + 0] = rgb(pc->getUInt32(i, fStart + slot));
                        c[slot * 3 + 1] = rgb(pc->getUInt32(i, fMid + slot));
                        c[slot * 3 + 2] = rgb(pc->getUInt32(i, fEnd + slot));
                    }
                    particleColors_[pc->getUInt32(i, (*pcL)["ID"])] = c;
                }
            }
        }
        LOG_INFO("Loaded ", particleColors_.size(), " particle colour records");
    }

    // CreatureFamily.dbc: the size a beast family grows through as it levels.
    // The client lets this override the display's own size - see
    // creatureRenderScale - so a family's young are not drawn at the size
    // their display alone asks for.
    if (auto fam = assetManager_->loadDBC("CreatureFamily.dbc"); fam && fam->isLoaded()) {
        const auto* famL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureFamily") : nullptr;
        const uint32_t fId = famL ? (*famL)["ID"] : 0;
        const uint32_t fMin = famL ? (*famL)["MinScale"] : 1;
        const uint32_t fMinLvl = famL ? (*famL)["MinScaleLevel"] : 2;
        const uint32_t fMax = famL ? (*famL)["MaxScale"] : 3;
        const uint32_t fMaxLvl = famL ? (*famL)["MaxScaleLevel"] : 4;
        if (fMaxLvl < fam->getFieldCount()) {
            for (uint32_t i = 0; i < fam->getRecordCount(); i++) {
                FamilyScale f;
                f.minScale = fam->getFloat(i, fMin);
                f.minScaleLevel = static_cast<int32_t>(fam->getUInt32(i, fMinLvl));
                f.maxScale = fam->getFloat(i, fMax);
                f.maxScaleLevel = static_cast<int32_t>(fam->getUInt32(i, fMaxLvl));
                familyScale_[fam->getUInt32(i, fId)] = f;
            }
        }
        LOG_INFO("Loaded ", familyScale_.size(), " creature family scales");
    }

    // Resolve gryphon/wyvern display IDs by exact model path so taxi mounts have textures.
    auto toLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto normalizePath = [&](const std::string& p) {
        std::string s = p;
        for (char& c : s) if (c == '/') c = '\\';
        return toLower(s);
    };
    // A display with no skin of its own, on a model that asks for one.
    //
    // CreatureDisplayInfo leaves Skin1 empty in 545 of its rows. 517 of those
    // carry an ExtraDisplayId: they are characters whose appearance is baked
    // from CreatureDisplayInfoExtra instead, and they have to stay empty or the
    // bake never runs. The other 28 are ordinary creatures with nothing to put
    // in the slot their model marks replaceable (texture type 11), so the slot
    // kept the white fallback and the creature drew untextured.
    //
    // 24 of those 28 have a sibling that names one - another display on the
    // same model FILE. Several model IDs point at one .m2: 110 and 3105 are
    // both WaterElemental.mdx, and 3105's only display is the empty one, so
    // matching by model ID finds nothing and matching by path finds the
    // skin every other water elemental in the game uses. That is the same
    // recovery the taxi mounts below do, for the same reason.
    {
        struct SkinDonor {
            uint32_t displayId = 0;
            std::string skin1, skin2, skin3;
        };
        std::unordered_map<std::string, SkinDonor> donorByPath;
        for (const auto& [dispId, data] : displayDataMap_) {
            if (data.skin1.empty()) continue;
            auto itPath = modelIdToPath_.find(data.modelId);
            if (itPath == modelIdToPath_.end()) continue;
            const std::string key = normalizePath(itPath->second);
            auto it = donorByPath.find(key);
            // Lowest display id wins, so the choice does not depend on the
            // order an unordered_map happens to walk in.
            if (it == donorByPath.end() || dispId < it->second.displayId) {
                donorByPath[key] = SkinDonor{dispId, data.skin1, data.skin2, data.skin3};
            }
        }

        int recovered = 0;
        for (auto& [dispId, data] : displayDataMap_) {
            (void)dispId;
            if (!data.skin1.empty() || data.extraDisplayId != 0) continue;
            auto itPath = modelIdToPath_.find(data.modelId);
            if (itPath == modelIdToPath_.end()) continue;
            auto donor = donorByPath.find(normalizePath(itPath->second));
            if (donor == donorByPath.end()) continue;
            data.skin1 = donor->second.skin1;
            data.skin2 = donor->second.skin2;
            data.skin3 = donor->second.skin3;
            ++recovered;
        }
        if (recovered > 0) {
            // Said out loud: this repairs the table the rest of the client
            // trusts, and an NPC drawn white is the only other sign of it.
            LOG_WARNING("Recovered skins for ", recovered,
                     " creature display(s) whose own CreatureDisplayInfo row names none");
        }
    }

    auto resolveDisplayIdForExactPath = [&](const std::string& exactPath) -> uint32_t {
        const std::string target = normalizePath(exactPath);
        // Collect ALL model IDs that map to this path (multiple model IDs can
        // share the same .m2 file, e.g. modelId 147 and 792 both → Gryphon.m2)
        std::vector<uint32_t> modelIds;
        for (const auto& [mid, path] : modelIdToPath_) {
            if (normalizePath(path) == target) {
                modelIds.push_back(mid);
            }
        }
        if (modelIds.empty()) return 0;
        uint32_t bestDisplayId = 0;
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            bool matches = false;
            for (uint32_t mid : modelIds) {
                if (data.modelId == mid) { matches = true; break; }
            }
            if (!matches) continue;
            int score = 0;
            if (!data.skin1.empty()) score += 3;
            if (!data.skin2.empty()) score += 2;
            if (!data.skin3.empty()) score += 1;
            if (score > bestScore) {
                bestScore = score;
                bestDisplayId = dispId;
            }
        }
        return bestDisplayId;
    };

    gryphonDisplayId_ = resolveDisplayIdForExactPath("Creature\\Gryphon\\Gryphon.m2");
    wyvernDisplayId_  = resolveDisplayIdForExactPath("Creature\\Wyvern\\Wyvern.m2");
    gameServices_->gryphonDisplayId = gryphonDisplayId_;
    gameServices_->wyvernDisplayId  = wyvernDisplayId_;
    LOG_INFO("Taxi mount displayIds: gryphon=", gryphonDisplayId_, " wyvern=", wyvernDisplayId_);

    // CharHairGeosets.dbc and CharacterFacialHairStyles.dbc, as the character
    // component reads them (0x004ea050, 0x004ea000).
    appearanceTables_ = core::loadAppearanceGeosetTables(*assetManager_);
    LOG_INFO("Loaded ", appearanceTables_.hair.size(), " hair and ", appearanceTables_.facial.size(),
             " facial hair geoset rows");

    creatureLookupsBuilt_ = true;
}

std::string EntitySpawner::getModelPathForDisplayId(uint32_t displayId) const {
    // WotLK 3.3.5a CreatureDisplayInfo tops out around ~32000; values far
    // beyond that are corrupted update-field data or packet parse errors.
    // Silently reject to avoid pointless DBC lookups and log spam.
    constexpr uint32_t kMaxReasonableDisplayId = 100000;
    if (displayId == 0 || displayId > kMaxReasonableDisplayId) {
        return "";
    }

    if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
    if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";

    // WotLK servers can send display IDs that do not exist in older/local
    // CreatureDisplayInfo datasets. Keep those creatures visible by falling
    // back to a close base model instead of dropping spawn entirely.
    switch (displayId) {
        case 31048: // Diseased Young Wolf variants (AzerothCore WotLK)
        case 31049: // Diseased Wolf variants (AzerothCore WotLK)
            return "Creature\\Wolf\\Wolf.m2";
        default:
            break;
    }

    auto itData = displayDataMap_.find(displayId);
    if (itData == displayDataMap_.end()) {
        // Some sources (e.g., taxi nodes) may provide a modelId directly.
        auto itPath = modelIdToPath_.find(displayId);
        if (itPath != modelIdToPath_.end()) {
            return itPath->second;
        }
        if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
        if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";
        if (warnedMissingDisplayDataIds_.insert(displayId).second) {
            LOG_WARNING("No display data for displayId ", displayId,
                        " (displayDataMap_ has ", displayDataMap_.size(), " entries)");
        }
        return "";
    }

    auto itPath = modelIdToPath_.find(itData->second.modelId);
    if (itPath == modelIdToPath_.end()) {
        if (warnedMissingModelPathIds_.insert(displayId).second) {
            LOG_WARNING("No model path for modelId ", itData->second.modelId,
                        " from displayId ", displayId,
                        " (modelIdToPath_ has ", modelIdToPath_.size(), " entries)");
        }
        return "";
    }

    // Which model a humanoid display actually resolved to, once. An asset
    // overlay can re-point a display id at a different model, and there is no
    // other way from outside to tell "the re-point never reached the client"
    // from "it reached it and the model looks the same". At debug, with the
    // other per-creature lines: WOWEE_LOG_LEVEL=debug when that is the question.
    if (itData->second.extraDisplayId != 0 &&
        humanoidDisplayCanaryCount_ < 5) {
        ++humanoidDisplayCanaryCount_;
        LOG_DEBUG("Humanoid display ", displayId, " -> model ",
                    itData->second.modelId, " -> ", itPath->second);
    }

    return itPath->second;
}

audio::VoiceType EntitySpawner::detectVoiceTypeFromDisplayId(uint32_t displayId) const {
    // Look up display data
    auto itDisplay = displayDataMap_.find(displayId);
    if (itDisplay == displayDataMap_.end() || itDisplay->second.extraDisplayId == 0) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no display data)");
        return audio::VoiceType::GENERIC;  // Not a humanoid or no extra data
    }

    // Look up humanoid extra data (race/sex info)
    auto itExtra = humanoidExtraMap_.find(itDisplay->second.extraDisplayId);
    if (itExtra == humanoidExtraMap_.end()) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no humanoid extra data)");
        return audio::VoiceType::GENERIC;
    }

    uint8_t raceId = itExtra->second.raceId;
    uint8_t sexId = itExtra->second.sexId;

    const char* raceName = "Unknown";
    const char* sexName = (sexId == 0) ? "Male" : "Female";

    // Map (raceId, sexId) to VoiceType
    // Race IDs: 1=Human, 2=Orc, 3=Dwarf, 4=NightElf, 5=Undead, 6=Tauren, 7=Gnome, 8=Troll
    // Sex IDs: 0=Male, 1=Female
    audio::VoiceType result;
    switch (raceId) {
        case 1: raceName = "Human"; result = (sexId == 0) ? audio::VoiceType::HUMAN_MALE : audio::VoiceType::HUMAN_FEMALE; break;
        case 2: raceName = "Orc"; result = (sexId == 0) ? audio::VoiceType::ORC_MALE : audio::VoiceType::ORC_FEMALE; break;
        case 3: raceName = "Dwarf"; result = (sexId == 0) ? audio::VoiceType::DWARF_MALE : audio::VoiceType::DWARF_FEMALE; break;
        case 4: raceName = "NightElf"; result = (sexId == 0) ? audio::VoiceType::NIGHTELF_MALE : audio::VoiceType::NIGHTELF_FEMALE; break;
        case 5: raceName = "Undead"; result = (sexId == 0) ? audio::VoiceType::UNDEAD_MALE : audio::VoiceType::UNDEAD_FEMALE; break;
        case 6: raceName = "Tauren"; result = (sexId == 0) ? audio::VoiceType::TAUREN_MALE : audio::VoiceType::TAUREN_FEMALE; break;
        case 7: raceName = "Gnome"; result = (sexId == 0) ? audio::VoiceType::GNOME_MALE : audio::VoiceType::GNOME_FEMALE; break;
        case 8: raceName = "Troll"; result = (sexId == 0) ? audio::VoiceType::TROLL_MALE : audio::VoiceType::TROLL_FEMALE; break;
        case 10: raceName = "BloodElf"; result = (sexId == 0) ? audio::VoiceType::BLOODELF_MALE : audio::VoiceType::BLOODELF_FEMALE; break;
        case 11: raceName = "Draenei"; result = (sexId == 0) ? audio::VoiceType::DRAENEI_MALE : audio::VoiceType::DRAENEI_FEMALE; break;
        default: result = audio::VoiceType::GENERIC; break;
    }

    LOG_INFO("Voice detection: displayId ", displayId, " -> ", raceName, " ", sexName, " (race=", static_cast<int>(raceId), ", sex=", static_cast<int>(sexId), ")");
    return result;
}

void EntitySpawner::buildGameObjectDisplayLookups() {
    if (gameObjectLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building gameobject display lookups from DBC files");

    // GameObjectDisplayInfo.dbc structure (3.3.5a):
    // Col 0: ID (displayId)
    // Col 1: ModelName
    if (auto godi = assetManager_->loadDBC("GameObjectDisplayInfo.dbc"); godi && godi->isLoaded()) {
        const auto* godiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < godi->getRecordCount(); i++) {
            uint32_t displayId = godi->getUInt32(i, godiL ? (*godiL)["ID"] : 0);
            std::string modelName = godi->getString(i, godiL ? (*godiL)["ModelName"] : 1);
            if (modelName.empty()) continue;
            // GameObjectDisplayInfo names .mdx and .mdl alike; this knew only
            // the first, so a .mdl gameobject had no model path at all.
            modelName = pipeline::modelPathToM2(modelName);
            gameObjectDisplayIdToPath_[displayId] = modelName;
        }
        LOG_INFO("Loaded ", gameObjectDisplayIdToPath_.size(), " gameobject display mappings");
    } else {
        LOG_WARNING("GameObjectDisplayInfo.dbc failed to load - no GO display mappings available");
    }

    if (gameObjectDisplayIdToPath_.empty()) {
        LOG_WARNING("GO display mapping table is EMPTY - game objects will not render");
    }

    gameObjectLookupsBuilt_ = true;
}

std::string EntitySpawner::getGameObjectModelPathForDisplayId(uint32_t displayId) const {
    auto it = gameObjectDisplayIdToPath_.find(displayId);
    if (it == gameObjectDisplayIdToPath_.end()) return "";
    return it->second;
}


/// Which character instance draws `guid`, or 0 for one that is not drawn.
///
/// The player is asked for by name rather than looked up: the local character
/// is not in either map. Three callers - bounds, foot Z and position - each
/// had this, identically, and a fourth reader would have made it four.
uint32_t EntitySpawner::characterInstanceIdForGuid(uint64_t guid) const {
    if (!renderer_) return 0;
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) {
        const uint32_t own = renderer_->getCharacterInstanceId();
        if (own != 0) return own;
    }
    auto pit = playerInstances_.find(guid);
    if (pit != playerInstances_.end()) return pit->second;
    auto cit = creatureInstances_.find(guid);
    if (cit != creatureInstances_.end()) return cit->second;
    return 0;
}

bool EntitySpawner::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (!renderer_) return false;

    // M2 game objects (mailboxes, chests, nodes, etc.) render via the M2
    // renderer, not the character renderer, so their bounds come from a
    // different instance table. Their true world-space visual sphere makes
    // cursor picking track the actual model instead of a flat fallback.
    //
    // WMO game objects (buildings) are intentionally not resolved here: a
    // bounding sphere from a building's AABB diagonal is enormous and would
    // swallow nearby objects in the picker. They keep the conservative
    // fallback sphere used by the click handlers.
    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        const auto& go = goIt->second;
        if (go.isWmo) return false;
        const auto* m2 = renderer_->queryM2Renderer();
        return m2 && m2->getInstanceBounds(go.instanceId, outCenter, outRadius);
    }

    // The query accessors: these three run from the interface every frame
    // while the world may still be recording, and only read.
    const auto* characters = renderer_->queryCharacterRenderer();
    if (!characters) return false;
    const uint32_t instanceId = characterInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return characters->getInstanceBounds(instanceId, outCenter, outRadius);
}

bool EntitySpawner::getRenderFootZForGuid(uint64_t guid, float& outFootZ) const {
    const auto* characters = renderer_ ? renderer_->queryCharacterRenderer() : nullptr;
    if (!characters) return false;
    const uint32_t instanceId = characterInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return characters->getInstanceFootZ(instanceId, outFootZ);
}

bool EntitySpawner::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    const auto* characters = renderer_ ? renderer_->queryCharacterRenderer() : nullptr;
    if (!characters) return false;
    const uint32_t instanceId = characterInstanceIdForGuid(guid);
    if (instanceId == 0) return false;

    return characters->getInstancePosition(instanceId, outPos);
}

EntitySpawner::CachedAttachmentModel
EntitySpawner::getOrLoadAttachmentModel(const std::vector<std::string>& candidatePaths,
                                        const std::string& texturePath) {
    // 1) Geometry: first candidate that yields a valid model wins, parsed at most once.
    std::shared_ptr<pipeline::M2Model> model;
    std::string resolvedPath;
    for (const auto& path : candidatePaths) {
        auto it = attachmentModelData_.find(path);
        if (it != attachmentModelData_.end()) {
            if (!it->second) continue;  // known missing - try the next candidate
            model = it->second;
            resolvedPath = path;
            break;
        }

        auto data = assetManager_->readFile(path);
        if (data.empty()) {
            attachmentModelData_[path] = nullptr;
            continue;
        }
        auto parsed = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(data));
        if (parsed->name.empty()) parsed->name = path;
        // Skin is a sidecar file for WotLK M2s; vanilla embeds it.
        if (parsed->version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(path);
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, *parsed);
        }
        if (!parsed->isValid()) {
            attachmentModelData_[path] = nullptr;
            continue;
        }
        attachmentModelData_[path] = parsed;
        model = std::move(parsed);
        resolvedPath = path;
        break;
    }
    if (!model) return {};

    // 2) Model id is per (geometry, texture) - see attachmentModelIds_.
    const std::string key = resolvedPath + '|' + texturePath;
    auto idIt = attachmentModelIds_.find(key);
    if (idIt == attachmentModelIds_.end()) {
        idIt = attachmentModelIds_.emplace(key, nextCreatureModelId_++).first;
    }
    return CachedAttachmentModel{.modelId = idIt->second, .model = std::move(model)};
}

std::string EntitySpawner::getHumanoidBakePath(uint32_t displayId) const {
    if (!assetManager_) return "";
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return "";
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end() || extra->second.bakeName.empty()) return "";

    // The bakes live in one directory and the dbc names only the file. It is
    // Textures\BakedNpcTextures, which is where the world path has always
    // looked; Creature\Baked is not a directory in any of this client's data,
    // so this returned nothing for every NPC there has ever been.
    const std::string path = "Textures\\BakedNpcTextures\\" + extra->second.bakeName;
    return assetManager_->fileExists(path) ? path : std::string();
}

bool EntitySpawner::getHumanoidAppearance(uint32_t displayId, uint8_t& race,
                                          uint8_t& sex, uint32_t& appearanceBytes,
                                          uint8_t& facialHair) const {
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return false;
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end()) return false;

    race = extra->second.raceId;
    sex = extra->second.sexId;
    facialHair = extra->second.facialHairId;
    appearanceBytes = static_cast<uint32_t>(extra->second.skinId)
                    | (static_cast<uint32_t>(extra->second.faceId) << 8)
                    | (static_cast<uint32_t>(extra->second.hairStyleId) << 16)
                    | (static_cast<uint32_t>(extra->second.hairColorId) << 24);
    // Only where there is a character model to load. This table gives naga,
    // broken, skeletons and a dozen other NPC-only races the same skin-and-face
    // columns a character has - about one row in fourteen - and the character
    // path answers HumanMale for every one of them. A human standing in the
    // target frame where a naga is standing is worse than the naga's own model
    // with no texture on it, which is what the creature path will give.
    return race != 0 && game::hasPlayerModel(static_cast<game::Race>(race));
}

std::vector<std::pair<uint32_t, uint8_t>>
EntitySpawner::getHumanoidEquipment(uint32_t displayId) const {
    std::vector<std::pair<uint32_t, uint8_t>> out;
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end() || disp->second.extraDisplayId == 0) return out;
    auto extra = humanoidExtraMap_.find(disp->second.extraDisplayId);
    if (extra == humanoidExtraMap_.end()) return out;

    // CreatureDisplayInfoExtra's slot order, against the inventory types
    // applyEquipment matches on. The order is the dbc's and the numbers are
    // WoW's INVTYPE_*, and the two have nothing to do with each other - which
    // is why this is written out rather than computed.
    static constexpr uint8_t kInvType[11] = {
        1,   // 0  helm      INVTYPE_HEAD
        3,   // 1  shoulder  INVTYPE_SHOULDER
        4,   // 2  shirt     INVTYPE_BODY
        5,   // 3  chest     INVTYPE_CHEST
        6,   // 4  belt      INVTYPE_WAIST
        7,   // 5  legs      INVTYPE_LEGS
        8,   // 6  feet      INVTYPE_FEET
        9,   // 7  wrist     INVTYPE_WRIST
        10,  // 8  hands     INVTYPE_HAND
        19,  // 9  tabard    INVTYPE_TABARD
        16,  // 10 cape      INVTYPE_CLOAK
    };
    for (int slot = 0; slot < 11; ++slot) {
        const uint32_t did = extra->second.equipDisplayId[slot];
        if (did != 0) out.emplace_back(did, kInvType[slot]);
    }
    return out;
}

std::vector<std::pair<uint32_t, std::string>>
EntitySpawner::getCreatureSkinPaths(uint32_t displayId,
                                    const std::string& modelPath) const {
    std::vector<std::pair<uint32_t, std::string>> out;
    if (!assetManager_) return out;
    auto it = displayDataMap_.find(displayId);
    if (it == displayDataMap_.end()) return out;

    std::string modelDir;
    if (const size_t slash = modelPath.rfind('\\'); slash != std::string::npos) {
        modelDir = modelPath.substr(0, slash + 1);
    }

    // Same resolution the spawner makes: the field may carry a directory or
    // not, and may carry the extension or not.
    auto resolve = [&](const std::string& skinField) -> std::string {
        if (skinField.empty()) return "";
        std::string raw = skinField;
        std::replace(raw.begin(), raw.end(), '/', '\\');
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(),
                  [&](unsigned char c) { return !isSpace(c); }));
        raw.erase(std::find_if(raw.rbegin(), raw.rend(),
                  [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
        if (raw.empty()) return "";

        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool hasExt = lower.size() >= 4 &&
                            lower.compare(lower.size() - 4, 4, ".blp") == 0;
        const bool hasDir = raw.find('\\') != std::string::npos;

        std::vector<std::string> candidates;
        if (hasDir) {
            candidates.push_back(raw);
            if (!hasExt) candidates.push_back(raw + ".blp");
        } else {
            candidates.push_back(modelDir + raw);
            if (!hasExt) candidates.push_back(modelDir + raw + ".blp");
            candidates.push_back(raw);
            if (!hasExt) candidates.push_back(raw + ".blp");
        }
        for (const std::string& c : candidates) {
            if (assetManager_->fileExists(c)) return c;
        }
        return "";
    };

    // The bake first, where the display has one: it is the whole appearance
    // already composited, and the world puts it over these same slots. It is
    // what a goblin, a naga or a broken has instead of skin fields - those are
    // empty on a display whose appearance lives in CreatureDisplayInfoExtra,
    // and a creature loaded from them alone draws as a white silhouette.
    if (const std::string bake = getHumanoidBakePath(displayId); !bake.empty()) {
        for (uint32_t texType : {1u, 11u, 12u, 13u}) out.emplace_back(texType, bake);
    }

    const std::pair<uint32_t, const std::string*> kSkins[] = {
        {11u, &it->second.skin1}, {12u, &it->second.skin2}, {13u, &it->second.skin3},
    };
    for (const auto& [texType, field] : kSkins) {
        std::string path = resolve(*field);
        if (!path.empty()) out.emplace_back(texType, std::move(path));
    }
    return out;
}

// Apply the textures a creature display names: its own skin variations, and
// for a humanoid the composited body, face, hair and equipment. Once per
// display, since the model is shared by every creature that uses it.
//
// Lifted out of spawnOnlineCreature, which was 1477 lines and is now under a
// thousand. Nothing here changed; it moved.
// The animation a creature starts in.
//
// A creature is not always new when the client first draws it. The server may
// have told us it was dead, or working, or eating, before the spawn came off the
// queue - so the pose is chosen from what is already known about it, and only a
// creature with nothing known plays a birth animation. Fades it in either way.
void EntitySpawner::playCreatureSpawnPose(uint64_t guid, uint32_t instanceId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
// Spawn in the correct pose. If the server marked this creature dead before
// the queued spawn was processed, start directly in death animation.
if (const auto pose = corpsePose(guid); pose && corpseGuids_.count(guid) &&
    charRenderer->hasAnimation(instanceId, rendering::anim::DEAD)) {
    // A corpse lies in Dead, or Drowned under water (0x00705b20).
    charRenderer->playAnimation(instanceId, charRenderer->hasAnimation(instanceId, *pose) ? *pose : rendering::anim::DEAD,
                                true);
} else if (deadCreatureGuids_.count(guid)) {
    charRenderer->playAnimation(instanceId, rendering::anim::DEATH, false);
} else {
    // Check if this NPC has a persistent emote state (e.g. working, eating, dancing)
    uint32_t npcEmote = 0;
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::UNIT) {
            npcEmote = std::static_pointer_cast<game::Unit>(entity)->getNpcEmoteState();
        }
    }
    uint32_t npcEmoteAnim = npcEmote != 0
        ? rendering::AnimationController::getEmoteAnimByEmotesId(npcEmote)
        : 0;
    if (npcEmoteAnim == 0) {
        auto activeIt = creatureActiveEmotes_.find(guid);
        if (activeIt != creatureActiveEmotes_.end()) {
            npcEmoteAnim = activeIt->second;
        }
    }
    uint32_t standLoop = 0;
    if (gameHandler_) {
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::UNIT) {
            const uint8_t st = std::static_pointer_cast<game::Unit>(entity)->getStandState();
            if (st != 0) standLoop = rendering::anim::standStateAnims(st).loop;
        }
    }
    if (npcEmoteAnim != 0 && charRenderer->hasAnimation(instanceId, npcEmoteAnim)) {
        creatureActiveEmotes_[guid] = npcEmoteAnim;
        charRenderer->playAnimation(instanceId, npcEmoteAnim, true);
    } else if (standLoop != 0 && charRenderer->hasAnimation(instanceId, standLoop)) {
        // Spawned sitting, sleeping, kneeling or as a corpse (creature_addon.bytes1).
        charRenderer->setRestAnimation(instanceId, standLoop);
        charRenderer->playAnimation(instanceId, standLoop, true);
    } else if (charRenderer->hasAnimation(instanceId, rendering::anim::BIRTH)) {
        // Play birth animation (one-shot) - will return to STAND after
        charRenderer->playAnimation(instanceId, rendering::anim::BIRTH, false);
    } else if (charRenderer->hasAnimation(instanceId, rendering::anim::SPAWN)) {
        charRenderer->playAnimation(instanceId, rendering::anim::SPAWN, false);
    } else {
        charRenderer->playAnimation(instanceId, rendering::anim::STAND, true);
    }
}
charRenderer->startFadeIn(instanceId, 0.5f);
}

// Choose one mesh per clothing group for a character-style NPC.
//
// These models carry every alternative the artists authored - six cloaks, a
// robe skirt and the trousers under it, several scalps - and a model drawn with
// all of them on shows a character wearing all of them at once. The player path
// avoids this by building a geoset set from the character's inventory; an NPC
// has no inventory to build one from, so its equipment comes from
// CreatureDisplayInfoExtra and only the clothing groups are touched. Everything
// else the model authored is left exactly as it is, because on a creature the
// same group numbers mean unrelated geometry.
void EntitySpawner::normalizeHumanoidClothingGeosets(uint32_t instanceId, uint32_t modelId,
                                                     uint32_t displayId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    // A display with a CreatureDisplayInfoExtra row is dressed through the
    // client's character component, as a player is (0x004ed900). Any other
    // model keeps its own geosets: a creature's numbers mean nothing of the
    // kind - elementals carry their wrist pieces in group 8.
    auto itDisplayData = displayDataMap_.find(displayId);
    if (itDisplayData == displayDataMap_.end() || itDisplayData->second.extraDisplayId == 0) return;
    auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
    if (itExtra == humanoidExtraMap_.end()) return;
    const HumanoidDisplayExtra& extra = itExtra->second;

    core::CharacterLook look;
    look.raceId = extra.raceId;
    look.genderId = extra.sexId;
    look.skinId = extra.skinId;
    look.faceId = extra.faceId;
    look.hairGeoset = appearanceTables_.hairGeoset(extra.raceId, extra.sexId, extra.hairStyleId);
    if (const auto* f = appearanceTables_.facialColumns(extra.raceId, extra.sexId, extra.facialHairId)) {
        look.facial = *f;
    }
    // CreatureDisplayInfoExtra's NPCItemDisplay: head, shoulder, shirt, chest,
    // belt, legs, feet, wrist, hands, tabard, cape.
    // A kit's worn head item over it (0x006f82d0), its own back after
    // (0x00723730: NPCItemDisplay[0]).
    const auto headIt = npcHeadItems_.find(instanceId);
    const uint32_t headDisplayId = headIt != npcHeadItems_.end() ? headIt->second : extra.equipDisplayId[0];
    look.worn.head = headDisplayId;
    look.worn.shirt = extra.equipDisplayId[2];
    look.worn.chest = extra.equipDisplayId[3];
    look.worn.belt = extra.equipDisplayId[4];
    look.worn.legs = extra.equipDisplayId[5];
    look.worn.boots = extra.equipDisplayId[6];
    look.worn.gloves = extra.equipDisplayId[8];
    look.worn.tabard = extra.equipDisplayId[9];
    look.worn.cape = extra.equipDisplayId[10];
    charRenderer->setActiveGeosets(
        instanceId, core::modelGeosetsShown(core::characterLookGeosets(*assetManager_, look),
                                            modelSubmeshIds(modelId)));
    // The helm and the shoulders, as the component hangs them on any character
    // (0x004ef0d0, 0x004ef840).
    core::attachHelm(*charRenderer, *assetManager_, instanceId, headDisplayId, extra.raceId,
                     extra.sexId, [this] { return nextWeaponModelId_++; });
    core::attachShoulders(*charRenderer, *assetManager_, instanceId, extra.equipDisplayId[1],
                          [this] { return nextWeaponModelId_++; });

    // The cape's texture, so the cloak mesh shows the cloak rather than the
    // body's texture.
    const uint32_t capeDisplayId = extra.equipDisplayId[10];
    if (capeDisplayId == 0) return;
    auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!itemDisplayDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const int32_t recIdx = itemDisplayDbc->findRecordById(capeDisplayId);
    if (recIdx < 0) return;
    const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
    const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;
    std::vector<std::string> capeNames;
    auto addName = [&](const std::string& n) {
        if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
            capeNames.push_back(n);
        }
    };
    addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), leftTexField));
    addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), rightTexField));
    const bool npcIsFemale = (extra.sexId == 1);
    // Same list, same order, one place: pipeline/item_textures.hpp.
    std::string capeTexturePath;
    for (const auto& raw : capeNames) {
        for (auto& c : pipeline::capeTextureCandidates(raw, npcIsFemale)) {
            if (assetManager_->fileExists(c)) {
                capeTexturePath = c;
                break;
            }
        }
        if (!capeTexturePath.empty()) break;
    }
    if (capeTexturePath.empty()) return;
    rendering::VkTexture* capeTex = charRenderer->loadTexture(capeTexturePath);
    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
    if (capeTex && capeTex != whiteTex) {
        charRenderer->setGroupTextureOverride(instanceId, 15, capeTex);
        if (const auto* md2 = charRenderer->getModelData(modelId)) {
            for (size_t ti = 0; ti < md2->textures.size(); ti++) {
                if (md2->textures[ti].type == 2) {
                    charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), capeTex);
                }
            }
        }
    }
}

// The per-instance colouring of a humanoid NPC: its hair, its skin, and the
// head-detail sheet an HD model draws its ears and eyes from.
//
// Per instance rather than per model, because two NPCs sharing one model still
// have their own hair colour - which is why these are texture slot overrides on
// the instance and not textures on the model.
void EntitySpawner::applyHumanoidInstanceOverrides(uint32_t instanceId, uint32_t modelId,
                                                   uint32_t displayId) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (!charSectionsCacheBuilt_) buildCharSectionsCache();
    auto itDD = displayDataMap_.find(displayId);
    if (itDD != displayDataMap_.end() && itDD->second.extraDisplayId != 0) {
        auto itExtra2 = humanoidExtraMap_.find(itDD->second.extraDisplayId);
        if (itExtra2 != humanoidExtraMap_.end()) {
            const auto& extra = itExtra2->second;
            const auto* md = charRenderer->getModelData(modelId);
            if (md) {
                    // Look up hair texture (section 3) via cache
                    rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    std::string hairPath = lookupCharSection(
                        extra.raceId, extra.sexId, 3, extra.hairStyleId, extra.hairColorId, 0);
                    if (!hairPath.empty()) {
                        rendering::VkTexture* hairTex = charRenderer->loadTexture(hairPath);
                        if (hairTex && hairTex != whiteTex) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 6) {
                                    charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), hairTex);
                                }
                            }
                        }
                    }

                    // The head detail sheet - eyes, mouth, ears, eyelashes -
                    // which an HD humanoid model asks for as texture type 8
                    // and the stock ones have no slot for. CharSections'
                    // second texture on the skin row is where it comes from,
                    // the same as for the player. Without it these slots
                    // keep whatever the model was authored with, and one of
                    // these models was authored with the word 'Ohren' - so
                    // the face detail fell back to the body art and every
                    // NPC wore its skin colour where its eyes should be.
                    {
                        std::string extraPath = lookupCharSection(
                            extra.raceId, extra.sexId, 0, 0, extra.skinId, 1);
                        int extraSlots = 0;
                        // Seven race and sex pairs name no extra art, and
                        // their models still carry 'Ohren' in the slot - a
                        // name that is not a file. The body skin is a poor
                        // substitute for an ear texture and a far better one
                        // than nothing, which is what those ears had.
                        if (extraPath.empty()) {
                            extraPath = lookupCharSection(
                                extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                        }
                        rendering::VkTexture* extraTex =
                            extraPath.empty() ? nullptr : charRenderer->loadTexture(extraPath);
                        if (extraTex && extraTex != whiteTex) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 8) {
                                    charRenderer->setTextureSlotOverride(
                                        instanceId, static_cast<uint16_t>(ti), extraTex);
                                    ++extraSlots;
                                }
                            }
                        }
                        // Three things decide whether an NPC's face is right,
                        // and a wrong face looks the same whichever failed:
                        // the table having the art, the art loading, and the
                        // model having a slot to put it in.
                        if (npcHeadDetailCanaryCount_ < 8) {
                            ++npcHeadDetailCanaryCount_;
                            // The face art this NPC was given, beside the
                            // face it was asked for. CharSections is keyed
                            // on (variation, colour) and a lookup that misses
                            // does not fail - it returns another row, and
                            // another row is another face.
                            const std::string faceLower = lookupCharSection(
                                extra.raceId, extra.sexId, 1, extra.faceId, extra.skinId, 0);
                            const std::string faceUpper = lookupCharSection(
                                extra.raceId, extra.sexId, 1, extra.faceId, extra.skinId, 1);
                            LOG_DEBUG("NPC head detail: displayId=", displayId,
                                        " race=", static_cast<int>(extra.raceId),
                                        " sex=", static_cast<int>(extra.sexId),
                                        " skin=", static_cast<int>(extra.skinId),
                                        " face=", static_cast<int>(extra.faceId),
                                        " extra='", extraPath,
                                        "' loaded=", (extraTex && extraTex != whiteTex ? "yes" : "NO"),
                                        " type8 slots=", extraSlots,
                                        " of ", md->textures.size(), " textures",
                                        " | faceLower='", faceLower,
                                        "' faceUpper='", faceUpper, "'");
                        }
                    }

                    // Look up skin texture (section 0) for per-instance skin color.
                    // Skip when the NPC has a baked texture or composited equipment -
                    // those already encode armor over skin and must not be replaced.
                    bool hasEquipOrBake = !extra.bakeName.empty();
                    if (!hasEquipOrBake) {
                        for (int s = 0; s < 11 && !hasEquipOrBake; s++)
                            if (extra.equipDisplayId[s] != 0) hasEquipOrBake = true;
                    }
                    if (!hasEquipOrBake) {
                        std::string skinPath = lookupCharSection(
                            extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                        if (!skinPath.empty()) {
                            rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                            if (skinTex) {
                                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                    uint32_t tt = md->textures[ti].type;
                                    if (tt == 1 || tt == 11) {
                                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), skinTex);
                                    }
                                }
                            }
                        }
                    }
            }
        }
    }
}

void EntitySpawner::applyCreatureDisplayTextures(uint32_t displayId, uint32_t modelId,
                                                 const CreatureDisplayData& dispData) {
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    auto texStart = std::chrono::steady_clock::now();
    displayIdTexturesApplied_.insert(displayId);

    // Use pre-decoded textures from async creature load (if available)
    auto itPreDec = displayIdPredecodedTextures_.find(displayId);
    bool hasPreDec = (itPreDec != displayIdPredecodedTextures_.end());
    if (hasPreDec) {
        charRenderer->setPredecodedBLPCache(&itPreDec->second);
    }

    // Creature skin names are relative to the model's own directory, so the
    // path is asked for here rather than passed in - the caller had it only
    // because it needed it for something else.
    const std::string m2Path = getModelPathForDisplayId(displayId);
    std::string modelDir;
    const size_t lastSlash = m2Path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        modelDir = m2Path.substr(0, lastSlash + 1);
    }

    LOG_DEBUG("DisplayId ", displayId, " skins: '", dispData.skin1, "', '", dispData.skin2, "', '", dispData.skin3,
              "' extraDisplayId=", dispData.extraDisplayId);

    // Get model data from CharacterRenderer for texture iteration
    const auto* modelData = charRenderer->getModelData(modelId);
    if (!modelData) {
        LOG_WARNING("Model data not found for modelId ", modelId);
    }

    // Log texture types in the model
    if (modelData) {
    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
        LOG_DEBUG("  Model texture ", ti, ": type=", modelData->textures[ti].type, " filename='", modelData->textures[ti].filename, "'");
    }
    }

    // Check if this is a humanoid NPC with extra display info
    bool hasHumanoidTexture = false;
    if (dispData.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(dispData.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            LOG_DEBUG("  Found humanoid extra: raceId=", static_cast<int>(extra.raceId), " sexId=", static_cast<int>(extra.sexId),
                      " hairStyle=", static_cast<int>(extra.hairStyleId), " hairColor=", static_cast<int>(extra.hairColorId),
                      " bakeName='", extra.bakeName, "'");

            // Collect model texture slot info (type 1 = skin, type 6 = hair)
            std::vector<uint32_t> skinSlots, hairSlots;
            // Is this one of the replacement character models, rather than a
            // model the game shipped? The baked NPC textures were composited
            // for the shipped ones and are not interchangeable.
            //
            // Told by size, because size separates them cleanly and nothing
            // else does. The twenty character models the game ships run from
            // 3078 vertices to 8737; the twenty replacements run from 15051
            // to 87569. There is no overlap and the gap is wide.
            //
            // A Skin Extra slot was tried as the marker first and is not
            // one: only twelve of the twenty replacements carry it, and the
            // eight without - human male, dwarf, undead male, gnome, troll
            // male, draenei female - were exactly the ones left wrong.
            constexpr size_t kShippedCharacterVertexCeiling = 12000;
            const bool isHdCharacterModel =
                modelData && modelData->vertices.size() > kShippedCharacterVertexCeiling;
            if (modelData) {
                for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                    uint32_t texType = modelData->textures[ti].type;
                    if (texType == 1 || texType == 11 || texType == 12 || texType == 13)
                        skinSlots.push_back(static_cast<uint32_t>(ti));
                    if (texType == 6)
                        hairSlots.push_back(static_cast<uint32_t>(ti));
                }
            }

            // Copy extra data for the async task (avoid dangling reference)
            HumanoidDisplayExtra extraCopy = extra;

            // Launch async task: ALL DBC lookups, path resolution, and BLP pre-decode
            // happen on a background thread. Only GPU texture upload runs on main thread
            // (in processAsyncNpcCompositeResults).
            auto* am = assetManager_;
            AsyncNpcCompositeLoad load;
            load.future = std::async(std::launch::async,
                [am, extraCopy, skinSlots = std::move(skinSlots),
                 hairSlots = std::move(hairSlots), modelId, displayId, isHdCharacterModel]() mutable -> PreparedNpcComposite {
                    PreparedNpcComposite result;
                    DeferredNpcComposite& def = result.info;
                    def.modelId = modelId;
                    def.displayId = displayId;
                    def.skinTextureSlots = std::move(skinSlots);
                    def.hairTextureSlots = std::move(hairSlots);

                    std::vector<std::string> allPaths;  // paths to pre-decode

                    // --- Baked skin texture ---
                    //
                    // A bake is one image with the skin, the face and the
                    // armour already composited into it, made for a
                    // particular model. Taking it means CharSections is
                    // never read at all - which is the whole difference
                    // between this path and the portrait's, and why a
                    // portrait's face is right where the same NPC's is not.
                    //
                    // It is only right for the model it was baked for. These
                    // displays now point at the HD character models, whose
                    // faces the bakes know nothing about, so those composite
                    // from the table instead - the same way the portrait
                    // always has.
                    if (!extraCopy.bakeName.empty() && !isHdCharacterModel) {
                        def.bakedSkinPath = "Textures\\BakedNpcTextures\\" + extraCopy.bakeName;
                        def.hasBakedSkin = true;
                        allPaths.push_back(def.bakedSkinPath);
                    }

                    // --- CharSections fallback (skin/face/underwear) ---
                    if (!def.hasBakedSkin) {
                        auto csDbc = am->loadDBC("CharSections.dbc");
                        if (csDbc) {
                            const auto* csL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                            auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                            uint32_t npcRace = static_cast<uint32_t>(extraCopy.raceId);
                            uint32_t npcSex = static_cast<uint32_t>(extraCopy.sexId);
                            uint32_t npcSkin = static_cast<uint32_t>(extraCopy.skinId);
                            uint32_t npcFace = static_cast<uint32_t>(extraCopy.faceId);
                            std::string npcFaceLower, npcFaceUpper;
                            std::vector<std::string> npcUnderwear;

                            // The one reader, in pipeline/char_sections.hpp.
                            //
                            // This copy had no fallback for a face the
                            // table does not carry, and no reading of the
                            // skin row's second texture. Both come with the
                            // conversion; the second is what an HD model
                            // draws its ears and eyelashes from.
                            pipeline::CharacterAppearance who;
                            who.raceId = npcRace;
                            who.sexId = npcSex;
                            who.skinId = static_cast<uint8_t>(npcSkin);
                            who.faceId = static_cast<uint8_t>(npcFace);
                            who.hairStyleId = extraCopy.hairStyleId;
                            who.hairColorId = extraCopy.hairColorId;

                            const auto sections = pipeline::resolveCharacterSections(
                                csDbc.get(), csF, who,
                                [](const std::string& path, void* ctx) {
                                    return static_cast<pipeline::AssetManager*>(ctx)->fileExists(path);
                                },
                                am);

                            def.basePath = sections.bodySkin;
                            npcFaceLower = sections.faceLower;
                            npcFaceUpper = sections.faceUpper;
                            npcUnderwear = sections.underwear;

                            if (!def.basePath.empty()) {
                                allPaths.push_back(def.basePath);
                                if (!npcFaceLower.empty()) { def.overlayPaths.push_back(npcFaceLower); allPaths.push_back(npcFaceLower); }
                                if (!npcFaceUpper.empty()) { def.overlayPaths.push_back(npcFaceUpper); allPaths.push_back(npcFaceUpper); }
                                for (const auto& uw : npcUnderwear) { def.overlayPaths.push_back(uw); allPaths.push_back(uw); }
                            }
                        }
                    }

                    // --- Equipment region layers (ItemDisplayInfo DBC) ---
                    // The character component's layers (0x004f2880, the table
                    // at 0x009f6a00), drawn layer by layer (0x004e8f00):
                    // CreatureDisplayInfoExtra's shirt, chest, belt, legs,
                    // boots, wrists, gloves and tabard are its items 2 to 9;
                    // the helm, shoulders and cape are models.
                    auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                    if (idiDbc) {
                        std::vector<core::ComponentItem> componentItems;
                        for (int eqSlot = 2; eqSlot <= 9; ++eqSlot) {
                            const uint32_t did = extraCopy.equipDisplayId[eqSlot];
                            if (did != 0) componentItems.push_back({core::componentItemForNpcSlot(eqSlot), did});
                        }
                        def.regionLayers = core::characterComponentLayers(*am, *idiDbc, componentItems,
                                                                          extraCopy.sexId == 1, std::nullopt);
                        for (const auto& layer : def.regionLayers) allPaths.push_back(layer.second);
                    }

                    // Determine compositing mode
                    if (!def.basePath.empty()) {
                        bool needsComposite = !def.overlayPaths.empty() || !def.regionLayers.empty();
                        if (needsComposite && !def.skinTextureSlots.empty()) {
                            def.hasComposite = true;
                        } else if (!def.skinTextureSlots.empty()) {
                            def.hasSimpleSkin = true;
                        }
                    }

                    // --- Hair texture from CharSections ---
                    // The one reader again, rather than a sixth scan of the
                    // table written out by hand. Only the hair is wanted here.
                    {
                        auto csDbc = am->loadDBC("CharSections.dbc");
                        if (csDbc) {
                            const auto* csL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                            const auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                            pipeline::CharacterAppearance who;
                            who.raceId = extraCopy.raceId;
                            who.sexId = extraCopy.sexId;
                            who.skinId = extraCopy.skinId;
                            who.faceId = extraCopy.faceId;
                            who.hairStyleId = extraCopy.hairStyleId;
                            who.hairColorId = extraCopy.hairColorId;
                            def.hairTexturePath =
                                pipeline::resolveCharacterSections(csDbc.get(), csF, who).hair;

                            if (!def.hairTexturePath.empty()) {
                                allPaths.push_back(def.hairTexturePath);
                            } else if (def.hasBakedSkin && !def.hairTextureSlots.empty()) {
                                def.useBakedForHair = true;
                                // bakedSkinPath already in allPaths
                            }
                        }
                    }

                    // --- Pre-decode all BLP textures on this background thread ---
                    for (const auto& path : allPaths) {
                        std::string key = path;
                        std::replace(key.begin(), key.end(), '/', '\\');
                        std::transform(key.begin(), key.end(), key.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.count(key)) continue;
                        auto blp = am->loadTexture(key);
                        if (blp.isValid()) {
                            result.predecodedTextures[key] = std::move(blp);
                        }
                    }

                    return result;
                });
            asyncNpcCompositeLoads_.push_back(std::move(load));
            hasHumanoidTexture = true;  // skip non-humanoid skin block
        } else {
            LOG_WARNING("  extraDisplayId ", dispData.extraDisplayId, " not found in humanoidExtraMap");
        }
    }

    // Apply creature skin textures (for non-humanoid creatures)
    if (!hasHumanoidTexture && modelData) {
        auto resolveCreatureSkinPath = [&](const std::string& skinField) -> std::string {
            if (skinField.empty()) return "";

            std::string raw = skinField;
            std::replace(raw.begin(), raw.end(), '/', '\\');
            auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [&](unsigned char c) { return !isSpace(c); }));
            raw.erase(std::find_if(raw.rbegin(), raw.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
            if (raw.empty()) return "";

            auto hasBlpExt = [](const std::string& p) {
                if (p.size() < 4) return false;
                std::string ext = p.substr(p.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return ext == ".blp";
            };
            auto addCandidate = [](std::vector<std::string>& out, const std::string& p) {
                if (p.empty()) return;
                if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
            };

            std::vector<std::string> candidates;
            const bool hasDir = (raw.find('\\') != std::string::npos || raw.find('/') != std::string::npos);
            const bool hasExt = hasBlpExt(raw);

            if (hasDir) {
                addCandidate(candidates, raw);
                if (!hasExt) addCandidate(candidates, raw + ".blp");
            } else {
                addCandidate(candidates, modelDir + raw);
                if (!hasExt) addCandidate(candidates, modelDir + raw + ".blp");
                addCandidate(candidates, raw);
                if (!hasExt) addCandidate(candidates, raw + ".blp");
            }

            for (const auto& c : candidates) {
                if (assetManager_->fileExists(c)) return c;
            }
            return "";
        };

        for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
            const auto& tex = modelData->textures[ti];
            std::string skinPath;

            // Creature skin types: 11 = skin1, 12 = skin2, 13 = skin3
            if (tex.type == 11 && !dispData.skin1.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin1);
            } else if (tex.type == 12 && !dispData.skin2.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin2);
            } else if (tex.type == 13 && !dispData.skin3.empty()) {
                skinPath = resolveCreatureSkinPath(dispData.skin3);
            }

            if (!skinPath.empty()) {
                rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                if (skinTex) {
                    charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                    LOG_DEBUG("Applied creature skin texture: ", skinPath, " to slot ", ti);
                } else {
                    // The row named a skin, the path was built, and the file
                    // behind it did not load. Said nothing before: the slot
                    // kept whatever it had and the creature drew wrong with
                    // no line anywhere saying which texture had not arrived.
                    LOG_WARNING("Creature skin did not load for displayId ", displayId,
                                " slot ", ti, " type ", tex.type, ": ", skinPath);
                }
            } else if ((tex.type == 11 && !dispData.skin1.empty()) ||
                       (tex.type == 12 && !dispData.skin2.empty()) ||
                       (tex.type == 13 && !dispData.skin3.empty())) {
                LOG_WARNING("Creature skin texture not found for displayId ", displayId,
                            " slot ", ti, " type ", tex.type,
                            " (skin fields: '", dispData.skin1, "', '",
                            dispData.skin2, "', '", dispData.skin3, "')");
            }
        }
    }

    // Clear pre-decoded cache after applying all display textures
    charRenderer->setPredecodedBLPCache(nullptr);
    displayIdPredecodedTextures_.erase(displayId);
    {
        auto texEnd = std::chrono::steady_clock::now();
        float texMs = std::chrono::duration<float, std::milli>(texEnd - texStart).count();
        if (texMs > 50.0f) {
            LOG_WARNING("spawnCreature texture setup took ", texMs, "ms displayId=", displayId,
                        " hasPreDec=", hasPreDec, " extra=", dispData.extraDisplayId);
        }
    }
}

float EntitySpawner::creatureModelScale(uint32_t displayId) const {
    auto disp = displayDataMap_.find(displayId);
    if (disp == displayDataMap_.end()) return 1.0f;
    auto it = modelIdToScale_.find(disp->second.modelId);
    return it != modelIdToScale_.end() ? it->second : 1.0f;
}

float EntitySpawner::creatureDisplayScale(uint32_t displayId) const {
    auto it = displayDataMap_.find(displayId);
    return it != displayDataMap_.end() ? it->second.displayScale : 1.0f;
}

namespace {
// OBJECT_FIELD_SCALE_X as the spawn path reads it: unset or implausible is 1.0.
float unitServerScale(const game::Entity& entity) {
    const uint16_t idx = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    if (idx == 0xFFFF) return 1.0f;
    const uint32_t raw = entity.getField(idx);
    if (raw == 0) return 1.0f;
    const float s = std::bit_cast<float>(raw);
    return (s > 0.01f && s <= 100.0f) ? s : 1.0f;
}
} // namespace

float EntitySpawner::creatureRenderScale(uint64_t guid, uint32_t displayId) const {
    // The 3.3.5a client (12340, FUN_0071c110) sizes a creature as the
    // display's scale times its model's, and then lets the creature's beast
    // family override that: the family grows linearly from minScale at
    // minScaleLevel to maxScale at maxScaleLevel, and where that is larger -
    // or the unit is a hunter pet, which always takes it - it wins. A
    // Springpaw Cub is a cat on a 0.4 display, so it is drawn at no less
    // than the cat family's size for its level.
    if (!gameHandler_) return creatureRenderScaleFor(nullptr, displayId);
    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    return creatureRenderScaleFor(entity.get(), displayId);
}

float EntitySpawner::creatureRenderScaleFor(const game::Entity* entity, uint32_t displayId) const {
    float s = creatureDisplayScale(displayId) * creatureModelScale(displayId);
    if (!(s > 0.0f)) s = 1.0f;

    if (!gameHandler_ || !entity || entity->getType() != game::ObjectType::UNIT) return s;
    const auto* unit = static_cast<const game::Unit*>(entity);

    // The family arrives with the creature query response, which can be
    // later than the spawn; refreshCreatureScales picks it up then.
    const uint32_t familyId = gameHandler_->getCreatureFamily(unit->getEntry());
    auto famIt = familyScale_.find(familyId);
    if (familyId == 0 || famIt == familyScale_.end()) return s;
    const FamilyScale& fam = famIt->second;

    const int32_t level = static_cast<int32_t>(unit->getLevel());
    const int32_t range = fam.maxScaleLevel - fam.minScaleLevel;
    int32_t into = level < fam.minScaleLevel ? 0 : level - fam.minScaleLevel;
    if (into > range) into = range;
    const float t = range != 0 ? static_cast<float>(into) / static_cast<float>(range) : 0.0f;
    const float familySize = fam.minScale + (fam.maxScale - fam.minScale) * t;

    const uint16_t petIdx = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
    const bool isPet = petIdx != 0xFFFF && unit->getField(petIdx) != 0;
    return (familySize > s || isPet) ? familySize : s;
}

void EntitySpawner::refreshCreatureScales() {
    // Level, family and the server's scale can all change, or arrive, after
    // the instance exists. A few sweeps a second is plenty for a size.
    if (++scaleSyncFrameCounter_ % 15 != 0) return;
    if (!renderer_ || !gameHandler_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    const auto& entities = gameHandler_->getEntityManager().getEntities();
    for (const auto& [guid, instanceId] : creatureInstances_) {
        auto entIt = entities.find(guid);
        if (entIt == entities.end() || !entIt->second) continue;
        auto dispIt = creatureDisplayIds_.find(guid);
        if (dispIt == creatureDisplayIds_.end()) continue;
        const float want = unitServerScale(*entIt->second) *
                           creatureRenderScaleFor(entIt->second.get(), dispIt->second);
        auto& applied = creatureAppliedScale_[guid];
        if (std::abs(applied - want) > 1e-4f) {
            charRenderer->setInstanceScale(instanceId, want);
            applied = want;
            // Its mount is its size times the mount display's (0x0071c0e0).
            if (auto mountIt = remotePlayerMounts_.find(guid); mountIt != remotePlayerMounts_.end()) {
                RemotePlayerMount& mount = mountIt->second;
                mount.scale = rendering::mount_seat::mountModelScale(want, creatureDisplayScale(mount.displayId));
                mount.riderHeight = rendering::mount_seat::seatHeight(mount.seatZ, mount.scale);
                charRenderer->setInstanceScale(mount.instanceId, mount.scale);
            }
        }
    }
}

void EntitySpawner::spawnCreatureParticleTwin(uint64_t guid, uint32_t displayId,
                                              uint32_t charModelId, uint32_t charInstanceId) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!charRenderer || !m2) return;
    const pipeline::M2Model* data = charRenderer->getModelData(charModelId);
    if (!data || (data->particleEmitters.empty() && data->ribbonEmitters.empty())) return;

    // The same model, loaded once into the M2 renderer for its emitters and
    // drawn for nothing else.
    const uint32_t twinModelId = 0x5A000000u + charModelId;
    if (!m2->hasModel(twinModelId)) {
        if (!m2->loadModel(*data, twinModelId)) return;
        m2->setModelParticlesOnly(twinModelId);
    }
    glm::mat4 model(1.0f);
    const std::vector<glm::mat4>* bones = nullptr;
    int seq = 0;
    float t = 0.0f, gt = 0.0f;
    if (!charRenderer->getInstancePose(charInstanceId, model, bones, seq, t, gt)) return;
    const uint32_t twin = m2->createInstance(twinModelId, glm::vec3(model[3]), glm::vec3(0.0f), 1.0f,
                                            /*allowPositionDedup=*/false);
    if (twin == 0) return;
    m2->setInstanceExternalPose(twin, model, *bones, seq, t, gt);
    if (auto disp = displayDataMap_.find(displayId);
        disp != displayDataMap_.end() && disp->second.particleColorId != 0) {
        if (auto pc = particleColors_.find(disp->second.particleColorId); pc != particleColors_.end()) {
            m2->setInstanceParticleColors(twin, pc->second);
        }
    }
    creatureParticleTwins_[guid] = twin;
}

void EntitySpawner::syncCreatureParticleTwins() {
    if (creatureParticleTwins_.empty()) return;
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!charRenderer || !m2) return;
    for (const auto& [guid, twin] : creatureParticleTwins_) {
        auto it = creatureInstances_.find(guid);
        if (it == creatureInstances_.end()) continue;
        glm::mat4 model(1.0f);
        const std::vector<glm::mat4>* bones = nullptr;
        int seq = 0;
        float t = 0.0f, gt = 0.0f;
        if (charRenderer->getInstancePose(it->second, model, bones, seq, t, gt) && bones) {
            m2->setInstanceExternalPose(twin, model, *bones, seq, t, gt);
        }
    }
}

void EntitySpawner::removeCreatureParticleTwin(uint64_t guid) {
    auto it = creatureParticleTwins_.find(guid);
    if (it == creatureParticleTwins_.end()) return;
    if (auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr) m2->removeInstance(it->second);
    creatureParticleTwins_.erase(it);
}

void EntitySpawner::spawnOnlineCreature(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_) return;

    // Skip if lookups not yet built (asset manager not ready)
    if (!creatureLookupsBuilt_) return;

    // Skip if already spawned
    if (creatureInstances_.count(guid)) return;
    if (nonRenderableCreatureDisplayIds_.count(displayId)) {
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }

    // Get model path from displayId
    std::string m2Path = getModelPathForDisplayId(displayId);
    if (m2Path.empty()) {
        nonRenderableCreatureDisplayIds_.insert(displayId);
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }
    {
        // Intentionally invisible helper creatures should not consume retry budget.
        std::string lowerPath = m2Path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (rendering::isHelperCreatureModel(lowerPath)) {
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }
    }

    auto* charRenderer = renderer_->getCharacterRenderer();

    // The model must already be in the cache: callers spawn only once the async load
    // has published it. Reading and parsing the M2 here instead would put file I/O on
    // the main thread mid-frame, so treat a miss as "not ready" and let the caller
    // re-queue the spawn through the async path.
    auto cacheIt = displayIdModelCache_.find(displayId);
    if (cacheIt == displayIdModelCache_.end()) {
        LOG_WARNING("spawnOnlineCreature: model not loaded yet for displayId=", displayId,
                    " - deferring to async load");
        return;
    }
    const uint32_t modelId = cacheIt->second;

    // What the server sent is only one of the three terms. A creature draws at
    // the model's own scale, times the size this display asks for, times the
    // per-unit scale the server sets - and only the last of those was applied,
    // so every display sharing a model came out the same size. That is one
    // model at 0.6 and at 1.5 both drawing at 1.0.
    const float dispScale = creatureDisplayScale(displayId);
    const float serverScale = scale;
    scale *= creatureRenderScale(guid, displayId);

    // Measured: this server sends 1.0 for every creature whose display asks
    // for a size of its own, so it does not fold CreatureDisplayInfo's scale
    // into the unit field and the multiply above is the client's to make.
    // What each display actually resolves to, once per display, because the
    // three terms are in three files and only their product is visible. This
    // is how the Greater Duskbat was settled: entry 1553, display 4734, a
    // FelBat at 0.15, which is a 2.2 yard wingspan and exactly what the data
    // asks for - the size came from CreatureDisplayInfo, not from here.
    {
        static std::set<uint32_t> saidDisplay;
        if (saidDisplay.size() < 60 && saidDisplay.insert(displayId).second) {
            std::string path = "?";
            uint32_t entry = 0;
            std::string name;
            if (auto disp = displayDataMap_.find(displayId); disp != displayDataMap_.end()) {
                if (auto pi = modelIdToPath_.find(disp->second.modelId); pi != modelIdToPath_.end()) {
                    path = pi->second;
                }
            }
            if (gameHandler_) {
                if (auto e = gameHandler_->getEntityManager().getEntity(guid)) {
                    if (e->getType() == game::ObjectType::UNIT) {
                        auto u = std::static_pointer_cast<game::Unit>(e);
                        entry = u->getEntry();
                        name = u->getName();
                    }
                }
            }
            // What every creature question needs first - a bat's size, a
            // goblin's portrait, an elemental's skin and an elemental's
            // geometry each began by working out which model a creature name
            // draws. At debug: sixty displays once each is most of a session's
            // log, and WOWEE_LOG_LEVEL=debug brings it back for the question.
            LOG_DEBUG("Creature display ", displayId, " (", (name.empty() ? "?" : name),
                        ", entry ", entry, ") draws ", path,
                        " at ", scale, " (server ", serverScale,
                        " x display ", dispScale,
                        " x model ", creatureModelScale(displayId),
                        ", family-adjusted ", creatureRenderScale(guid, displayId), ")");
        }
    }

    // Apply skin textures from CreatureDisplayInfo.dbc (only once per displayId model).
    // Track separately from model cache because async loading may upload the model
    // before textures are applied.
    auto itDisplayData = displayDataMap_.find(displayId);
    bool needsTextures = (displayIdTexturesApplied_.find(displayId) == displayIdTexturesApplied_.end());
    if (needsTextures && itDisplayData != displayDataMap_.end()) {
        applyCreatureDisplayTextures(displayId, modelId, itDisplayData->second);
    }

    // Use the entity's latest server-authoritative position rather than the stale spawn
    // position. Movement packets (SMSG_MONSTER_MOVE) can arrive while a creature is still
    // queued in pendingCreatureSpawns_ and get silently dropped. getLatestX/Y/Z returns
    // the movement destination if the entity is mid-move, which is always up-to-date
    // regardless of distance culling (unlike getX/Y/Z which requires updateMovement).
    if (gameHandler_) {
        if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
            x = entity->getLatestX();
            y = entity->getLatestY();
            z = entity->getLatestZ();
            orientation = entity->getOrientation();
        }
    }

    // Convert canonical → render coordinates
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));

    // Keep authoritative server Z for online creature spawns.
    // Terrain-based lifting can incorrectly move elevated NPCs (e.g. flight masters on
    // Stormwind ramparts) to bad heights relative to WMO geometry.

    // Convert canonical WoW orientation (0=north) -> render yaw (0=west)
    float renderYaw = orientation + glm::radians(90.0f);

    // Create instance (apply server-provided scale from OBJECT_FIELD_SCALE_X)
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos,
        glm::vec3(0.0f, 0.0f, renderYaw), scale);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create creature instance for guid 0x", std::hex, guid, std::dec);
        return;
    }

    // Per-instance hair, skin and head-detail overrides. These run for every
    // NPC, cached model or not, so two NPCs sharing a model still get their own
    // colouring.
    applyHumanoidInstanceOverrides(instanceId, modelId, displayId);
    // A humanoid NPC geoset mask used to be built here, behind
    // `static constexpr bool kEnableNpcSafeGeosetMask = false`. Same story as
    // the block below: disabled, unreachable, and edited tonight as though it
    // were live.

    // The humanoid geoset and equipment overrides that used to sit here are
    // gone. They were three hundred and ninety-five lines behind
    // `static constexpr bool kEnableNpcHumanoidOverrides = false`, and the
    // comment above them said why: too aggressive, made NPCs invisible.
    //
    // Code that cannot run is not a record of an idea, it is a place for
    // mistakes to hide. This block was edited twice tonight - once to stop a
    // geoset filter hiding a body, once to stop a zero facial-hair variant
    // becoming a beard - and neither edit could have done anything. Both were
    // real faults, and both had to be found again in the code that does run.
    //
    // git has it if the idea is wanted back.

    // Character-style NPC models can carry several conflicting clothing meshes
    // at once - a cape and no cape, a robe skirt over trousers. Pick one per
    // clothing group and leave every other batch of the model alone.
    normalizeHumanoidClothingGeosets(instanceId, modelId, displayId);

    // Start the creature in the pose the server says it is already in: dead,
    // mid-emote, or newly arrived.
    playCreatureSpawnPose(guid, instanceId);

    // Track instance
    creatureInstances_[guid] = instanceId;
    creatureAppliedScale_[guid] = scale;
    spawnCreatureParticleTwin(guid, displayId, modelId, instanceId);
    creatureModelIds_[guid] = modelId;
    creatureDisplayIds_[guid] = displayId;
    creatureRenderPosCache_[guid] = renderPos;
    // Already riding (UNIT_FIELD_MOUNTDISPLAYID in the create block, or a
    // model rebuilt under a rider): its mount, as a player's.
    if (gameHandler_) {
        if (auto e = gameHandler_->getEntityManager().getEntity(guid); e && e->isUnit()) {
            const uint32_t mount = static_cast<const game::Unit&>(*e).getMountDisplayId();
            if (mount != 0) setRemotePlayerMountDisplayId(guid, mount);
        }
    }
    LOG_DEBUG("Spawned creature: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

} // namespace core
} // namespace wowee

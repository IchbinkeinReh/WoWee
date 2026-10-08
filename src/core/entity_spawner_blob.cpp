// Which units and corpses the client puts a blob shadow under, and how big
// (0x00743760, 0x00793980, 0x0071ed80, 0x0082ced0).
#include "core/entity_spawner.hpp"

#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"

namespace wowee::core {

namespace {

/// CREATURE_TYPEFLAGS bit 25: the creature cache row's +0xc, which
/// 0x00743760 asks through 0x00715db0 before giving the unit's map entity
/// its blob-shadow flag (0x800, through 0x00781a10).
constexpr uint32_t kTypeFlagNoShadowBlob = 0x02000000;

/// UNIT_STAND_STATE_SUBMERGED: 0x00793980 skips the blob of a unit whose
/// UNIT_FIELD_BYTES_1 stand state (descriptor +0x110) is 9.
constexpr uint8_t kStandStateSubmerged = 9;

}  // namespace

const EntitySpawner::ModelGeoBox* EntitySpawner::geoBoxForDisplay(uint32_t displayId) const {
    auto d = displayDataMap_.find(displayId);
    if (d == displayDataMap_.end()) return nullptr;
    auto g = modelIdToGeoBox_.find(d->second.modelId);
    return g != modelIdToGeoBox_.end() ? &g->second : nullptr;
}

void EntitySpawner::updateBlobShadows(uint32_t localPlayerInstance) {
    rendering::CharacterRenderer* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_) return;
    const auto& entities = gameHandler_->getEntityManager();
    const auto& creatureInfo = gameHandler_->getCreatureInfoCache();

    // A unit with a blob shadow and the box it is sized by: the display's
    // CreatureModelData box, or, mounted, that raised onto the mount's and
    // joined to it (0x0071ed80).
    auto unitBox = [&](const game::Unit& unit, uint32_t displayId,
                       uint32_t mountDisplayId) -> std::optional<rendering::blob_shadow::Box> {
        if (unit.getStandState() == kStandStateSubmerged) return std::nullopt;
        const ModelGeoBox* own = geoBoxForDisplay(displayId);
        if (!own) return std::nullopt;
        if (mountDisplayId != 0) {
            if (const ModelGeoBox* mount = geoBoxForDisplay(mountDisplayId)) {
                return rendering::blob_shadow::mountedBox(own->box, mount->box, mount->mountHeight);
            }
        }
        return own->box;
    };

    for (const auto& [guid, instanceId] : creatureInstances_) {
        std::optional<rendering::blob_shadow::Box> box;
        auto entity = entities.getEntity(guid);
        if (entity && entity->getType() == game::ObjectType::UNIT) {
            const auto& unit = static_cast<const game::Unit&>(*entity);
            bool blob = true;
            if (auto it = creatureInfo.find(unit.getEntry()); it != creatureInfo.end()) {
                blob = rendering::blob_shadow::carriesBlobFlag(rendering::blob_shadow::WorldObjectKind::Unit,
                                                               (it->second.typeFlags & kTypeFlagNoShadowBlob) != 0,
                                                               0);
            }
            if (blob) {
                auto dIt = creatureDisplayIds_.find(guid);
                const uint32_t displayId = dIt != creatureDisplayIds_.end() ? dIt->second
                                                                             : unit.getDisplayId();
                // A creature's mount is not drawn here, so its box is its own.
                box = unitBox(unit, displayId, 0);
            }
        }
        cr->setInstanceBlobShadow(instanceId, box);
    }

    // A player riding is drawn on the mount's instance: the blob goes there,
    // sized by both (the client's unit draws through the mount model).
    auto placePlayer = [&](uint64_t guid, uint32_t riderInstance, uint32_t mountInstance,
                           uint32_t mountDisplayId) {
        std::optional<rendering::blob_shadow::Box> box;
        auto entity = entities.getEntity(guid);
        if (entity && entity->isUnit()) {
            const auto& unit = static_cast<const game::Unit&>(*entity);
            box = unitBox(unit, unit.getDisplayId(), mountInstance != 0 ? mountDisplayId : 0);
        } else if (entity && entity->getType() == game::ObjectType::CORPSE) {
            // A corpse that is not bones carries the flag too (0x00743760),
            // and not being a unit, 0x00793980 sizes its blob by the bounds
            // of the sequence its model plays (0x0082ced0).
            const uint16_t flagsField = game::fieldIndex(game::UF::CORPSE_FIELD_FLAGS);
            const uint32_t flags = flagsField != 0xFFFF ? entity->getField(flagsField) : 0u;
            if (rendering::blob_shadow::carriesBlobFlag(rendering::blob_shadow::WorldObjectKind::Corpse, false,
                                                        flags)) {
                box = cr->instanceSequenceBounds(riderInstance);
            }
        }
        if (mountInstance != 0) {
            cr->setInstanceBlobShadow(mountInstance, box);
            cr->setInstanceBlobShadow(riderInstance, std::nullopt);
        } else {
            cr->setInstanceBlobShadow(riderInstance, box);
        }
    };

    for (const auto& [guid, instanceId] : playerInstances_) {
        const RemotePlayerMount* mount = getRemotePlayerMount(guid);
        placePlayer(guid, instanceId, mount ? mount->instanceId : 0, mount ? mount->displayId : 0);
    }

    const uint64_t self = gameHandler_->getPlayerGuid();
    if (self != 0 && localPlayerInstance != 0) {
        uint32_t mountDisplay = 0;
        if (auto e = entities.getEntity(self); e && e->isUnit()) {
            mountDisplay = static_cast<const game::Unit&>(*e).getMountDisplayId();
        }
        placePlayer(self, localPlayerInstance, mountInstanceId_, mountDisplay);
    }
}

}  // namespace wowee::core

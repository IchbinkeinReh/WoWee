// entity_spawner_object_effect.cpp - an object's effects (ObjectEffect.cpp).
//
// A unit's effects come from its display's ObjectEffectPackage (column 15) -
// the mount's display while it rides one (UNIT_FIELD_MOUNTDISPLAYID,
// 0x00725df0) - and a game object's from GameObjectDisplayInfo's (column 18):
// siege engines, rocket mounts, lifts, gunships (game/object_effect.hpp).
// A unit's states are its movement flags' (0x00721300) and those of the up to
// three animations it plays - the mount's, its body's, its arm's
// (0x0071e5b0, 0x007173f0); a game object's its animation's (0x0070bb10), its
// route's (0x0070b390) and its doodads' events' (0x007bd5a0). Each state's
// effects play once on the way in or out, or loop while any state holds them,
// a loop rising and dying away over 0.7 s (0x006f1ef0, 0x006f61d0) and its
// pitch set by its modifier from the object's answer (0x006f1790).
#include "core/entity_spawner.hpp"

#include "audio/audio_engine.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_event_sounds.hpp"
#include "rendering/renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "game/update_field_table.hpp"
#include "game/transport_manager.hpp"

#include <functional>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee::core {

namespace {

namespace oe = game::object_effect;

constexpr uint32_t kDisplayPackageColumn = 15;   // CreatureDisplayInfo ObjectEffectPackageID
constexpr uint32_t kGameObjectPackageColumn = 18; // GameObjectDisplayInfo ObjectEffectPackageID
constexpr float kLoopFadeSeconds = 0.7f;

std::shared_ptr<const oe::Tables> loadTables(pipeline::AssetManager& assets) {
    auto effects = assets.loadDBCOptional("ObjectEffect.dbc");
    auto elems = assets.loadDBCOptional("ObjectEffectPackageElem.dbc");
    if (!effects || !elems || effects->getFieldCount() < 12 || elems->getFieldCount() < 4) return nullptr;
    auto t = std::make_shared<oe::Tables>();
    for (uint32_t i = 0; i < effects->getRecordCount(); ++i) {
        oe::Effect e;
        e.id = effects->getUInt32(i, 0);
        e.group = effects->getUInt32(i, 2);
        e.trigger = effects->getUInt32(i, 3);
        e.recType = effects->getUInt32(i, 5);
        e.recId = effects->getUInt32(i, 6);
        e.attachment = static_cast<int32_t>(effects->getUInt32(i, 7));
        e.offset = {effects->getFloat(i, 8), effects->getFloat(i, 9), effects->getFloat(i, 10)};
        e.modifier = effects->getUInt32(i, 11);
        t->group[e.group].push_back(e);
    }
    for (uint32_t i = 0; i < elems->getRecordCount(); ++i) {
        // ID, Package, Group, StateType.
        t->package[elems->getUInt32(i, 1)].emplace_back(elems->getUInt32(i, 3), elems->getUInt32(i, 2));
    }
    if (auto mods = assets.loadDBCOptional("ObjectEffectModifier.dbc"); mods && mods->getFieldCount() >= 8) {
        for (uint32_t i = 0; i < mods->getRecordCount(); ++i) {
            oe::Modifier m;
            m.input = mods->getUInt32(i, 1);
            m.map = mods->getUInt32(i, 2);
            m.output = mods->getUInt32(i, 3);
            for (uint32_t k = 0; k < 4; ++k) m.param[k] = mods->getFloat(i, 4 + k);
            t->modifier[mods->getUInt32(i, 0)] = m;
        }
    }
    return t;
}

}  // namespace

void EntitySpawner::clearObjectEffects(uint64_t guid) {
    auto it = objectEffects_.find(guid);
    if (it == objectEffects_.end()) return;
    auto& audio = audio::AudioEngine::instance();
    for (const auto& [effect, loop] : it->second.loops) audio.stopSoundWithFade(loop.first, kLoopFadeSeconds);
    objectEffects_.erase(it);
}

void EntitySpawner::noteDoodadOwner(uint32_t doodadInstance, uint32_t wmoInstance) {
    for (const auto& [guid, info] : gameObjectInstances_) {
        if (info.isWmo && info.instanceId == wmoInstance) {
            doodadOwnerGo_[doodadInstance] = guid;
            return;
        }
    }
}

void EntitySpawner::updateObjectEffects(float deltaTime) {
    (void)deltaTime;
    rendering::CharacterRenderer* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_ || !assetManager_) return;
    if (!objectEffectTablesLoaded_) {
        objectEffectTablesLoaded_ = true;
        objectEffectTables_ = loadTables(*assetManager_);
        if (auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
            displays && displays->getFieldCount() > kDisplayPackageColumn) {
            for (uint32_t i = 0; i < displays->getRecordCount(); ++i) {
                const uint32_t package = displays->getUInt32(i, kDisplayPackageColumn);
                if (package != 0) objectEffectPackageByDisplay_[displays->getUInt32(i, 0)] = package;
            }
        }
        if (auto displays = assetManager_->loadDBCOptional("GameObjectDisplayInfo.dbc");
            displays && displays->getFieldCount() > kGameObjectPackageColumn) {
            for (uint32_t i = 0; i < displays->getRecordCount(); ++i) {
                const uint32_t package = displays->getUInt32(i, kGameObjectPackageColumn);
                if (package != 0) objectEffectPackageByGoDisplay_[displays->getUInt32(i, 0)] = package;
            }
        }
    }
    rendering::M2EventSounds& sounds = renderer_->getM2EventSounds();
    std::vector<rendering::M2EventSounds::ObjectStateEvent> doodadEvents = sounds.takeObjectStateEvents();
    if (!objectEffectTables_) return;
    const oe::Tables& tables = *objectEffectTables_;
    auto& audio = audio::AudioEngine::instance();
    rendering::M2Renderer* m2 = renderer_->getM2Renderer();
    rendering::WMORenderer* wmo = renderer_->getWMORenderer();
    game::TransportManager* transports = gameHandler_->getTransportManager();
    static const oe::States kAnimationScope = oe::animationScope();
    const uint16_t goDisplayField = game::fieldIndex(game::UF::GAMEOBJECT_DISPLAYID);
    const uint64_t self = gameHandler_->getPlayerGuid();
    const uint32_t selfInstance = renderer_->getCharacterInstanceId();

    auto packageOfDisplay = [&](uint32_t display) -> uint32_t {
        const auto pkg = objectEffectPackageByDisplay_.find(display);
        return pkg != objectEffectPackageByDisplay_.end() ? pkg->second : 0;
    };
    // A unit's render instance, and its mount's while it rides one.
    auto unitInstance = [&](uint64_t guid) -> uint32_t {
        if (guid == self) return selfInstance;
        if (auto it = creatureInstances_.find(guid); it != creatureInstances_.end()) return it->second;
        if (auto it = playerInstances_.find(guid); it != playerInstances_.end()) return it->second;
        return 0;
    };
    auto mountInstance = [&](uint64_t guid) -> uint32_t {
        if (guid == self) return mountInstanceId_;
        if (auto it = remotePlayerMounts_.find(guid); it != remotePlayerMounts_.end()) return it->second.instanceId;
        return 0;
    };
    // 0x00725df0: the mount's display while mounted, else the unit's own.
    auto unitPackage = [&](uint64_t guid) -> uint32_t {
        if (unitInstance(guid) == 0) return 0;
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return 0;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        return packageOfDisplay(unit.getMountDisplayId() != 0 ? unit.getMountDisplayId() : unit.getDisplayId());
    };
    auto goPackage = [&](uint64_t guid) -> uint32_t {
        if (!gameObjectInstances_.count(guid) || goDisplayField == 0xFFFF) return 0;
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (!entity) return 0;
        const auto pkg = objectEffectPackageByGoDisplay_.find(entity->getField(goDisplayField));
        return pkg != objectEffectPackageByGoDisplay_.end() ? pkg->second : 0;
    };

    // Objects gone, or whose display no longer names that package, let go;
    // a doodad whose owner is gone forgotten.
    for (auto it = objectEffects_.begin(); it != objectEffects_.end();) {
        const uint64_t guid = it->first;
        const uint32_t now = it->second.gameObject ? goPackage(guid) : unitPackage(guid);
        const uint32_t had = it->second.package;
        ++it;
        if (now != had) clearObjectEffects(guid);
    }
    for (auto it = doodadOwnerGo_.begin(); it != doodadOwnerGo_.end();) {
        if (gameObjectInstances_.count(it->second)) ++it;
        else it = doodadOwnerGo_.erase(it);
    }

    // An object's effects this frame: its sounds played, its loops kept with
    // it and pitched by its modifier from `input` (0x006f1fb0, 0x006f1790).
    using PlaceOf = std::function<glm::vec3(const oe::Effect&)>;
    auto run = [&](ObjectEffectState& st, std::vector<oe::Action>& actions, const PlaceOf& placeOf,
                   std::optional<float> input) {
        auto pitchOf = [&](const oe::Effect& e, float& pitch) {
            if (!input || e.modifier == 0) return false;
            const auto m = tables.modifier.find(e.modifier);
            return m != tables.modifier.end() && oe::modifiedPitch(m->second, *input, pitch);
        };
        for (const oe::Action& a : actions) {
            const oe::Effect& e = *a.effect;
            if (e.recType != oe::kRecSound || e.recId == 0) continue;
            switch (a.kind) {
            case oe::Action::Kind::PlayOnce: {
                const glm::vec3 at = placeOf(e);
                sounds.playOnce(e.recId, &at, 1.0f, assetManager_);
                break;
            }
            case oe::Action::Kind::StartLoop: {
                const uint32_t handle = sounds.playLoop(e.recId, placeOf(e), kLoopFadeSeconds, assetManager_);
                if (handle != 0) st.loops[e.id] = {handle, &e};
                break;
            }
            case oe::Action::Kind::StopLoop: {
                auto it = st.loops.find(e.id);
                if (it != st.loops.end()) {
                    audio.stopSoundWithFade(it->second.first, kLoopFadeSeconds);
                    st.loops.erase(it);
                }
                break;
            }
            }
        }
        for (const auto& [effectId, loop] : st.loops) {
            const auto [handle, e] = loop;
            audio.setSoundPosition(handle, placeOf(*e));
            float pitch = 1.0f;
            if (pitchOf(*e, pitch)) audio.setSoundPitch(handle, pitch);
        }
    };
    // Made in states 1 and 0x26 (0x006f7420).
    auto stateFor = [&](uint64_t guid, uint32_t package, bool gameObject,
                        std::vector<oe::Action>& actions) -> ObjectEffectState& {
        auto [it, made] = objectEffects_.try_emplace(guid);
        ObjectEffectState& st = it->second;
        if (made) {
            st.package = package;
            st.gameObject = gameObject;
            st.runner.enter(tables, package, 1, actions);
            st.runner.enter(tables, package, 0x26, actions);
        }
        return st;
    };

    // Units: creatures, players, the player.
    std::vector<uint64_t> units;
    units.reserve(creatureInstances_.size() + playerInstances_.size() + 1);
    for (const auto& [guid, instance] : creatureInstances_) units.push_back(guid);
    for (const auto& [guid, instance] : playerInstances_) units.push_back(guid);
    if (self != 0 && !playerInstances_.count(self)) units.push_back(self);
    for (const uint64_t guid : units) {
        const uint32_t package = unitPackage(guid);
        if (package == 0) continue;
        const uint32_t instance = unitInstance(guid);
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        const auto& unit = static_cast<const game::Unit&>(*entity);
        const uint32_t mount = unit.getMountDisplayId() != 0 ? mountInstance(guid) : 0;
        // Sounded where the model it rides on is: the mount's under a rider.
        const uint32_t placeInstance = mount != 0 ? mount : instance;
        glm::vec3 position;
        if (!cr->getInstancePosition(placeInstance, position)) continue;
        std::vector<oe::Action> actions;
        ObjectEffectState& st = stateFor(guid, package, false, actions);
        // The up to three animations' states (0x007173f0 -> 0x0071e5b0): the
        // mount's, the body's and an arm's, each by its shown sequence and
        // variation; Death's takes the unit out of every movement state.
        oe::States animWanted;
        bool dead = false;
        auto slotState = [&](const std::optional<rendering::CharacterRenderer::AnimationSlot>& slot) {
            if (!slot) return;
            if (const uint32_t s = oe::animationState(slot->animationId, slot->variation)) {
                animWanted[s] = true;
                dead = dead || oe::endsMovementStates(s);
            }
        };
        if (mount != 0) slotState(cr->animationSlot(mount));
        slotState(cr->animationSlot(instance));
        auto arm = cr->animationSlot(instance, 0);
        if (!arm) arm = cr->animationSlot(instance, 1);
        slotState(arm);
        st.runner.apply(tables, package, animWanted, kAnimationScope, actions);
        // The movement flags' states (0x00721300).
        oe::States moveWanted, moveScope;
        const uint32_t flags = guid == self ? gameHandler_->getMovementInfo().flags : unit.getReportedMoveFlags();
        oe::movementStates(flags, /*airborne=*/false, /*nearGround=*/true, moveWanted, moveScope);
        if (dead) moveWanted.reset();
        st.runner.apply(tables, package, moveWanted, moveScope, actions);
        // Where an effect sounds: its attachment's place on the model, else
        // the model's, moved by its offset (0x006f1d70).
        run(st, actions, [&](const oe::Effect& e) {
            glm::mat4 frame(1.0f);
            const glm::vec3 offset(e.offset[0], e.offset[1], e.offset[2]);
            if (e.attachment >= 0 &&
                cr->getAttachmentTransform(placeInstance, static_cast<uint32_t>(e.attachment), frame))
                return glm::vec3(frame * glm::vec4(offset, 1.0f));
            return position + offset;
        }, oe::kUnitModifierInput);
    }

    // The doodads' events, by whose doodad it is (0x007bd5a0).
    std::unordered_map<uint64_t, std::vector<uint32_t>> doodadStates;
    for (const auto& ev : doodadEvents) {
        if (auto owner = doodadOwnerGo_.find(ev.instanceId); owner != doodadOwnerGo_.end())
            doodadStates[owner->second].push_back(ev.state);
    }

    // Game objects: a lift's, a tram car's, a gunship's.
    for (const auto& [guid, info] : gameObjectInstances_) {
        const uint32_t package = goPackage(guid);
        if (package == 0) continue;
        glm::mat4 model(1.0f);
        if (info.isWmo) {
            const auto m = wmo ? wmo->getInstanceModelMatrix(info.instanceId) : std::nullopt;
            if (!m) continue;
            model = *m;
        } else {
            const auto p = m2 ? m2->getInstancePosition(info.instanceId) : std::nullopt;
            if (!p) continue;
            model = glm::translate(glm::mat4(1.0f), *p);
        }
        const glm::vec3 position(model[3]);
        std::vector<oe::Action> actions;
        ObjectEffectState& st = stateFor(guid, package, true, actions);
        // The animation the model plays, one state at a time (0x0070bb10).
        if (!info.isWmo && m2) {
            oe::States animWanted;
            if (const auto anim = m2->currentAnimationId(info.instanceId))
                if (const uint32_t s = oe::animationState(*anim, 0)) animWanted[s] = true;
            st.runner.apply(tables, package, animWanted, kAnimationScope, actions);
        }
        // Its route's state (0x007134a0 -> 0x0070b390).
        if (transports) {
            if (const auto route = transports->routeState(guid))
                oe::moveState(st.runner, tables, package, oe::routeMoveState(*route), actions);
        }
        // Its doodads' $EWT/$EAC/$EMV/$EDC (0x007bd5a0).
        if (auto ev = doodadStates.find(guid); ev != doodadStates.end()) {
            for (uint32_t state : ev->second) oe::moveState(st.runner, tables, package, state, actions);
        }
        run(st, actions, [&](const oe::Effect& e) {
            const glm::vec3 offset(e.offset[0], e.offset[1], e.offset[2]);
            glm::mat4 frame(1.0f);
            if (!info.isWmo && e.attachment >= 0 &&
                m2->getAttachmentTransform(info.instanceId, static_cast<uint32_t>(e.attachment), frame))
                return glm::vec3(frame * glm::vec4(offset, 1.0f));
            return glm::vec3(model * glm::vec4(offset, 1.0f));
        }, std::nullopt);
    }
}

}  // namespace wowee::core

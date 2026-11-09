// entity_spawner_object_effect.cpp - a unit's object effects (ObjectEffect.cpp).
//
// A unit whose CreatureDisplayInfo names an ObjectEffectPackage (column 15) -
// a siege engine, a demolisher, a flying machine, Mimiron's head - sounds of
// the states it is in (game/object_effect.hpp): its movement flags' states
// (0x00721300) and its animation's (0x0071e5b0), each one's effects played
// once on the way in or out, or looped while any state holds them, a loop's
// pitch following the unit's speed where its modifier says (0x006f1790). A
// loop rises over 0.7 s and dies away over 0.7 s (0x006f1ef0, 0x006f61d0).
#include "core/entity_spawner.hpp"

#include "audio/audio_engine.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_event_sounds.hpp"
#include "rendering/renderer.hpp"

namespace wowee::core {

namespace {

namespace oe = game::object_effect;

constexpr uint32_t kDisplayPackageColumn = 15;   // CreatureDisplayInfo ObjectEffectPackageID
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

void EntitySpawner::updateObjectEffects(float deltaTime) {
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
    }
    if (!objectEffectTables_ || objectEffectPackageByDisplay_.empty()) return;
    const oe::Tables& tables = *objectEffectTables_;
    auto& audio = audio::AudioEngine::instance();
    rendering::M2EventSounds& sounds = renderer_->getM2EventSounds();
    static const oe::States kAnimationScope = oe::animationScope();

    // Units gone, or whose display no longer has a package, let go.
    for (auto it = objectEffects_.begin(); it != objectEffects_.end();) {
        const auto display = creatureDisplayIds_.find(it->first);
        const bool keep = creatureInstances_.count(it->first) && display != creatureDisplayIds_.end() &&
                          objectEffectPackageByDisplay_.count(display->second) &&
                          objectEffectPackageByDisplay_.at(display->second) == it->second.package;
        if (keep) { ++it; continue; }
        const uint64_t guid = it->first;
        ++it;
        clearObjectEffects(guid);
    }

    const uint64_t self = gameHandler_->getPlayerGuid();
    for (const auto& [guid, instance] : creatureInstances_) {
        const auto display = creatureDisplayIds_.find(guid);
        if (display == creatureDisplayIds_.end()) continue;
        const auto pkg = objectEffectPackageByDisplay_.find(display->second);
        if (pkg == objectEffectPackageByDisplay_.end()) continue;
        auto entity = gameHandler_->getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) continue;
        const auto& unit = static_cast<const game::Unit&>(*entity);
        glm::vec3 position;
        if (!cr->getInstancePosition(instance, position)) continue;

        ObjectEffectState& st = objectEffects_[guid];
        st.package = pkg->second;
        if (st.hasLastPosition && deltaTime > 0.0f) {
            st.speed = glm::length(position - st.lastPosition) / deltaTime;
        }
        st.lastPosition = position;
        st.hasLastPosition = true;

        std::vector<oe::Action> actions;
        // The animation's state (0x0071e5b0), and Death's end of every
        // movement state.
        oe::States animWanted;
        uint32_t anim = 0;
        float animTime = 0.0f, animDuration = 0.0f;
        bool dead = false;
        if (cr->getAnimationState(instance, anim, animTime, animDuration)) {
            if (const uint32_t s = oe::animationState(anim)) {
                animWanted[s] = true;
                dead = oe::endsMovementStates(s);
            }
        }
        st.runner.apply(tables, st.package, animWanted, kAnimationScope, actions);
        // The movement flags' states (0x00721300).
        oe::States moveWanted, moveScope;
        const uint32_t flags = guid == self ? gameHandler_->getMovementInfo().flags : unit.getReportedMoveFlags();
        oe::movementStates(flags, /*airborne=*/false, /*nearGround=*/true, moveWanted, moveScope);
        if (dead) moveWanted.reset();
        st.runner.apply(tables, st.package, moveWanted, moveScope, actions);

        // Where an effect sounds: its attachment's place on the model, else
        // the model's, moved by its offset (0x006f1d70).
        auto placeOf = [&](const oe::Effect& e) {
            glm::mat4 frame(1.0f);
            const glm::vec3 offset(e.offset[0], e.offset[1], e.offset[2]);
            if (e.attachment >= 0 && cr->getAttachmentTransform(instance, static_cast<uint32_t>(e.attachment), frame))
                return glm::vec3(frame * glm::vec4(offset, 1.0f));
            return position + offset;
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
        // The loops follow the unit, and their pitch its speed (0x006f1fb0).
        for (const auto& [effectId, loop] : st.loops) {
            const auto [handle, e] = loop;
            audio.setSoundPosition(handle, placeOf(*e));
            if (e->modifier == 0) continue;
            const auto m = tables.modifier.find(e->modifier);
            float pitch = 1.0f;
            if (m != tables.modifier.end() && oe::modifiedPitch(m->second, st.speed, pitch))
                audio.setSoundPitch(handle, pitch);
        }
    }
}

}  // namespace wowee::core

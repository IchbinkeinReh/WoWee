#include "rendering/levelup_effect.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

LevelUpEffect::LevelUpEffect() = default;
LevelUpEffect::~LevelUpEffect() = default;

bool LevelUpEffect::loadModel(M2Renderer* m2Renderer,
                               const std::vector<uint8_t>& m2FileData,
                               const std::vector<uint8_t>& skinFileData) {
    if (!m2Renderer || m2FileData.empty()) return false;

    m2Renderer_ = m2Renderer;

    pipeline::M2Model model = pipeline::M2Loader::load(m2FileData);
    if (model.name.empty()) model.name = "Spells\\LevelUp.m2";
    // Spell effect M2s may have no geometry (particle-only), so don't require isValid()
    if (model.vertices.empty() && model.particleEmitters.empty()) {
        LOG_WARNING("LevelUpEffect: M2 has no vertices and no particle emitters");
        return false;
    }

    if (!skinFileData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinFileData, model);
    }

    if (!m2Renderer_->loadModel(model, MODEL_ID)) {
        LOG_WARNING("LevelUpEffect: failed to load model to GPU");
        return false;
    }

    eventsSeconds_ = model.sequences.empty() ? 0.0f
                                             : static_cast<float>(model.sequences[0].duration) / 1000.0f;
    modelLoaded_ = true;
    LOG_INFO("LevelUpEffect: loaded LevelUp.m2 - vertices=", model.vertices.size(),
             " indices=", model.indices.size(),
             " emitters=", model.particleEmitters.size(),
             " batches=", model.batches.size());
    return true;
}

void LevelUpEffect::trigger(const glm::vec3& position, uint32_t unitInstanceId) {
    if (!modelLoaded_ || !m2Renderer_) return;

    uint32_t instanceId = m2Renderer_->createInstance(MODEL_ID, position,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("LevelUpEffect: failed to create instance");
        return;
    }

    // From its first frame, as the client starts it (0x006f7680 sets the
    // sequence going once the model is in), not at the random point a placed
    // doodad starts at: the pillar rises from the ground, and the $SND at
    // 33 ms sounds as the level is gained rather than up to a lap later.
    m2Renderer_->restartInstanceAnimation(instanceId);
    // A spell effect on the unit (0x006f9260 -> 0x00824060 with 0x006f7b00):
    // its $SND plays, at the listener when the unit is the active mover's.
    m2Renderer_->setInstanceEventOwner(instanceId, m2_event::Owner::Effect, unitInstanceId);

    activeEffects_.push_back({.instanceId = instanceId, .elapsed = 0.0f, .eventsOwned = true});
}

void LevelUpEffect::update(float deltaTime) {
    if (activeEffects_.empty() || !m2Renderer_) return;

    for (auto it = activeEffects_.begin(); it != activeEffects_.end(); ) {
        it->elapsed += deltaTime;
        // Updated ahead of the models, so the pass's end is caught before the
        // sequence comes round again.
        if (it->eventsOwned && it->elapsed >= eventsSeconds_) {
            m2Renderer_->setInstanceEventOwner(it->instanceId, m2_event::Owner::None, 0);
            it->eventsOwned = false;
        }
        if (it->elapsed >= EFFECT_DURATION) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeEffects_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rendering
} // namespace wowee

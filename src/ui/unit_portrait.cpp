#include "ui/unit_portrait.hpp"

#include "core/logger.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/portrait_camera.hpp"
#include "rendering/renderer.hpp"

#include <utility>

namespace wowee::ui {

UnitPortrait::UnitPortrait() = default;

UnitPortrait::~UnitPortrait() = default;

bool UnitPortrait::show(const UnitLook& look, pipeline::AssetManager* assets, rendering::Renderer* renderer,
                        float deltaTime) {
    if (!assets || !renderer || !look.any()) return false;

    if (!preview_) {
        preview_ = std::make_unique<rendering::CharacterPreview>();
        // Small: this is drawn into a circle a few dozen pixels across, and the
        // cost of the pass scales with the target.
        if (!preview_->initialize(assets, targetWidth_, targetHeight_)) {
            LOG_WARNING("UnitPortrait: could not build the offscreen view");
            preview_.reset();
            return false;
        }
        builtWidth_ = targetWidth_;
        builtHeight_ = targetHeight_;
        renderer->registerPreview(preview_.get());
        registered_ = true;
        loaded_ = {};
    }

    if (look != loaded_) {
        beginRebuild();
        loadedWell_ = load(look);
        loaded_ = look;
        turnLeft_ = 0.0f;
        turning_ = 0;
        // Logged because a portrait that rebuilds every frame looks like one
        // that flickers, and the two are indistinguishable from outside.
        if (look.character) {
            LOG_INFO("UnitPortrait: rebuilt for race ", static_cast<int>(look.race), " appearance ", look.appearance);
        } else {
            LOG_INFO("UnitPortrait: rebuilt for creature ", look.model);
        }
    }
    if (!loadedWell_) return false;

    present(deltaTime);
    return preview_->isModelLoaded();
}

bool UnitPortrait::load(const UnitLook& look) {
    // Declared before the model loads, so the racial backdrop is never built
    // in the first place.
    preview_->setTransparentBackground(true, framing_ == Framing::Face);
    if (look.character) {
        preview_->setCharacterClass(look.classId);
        preview_->setTabardDesigner(look.tabardDesigner);
        if (!preview_->loadCharacter(static_cast<game::Race>(look.race), static_cast<game::Gender>(look.gender),
                                     static_cast<uint8_t>(look.appearance & 0xFF),
                                     static_cast<uint8_t>((look.appearance >> 8) & 0xFF),
                                     static_cast<uint8_t>((look.appearance >> 16) & 0xFF),
                                     static_cast<uint8_t>((look.appearance >> 24) & 0xFF),
                                     look.facialFeatures, look.femaleModel))
            return false;
        // After the model, because applyEquipment reads its geosets, and only
        // where there is something to apply.
        preview_->setGuildEmblem(look.emblem);
        // An NPC's bake makes the component a baked one (0x00598xxx sets
        // +0x3c bit 1 with Textures\BakedNpcTextures\): its items then set
        // geosets, models and the cape, and paint no layers on the skin
        // (0x004f2640); the bake already wears them.
        const bool baked = !look.bakedSkin.empty() && preview_->setBakedSkin(look.bakedSkin);
        if (!look.worn.empty()) preview_->applyEquipment(look.worn, !baked);
    } else if (!preview_->loadCreature(look.model, look.skins)) {
        return false;
    }
    // After the model, because its cameras and bounds are what the framing is
    // made from.
    frame();
    return true;
}

void UnitPortrait::frame() {
    if (framing_ == Framing::Face) {
        preview_->setPortraitFraming();
    } else {
        preview_->setFigureFraming(camera_, pose_);
    }
}

void UnitPortrait::setFigure(int camera, const rendering::figure_camera::Pose& pose) {
    if (camera_ == camera && pose_ == pose) return;
    camera_ = camera;
    pose_ = pose;
    if (preview_ && loadedWell_ && framing_ == Framing::Figure) frame();
}

void UnitPortrait::setFigureLight(const rendering::model_frame::Light& light, const rendering::model_frame::Fog& fog) {
    if (preview_) preview_->setFigureLight(light, fog);
}

void UnitPortrait::setTurning(int direction, uint32_t serial) {
    if (serial == turnSerial_) return;
    turnSerial_ = serial;
    if (!preview_ || !loadedWell_) return;
    constexpr float kTurnSeconds = 0.1f;
    if (direction != turning_) {
        turning_ = direction;
        preview_->playModelAnimation(direction < 0   ? rendering::anim::SHUFFLE_LEFT
                                     : direction > 0 ? rendering::anim::SHUFFLE_RIGHT
                                                     : rendering::anim::STAND,
                                     true);
    }
    turnLeft_ = kTurnSeconds;
}

void UnitPortrait::setSequence(int animation, uint32_t serial) {
    if (serial == sequenceSerial_) return;
    sequenceSerial_ = serial;
    if (animation < 0 || !preview_ || !loadedWell_) return;
    preview_->playModelAnimation(static_cast<uint32_t>(animation), true);
    turnLeft_ = 0.0f;
    turning_ = 0;
}

void UnitPortrait::fitTo(int width, int height, rendering::Renderer* renderer) {
    setTargetSize(width, height);
    if (preview_ && (builtWidth_ != width || builtHeight_ != height)) shutdown(renderer);
}

void UnitPortrait::beginRebuild() {
    // The framing applied after the load reads this.
    preview_->setPortraitHeadroom(animated_ ? rendering::portrait_camera::kAnimatedHeadroom
                                            : rendering::portrait_camera::kStillHeadroom);
    framedAnimated_ = animated_;
    stillFramesLeft_ = rendering::portrait_camera::kStillFrames;
}

void UnitPortrait::present(float deltaTime) {
    if (animated_ != framedAnimated_) {
        // Switched between a still and a live one: framed again, and drawn
        // again either way.
        preview_->setPortraitHeadroom(animated_ ? rendering::portrait_camera::kAnimatedHeadroom
                                                : rendering::portrait_camera::kStillHeadroom);
        if (framing_ == Framing::Face) preview_->setPortraitFraming();
        framedAnimated_ = animated_;
        stillFramesLeft_ = rendering::portrait_camera::kStillFrames;
    }
    if (turnLeft_ > 0.0f && (turnLeft_ -= deltaTime) <= 0.0f && std::exchange(turning_, 0) != 0)
        preview_->playModelAnimation(rendering::anim::STAND, true);   // 0x005971b0
    if (animated_) {
        preview_->update(deltaTime);
        preview_->render();
        preview_->requestComposite();
        return;
    }
    // A still picture (0x00619580): drawn when the model is built, posed at
    // the start of its stand, then left as it is.
    if (stillFramesLeft_ <= 0) return;
    --stillFramesLeft_;
    preview_->update(0.0f);
    preview_->render();
    preview_->requestComposite();
}

uint64_t UnitPortrait::textureId() const {
    if (!preview_) return 0;
    return reinterpret_cast<uint64_t>(preview_->getTextureId());
}

void UnitPortrait::shutdown(rendering::Renderer* renderer) {
    if (preview_ && registered_ && renderer) renderer->unregisterPreview(preview_.get());
    registered_ = false;
    preview_.reset();
    // What the gone model was built from. A portrait used again afterwards -
    // after a lost device is rebuilt - makes a new view, and has to load the
    // model into it rather than take it as already there.
    loaded_ = {};
    loadedWell_ = false;
}

} // namespace wowee::ui

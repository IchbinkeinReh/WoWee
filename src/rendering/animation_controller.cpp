#include <span>

#include "rendering/animation_controller.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/mount_motion.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/inventory.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/music_manager.hpp"
#include "rendering/swim_effects.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <set>
#include <random>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// ── AnimationController implementation ───────────────────────────────────────

AnimationController::AnimationController() = default;
AnimationController::~AnimationController() = default;

void AnimationController::initialize(Renderer* renderer) {
    renderer_ = renderer;
}

// ── Emote support ────────────────────────────────────────────────────────────

// An emote the player does: its Emotes row's animation, asked as any
// animation of the unit's is (0x007385c0) - looped for a state emote, once
// for the rest, which ends into the unit's base animation (0x0073b510).
void AnimationController::playEmote(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    auto result = registry.findEmote(emoteName);
    if (!result) return;

    auto* characterRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t characterInstanceId = renderer_ ? renderer_->getCharacterInstanceId() : 0;
    if (!characterRenderer || characterInstanceId == 0) return;
    // What an emote resolved to, and whether the model can play it. An emote
    // that does nothing is either a name that resolved to no animation or an
    // animation the model does not carry, and from outside those look the same.
    LOG_WARNING("Emote '", emoteName, "' -> animation ", result->animId,
                (result->loop ? " (looping)" : " (one shot)"),
                " model has it: ", (characterRenderer->hasAnimation(characterInstanceId, result->animId) ? "yes" : "NO"));
    characterRenderer->playAnimation(characterInstanceId, result->animId, result->loop);
}

std::string AnimationController::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textFor(emoteName, targetName);
}

uint32_t AnimationController::getEmoteDbcId(const std::string& emoteName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.dbcIdFor(emoteName);
}

std::string AnimationController::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                                      const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.textByDbcId(dbcId, senderName, targetName);
}
std::string AnimationController::getSelfEmoteTextByDbcId(uint32_t dbcId,
                                                          const std::string* targetName) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.selfTextByDbcId(dbcId, targetName);
}

uint32_t AnimationController::getEmoteAnimByEmotesId(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.animByEmotesId(emoteId);
}

uint32_t AnimationController::getEmoteFlagsByEmotesId(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.flagsByEmotesId(emoteId);
}

bool AnimationController::isStateEmoteById(uint32_t emoteId) {
    auto& registry = EmoteRegistry::instance();
    registry.loadFromDbc();
    return registry.isStateEmote(emoteId);
}

void AnimationController::resetCombatVisualState() {
    if (auto* svs = renderer_->getSpellVisualSystem()) svs->reset();
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position, uint32_t unitInstanceId) {
    auto* levelUpEffect = renderer_->getLevelUpEffect();
    if (!levelUpEffect) return;

    if (!levelUpEffect->isModelLoaded()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (!assetManager) {
                LOG_WARNING("LevelUpEffect: no asset manager available");
            } else {
                auto m2Data = assetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
                auto skinData = assetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
                LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
                if (!m2Data.empty()) {
                    levelUpEffect->loadModel(m2Renderer, m2Data, skinData);
                } else {
                    LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
                }
            }
        }
    }

    levelUpEffect->trigger(position, unitInstanceId);
}

// ── Mount ────────────────────────────────────────────────────────────────────

void AnimationController::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();

    mountInstanceId_ = mountInstId;
    mountHeightOffset_ = heightOffset;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = renderer_->getCharacterPosition();
    mountSeatSmoothingInit_ = false;
    mountPitch_ = 0.0f;

    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    if (characterRenderer && mountInstId > 0) characterRenderer->dumpAnimations(mountInstId);

    LOG_INFO("Mount configured: displayId=", mountDisplayId, " taxiFlight=", taxiFlight_);
    mountMotion_.reset();

    footstepDriver_.setMountModel(modelPath);
}

void AnimationController::clearMount() {
    LOG_INFO("Mount cleared: was taxiFlight=", taxiFlight_, " mountInstanceId=", mountInstanceId_);
    mountInstanceId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = glm::vec3(0.0f);
    mountSeatSmoothingInit_ = false;

    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
        cameraController->setMountPivot(std::nullopt);
    }
}

// ── Mount positioning helper ─────────────────────────────────────────────────

// A seat that shifts more than this between frames is being carried, not
// jittering: standing still on solid ground moves it by a fraction of this,
// while a ferry at speed moves it by several times as much in a single frame.
static constexpr float kSeatCarriedStepSq = 0.05f * 0.05f;

void AnimationController::applyMountPositioning(float mountBob, float mountRoll, float characterYaw) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;

    float mountYawRad = glm::radians(characterYaw);

    // Position mount model
    if (mountInstanceId_ > 0) {
        const glm::vec3& characterPosition = renderer_->getCharacterPosition();
        characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);
        // (roll, pitch, yaw) as the renderer takes them: a model facing +X
        // noses up about Y the negative way, and banks about X.
        characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountRoll, -mountPitch_, mountYawRad));
    }

    // Use mount's attachment point for proper bone-driven rider positioning.
    if (taxiFlight_) {
        glm::mat4 mountSeatTransform(1.0f);
        bool haveSeat = false;
        static constexpr uint32_t kTaxiSeatAttachmentId = 0;
        if (mountSeatAttachmentId_ == -1) {
            mountSeatAttachmentId_ = static_cast<int>(kTaxiSeatAttachmentId);
        }
        if (mountSeatAttachmentId_ >= 0) {
            haveSeat = characterRenderer->getAttachmentTransform(
                mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
        }
        // A failed lookup is "not yet", not "never". The seat is read off the
        // mount's model, and a mount summoned before its M2 has finished
        // loading answers no on the first frame or two - latching that closed
        // left the rider on the guessed height for the whole ride.
        if (haveSeat) {
            glm::vec3 targetRiderPos = glm::vec3(mountSeatTransform[3]) + glm::vec3(0.0f, 0.0f, 0.02f);
            mountSeatSmoothingInit_ = false;
            smoothedMountSeatPos_ = targetRiderPos;
            characterRenderer->setInstancePosition(characterInstanceId, targetRiderPos);
        } else {
            mountSeatSmoothingInit_ = false;
            const glm::vec3& characterPosition = renderer_->getCharacterPosition();
            glm::vec3 playerPos = characterPosition + glm::vec3(0.0f, 0.0f, mountHeightOffset_ + 0.10f);
            characterRenderer->setInstancePosition(characterInstanceId, playerPos);
        }

        float riderPitch = mountPitch_ * 0.35f;
        float riderRoll = mountRoll * 0.35f;
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderRoll, -riderPitch, mountYawRad));
        return;
    }

    // Ground mounts: try a seat attachment first.
    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
    bool moving = renderer_->getCameraController() && renderer_->getCameraController()->isMoving();

    glm::mat4 mountSeatTransform;
    bool haveSeat = false;
    if (mountSeatAttachmentId_ >= 0) {
        haveSeat = characterRenderer->getAttachmentTransform(
            mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
    } else if (mountSeatAttachmentId_ == -1) {
        static constexpr uint32_t kSeatAttachments[] = {0, 5, 6, 7, 8};
        for (uint32_t attId : kSeatAttachments) {
            if (characterRenderer->getAttachmentTransform(mountInstanceId_, attId, mountSeatTransform)) {
                mountSeatAttachmentId_ = static_cast<int>(attId);
                haveSeat = true;
                break;
            }
        }
        // Left at -1 on failure, so the next frame probes again - see the note
        // in the taxi branch above.
    }

    if (haveSeat) {
        glm::vec3 mountSeatPos = glm::vec3(mountSeatTransform[3]);
        glm::vec3 seatOffset = glm::vec3(0.0f, 0.0f, taxiFlight_ ? 0.04f : 0.08f);
        glm::vec3 targetRiderPos = mountSeatPos + seatOffset;

        // The smoothing below is for a rider sitting still, where it damps the
        // jitter of a bone-driven seat. `moving` only reports movement *input*,
        // though, and a player standing on a boat presses nothing while the world
        // carries them at the ship's speed. The filter then trailed the rider
        // behind the seat by its own time constant - about two yards at ferry
        // speed, swinging out to one side as the hull turned. Reported as the
        // character riding behind the direction of travel, alongside its mount.
        //
        // So ask whether the seat is actually moving in the world rather than
        // whether the player asked for it to. This covers anything that carries a
        // rider - ships, elevators, a moving platform - without naming any of them.
        const glm::vec3 seatStep = targetRiderPos - lastMountSeatTarget_;
        const bool seatCarried = mountSeatSmoothingInit_ &&
                                 glm::dot(seatStep, seatStep) > kSeatCarriedStepSq;
        lastMountSeatTarget_ = targetRiderPos;

        if (moving || seatCarried) {
            mountSeatSmoothingInit_ = false;
            smoothedMountSeatPos_ = targetRiderPos;
        } else if (!mountSeatSmoothingInit_) {
            smoothedMountSeatPos_ = targetRiderPos;
            mountSeatSmoothingInit_ = true;
        } else {
            float smoothHz = taxiFlight_ ? 10.0f : 14.0f;
            float alpha = 1.0f - std::exp(-smoothHz * std::max(lastDeltaTime_, 0.001f));
            smoothedMountSeatPos_ = glm::mix(smoothedMountSeatPos_, targetRiderPos, alpha);
        }

        characterRenderer->setInstancePosition(characterInstanceId, smoothedMountSeatPos_);

        // It hangs on the mount (0x00831630, attachment 0): pitched and
        // rolled as that is.
        float yawRad = glm::radians(characterYaw);
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountRoll, -mountPitch_, yawRad));
    } else {
        mountSeatSmoothingInit_ = false;
        float yawRad = glm::radians(characterYaw);
        glm::mat4 mountRotation = glm::mat4(1.0f);
        mountRotation = glm::rotate(mountRotation, yawRad, glm::vec3(0.0f, 0.0f, 1.0f));
        mountRotation = glm::rotate(mountRotation, -mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
        mountRotation = glm::rotate(mountRotation, mountRoll, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
        glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
        glm::vec3 playerPos = characterPosition + worldOffset;
        characterRenderer->setInstancePosition(characterInstanceId, playerPos);
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountRoll, -mountPitch_, yawRad));
    }
}

// ── The mount each frame ────────────────────────────────────────────────────

// What the mount plays is its rider's unit's (CharacterRenderer::
// setRiderMount, rider_mount.hpp); here it goes where the player is, banks
// in flight and carries its rider.
void AnimationController::updateMount(float deltaTime) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    // The body's yaw rather than the facing, so mount and rider turn after
    // the player the way a unit on foot does. A taxi has no body lag.
    float characterYaw = (hasMountBodyYaw_ && !taxiFlight_) ? mountBodyYawDeg_
                                                            : renderer_->getCharacterYaw();
    hasMountBodyYaw_ = false;

    MountMotion::Input in;
    in.moving = cameraController->isMoving();
    in.forwardOrBack = cameraController->isMovingForward() || cameraController->isMovingBackward();
    // In the air, not allowed to be: on the ground a flying mount runs.
    in.flying = cameraController->isFlightAirborne();
    in.taxiFlight = taxiFlight_;
    in.deltaTime = deltaTime;
    in.characterYaw = characterYaw;
    uint32_t mountAnim = 0;
    if (mountInstanceId_ > 0 && characterRenderer) {
        in.haveAnimation = characterRenderer->getAnimationState(mountInstanceId_, mountAnim, in.animationTime,
                                                                in.animationDuration);
    }
    const MountMotion::Output out = mountMotion_.update(in);

    // The mount's stride follows the speed the player moves at, the rider's
    // does not move (FUN_007385c0 scales only movement animations).
    if (characterRenderer) {
        if (mountInstanceId_ > 0) {
            characterRenderer->setLocomotionSpeed(mountInstanceId_,
                                                  cameraController->getLocomotionSpeed());
        }
        characterRenderer->setLocomotionSpeed(characterInstanceId, 0.0f);
    }

    // The rider holds its pose (+0xb7c: Mount, or a mount aura kit's,
    // 0x0073d5d0), in the air as on the ground: 3.3.5 has no flight poses
    // for a rider.
    uint32_t riderAnim = anim::MOUNT;
    if (auto* svs = renderer_->getSpellVisualSystem()) {
        const uint32_t pose = svs->activePlayerRiderPose();
        if (characterRenderer->hasAnimation(characterInstanceId, pose)) riderAnim = pose;
    }

    // Apply rider animation
    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    if (!haveState || currentAnimId != riderAnim) {
        characterRenderer->playAnimation(characterInstanceId, riderAnim, true);
    }

    // On a taxi the roll is the route's (setMountPitchRoll).
    applyMountPositioning(out.bob, taxiFlight_ ? mountRoll_ : out.roll, characterYaw);
}

// ── The player's animation ───────────────────────────────────────────────────

// The player is animated as any unit is: its base animation asked on each
// event of its movement (Application, 0x0073ed10), its swings, emotes,
// kits and wounds as they come, and what follows a one-shot by the
// CharacterRenderer (0x0073b510). Each frame leaves only the speed its
// movement animation plays at (0x007385c0), and where its mount goes.
void AnimationController::updateCharacterAnimation() {
    if (isMounted()) {
        updateMount(lastDeltaTime_);
        return;
    }
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    const uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (characterRenderer && cameraController && characterInstanceId > 0) {
        characterRenderer->setLocomotionSpeed(characterInstanceId, cameraController->getLocomotionSpeed());
    }
}

// ── Footstep update (delegated to FootstepDriver) ───────────────────────────

void AnimationController::updateFootsteps(float deltaTime) {
    footstepDriver_.update(deltaTime, renderer_, isMounted(), mountInstanceId_,
                            taxiFlight_,
                            renderer_->getCameraController() && renderer_->getCameraController()->isMoving());
}

} // namespace rendering
} // namespace wowee

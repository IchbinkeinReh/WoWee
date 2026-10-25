#include <span>

#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/mount_fsm.hpp"
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
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/movement_sound_manager.hpp"
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

// ── Melee combat ─────────────────────────────────────────────────────────────

// The player's swing (0x00756800): the attack for its weapons, asked as any
// animation is (0x007385c0) - on the upper body while it moves, and waiting
// behind a swing still playing (+0xb88).
void AnimationController::triggerMeleeSwing() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (const uint32_t swing = resolveMeleeAnimId(); swing != 0) {
        characterRenderer->playAnimation(characterInstanceId, swing, false);
    }
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

void AnimationController::setEquippedWeaponType(uint32_t inventoryType, bool is2HLoose,
                                                 bool isFist, bool isDagger,
                                                 bool hasOffHand, bool hasShield,
                                                 bool offHandIsFist,
                                                 bool offHandIsDagger) {
    weaponLoadout_.inventoryType = inventoryType;
    weaponLoadout_.is2HLoose = is2HLoose;
    weaponLoadout_.isFist = isFist;
    weaponLoadout_.isDagger = isDagger;
    weaponLoadout_.hasOffHand = hasOffHand;
    weaponLoadout_.offHandIsFist = offHandIsFist;
    weaponLoadout_.offHandIsDagger = offHandIsDagger;
    weaponLoadout_.hasShield = hasShield;
    meleeAnimId_ = 0;  // Force re-resolve on next swing
}

void AnimationController::triggerSpecialAttack(uint32_t /*spellId*/) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Choose special attack animation based on equipped weapon type
    uint32_t specAnim = 0;
    if (weaponLoadout_.hasShield && has(anim::SHIELD_BASH)) {
        specAnim = anim::SHIELD_BASH;
    } else if ((weaponLoadout_.inventoryType == game::InvType::TWO_HAND || weaponLoadout_.is2HLoose) && has(anim::SPECIAL_2H)) {
        specAnim = anim::SPECIAL_2H;
    } else if (weaponLoadout_.inventoryType != game::InvType::NON_EQUIP && has(anim::SPECIAL_1H)) {
        specAnim = anim::SPECIAL_1H;
    } else if (has(anim::SPECIAL_UNARMED)) {
        specAnim = anim::SPECIAL_UNARMED;
    } else if (has(anim::SPECIAL_1H)) {
        specAnim = anim::SPECIAL_1H;
    }

    if (specAnim == 0) {
        // No special animation available - fall back to regular melee swing
        triggerMeleeSwing();
        return;
    }

    characterRenderer->playAnimation(characterInstanceId, specAnim, false);
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

// ── Ranged combat ────────────────────────────────────────────────────────────

void AnimationController::setEquippedRangedType(RangedWeaponType type) {
    weaponLoadout_.rangedType = type;
}

void AnimationController::triggerRangedShot() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Resolve ranged attack animation based on weapon type
    uint32_t shootAnim = 0;
    switch (weaponLoadout_.rangedType) {
        case RangedWeaponType::BOW:
            if (has(anim::FIRE_BOW))        shootAnim = anim::FIRE_BOW;
            else if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::GUN:
            if (has(anim::ATTACK_RIFLE))    shootAnim = anim::ATTACK_RIFLE;
            break;
        case RangedWeaponType::CROSSBOW:
            if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::THROWN:
            if (has(anim::ATTACK_THROWN))    shootAnim = anim::ATTACK_THROWN;
            break;
        case RangedWeaponType::WAND: {
            // WOWEE_WAND_ANIM=<id> overrides the shot animation, for comparing
            // candidates on screen: 32 cast, 49 rifle, 53 directed, 54 omni.
            static const uint32_t kForced = []() -> uint32_t {
                const char* v = std::getenv("WOWEE_WAND_ANIM");
                return v ? static_cast<uint32_t>(std::atoi(v)) : 0;
            }();
            if (kForced != 0 && has(kForced))        shootAnim = kForced;
            else if (has(anim::SPELL_CAST_DIRECTED)) shootAnim = anim::SPELL_CAST_DIRECTED;
            else if (has(anim::ATTACK_RIFLE))        shootAnim = anim::ATTACK_RIFLE;
            break;
        }
        default: break;
    }
    if (shootAnim == 0) return;  // Model has no ranged animation

    characterRenderer->playAnimation(characterInstanceId, shootAnim, false);
}

uint32_t AnimationController::resolveMeleeAnimId() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId_ = 0;
        return 0;
    }

    // When dual-wielding, bypass cache to alternate main/off-hand animations
    if (!weaponLoadout_.hasOffHand && meleeAnimId_ != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId_)) {
        return meleeAnimId_;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId_ = 0;
        return 0;
    }

    std::span<const uint32_t> chain;
    // The chains live in melee_anim_chains.hpp, which the capability probe
    // reads too - it used to keep its own copies under a comment saying they
    // matched these.
    using anim::MeleeChain;

    // Dual-wield: alternate main-hand and off-hand swings
    bool useOffHand = weaponLoadout_.hasOffHand && meleeOffHandTurn_;
    meleeOffHandTurn_ = weaponLoadout_.hasOffHand ? !meleeOffHandTurn_ : false;

    if (useOffHand) {
        // The off-hand weapon's kind, not the main hand's.
        if (weaponLoadout_.offHandIsFist) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandFist);
        } else if (weaponLoadout_.offHandIsDagger) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandPierce);
        } else if (weaponLoadout_.inventoryType == game::InvType::NON_EQUIP) {
            chain = anim::meleeAnimChain(MeleeChain::OffHandUnarmed);
        } else {
            chain = anim::meleeAnimChain(MeleeChain::OffHand);
        }
    } else if (weaponLoadout_.isFist) {
        chain = anim::meleeAnimChain(MeleeChain::Fist);
    } else if (weaponLoadout_.isDagger) {
        chain = anim::meleeAnimChain(MeleeChain::Dagger);
    } else if (weaponLoadout_.is2HLoose) {
        // Polearm thrust uses pierce variant
        chain = anim::meleeAnimChain(MeleeChain::TwoHandLoose);
    } else if (weaponLoadout_.inventoryType == game::InvType::TWO_HAND) {
        chain = anim::meleeAnimChain(MeleeChain::TwoHand);
    } else if (weaponLoadout_.inventoryType == game::InvType::NON_EQUIP) {
        chain = anim::meleeAnimChain(MeleeChain::Unarmed);
    } else {
        chain = anim::meleeAnimChain(MeleeChain::OneHand);
    }
    for (uint32_t id : chain) {
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId_ = id;
            return meleeAnimId_;
        }
    }

    const uint32_t avoidIds[] = {anim::STAND, anim::DEATH, anim::WALK, anim::RUN, anim::SHUFFLE_LEFT, anim::SHUFFLE_RIGHT, anim::WALK_BACKWARDS, anim::JUMP_START, anim::JUMP, anim::JUMP_END, anim::SWIM_IDLE, anim::SWIM, anim::SITTING};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId_ = bestId;
    return meleeAnimId_;
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position) {
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

    levelUpEffect->trigger(position);
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

    if (characterRenderer && mountInstId > 0) {
        characterRenderer->dumpAnimations(mountInstId);
    }

    // Discover mount animation capabilities (property-based, not hardcoded IDs)
    LOG_DEBUG("=== Mount Animation Dump (Display ID ", mountDisplayId, ") ===");
    if (characterRenderer) characterRenderer->dumpAnimations(mountInstId);

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer || !characterRenderer->getAnimationSequences(mountInstId, sequences)) {
        LOG_WARNING("Failed to get animation sequences for mount, using fallback IDs");
        sequences.clear();
    }

    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer && characterRenderer->hasAnimation(mountInstId, id)) {
                return id;
            }
        }
        return 0;
    };

    // Property-based jump animation discovery with chain-based scoring
    auto discoverJumpSet = [&]() {
        LOG_DEBUG("=== Full sequence table for mount ===");
        for (const auto& seq : sequences) {
            LOG_DEBUG("SEQ id=", seq.id,
                     " dur=", seq.duration,
                     " flags=0x", std::hex, seq.flags, std::dec,
                     " moveSpd=", seq.movingSpeed,
                     " blend=", seq.blendTime,
                     " next=", seq.nextAnimation,
                     " alias=", seq.aliasNext);
        }
        LOG_DEBUG("=== End sequence table ===");

        std::set<uint32_t> forbiddenIds = {53, 54, 16};

        auto scoreNear = [](int a, int b) -> int {
            int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0;
        };

        auto isForbidden = [&](uint32_t id) {
            return forbiddenIds.count(id) != 0;
        };

        auto findSeqById = [&](uint32_t id) -> const pipeline::M2Sequence* {
            for (const auto& s : sequences) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        uint32_t runId = findFirst({anim::RUN, anim::WALK});
        uint32_t standId = findFirst({anim::STAND});

        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.duration >= 350 && seq.duration <= 1000 &&
                seq.id != runId && seq.id != standId) {
                loops.push_back(seq.id);
            }
        }

        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear(static_cast<int>(id), 38);
                const auto* s = findSeqById(id);
                if (s) sc += (s->duration >= 500 && s->duration <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }

        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;

        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;

            if (seq.duration >= 450 && seq.duration <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (loop && (seq.nextAnimation == static_cast<int16_t>(loop) || seq.aliasNext == loop)) sc += 30;
                if (loop && scoreNear(seq.nextAnimation, static_cast<int>(loop)) > 0) sc += 10;
                if (seq.blendTime > 400) sc -= 5;

                if (sc > bestStart) {
                    bestStart = sc;
                    start = seq.id;
                }
            }

            if (seq.duration >= 650 && seq.duration <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (seq.nextAnimation == static_cast<int16_t>(runId) || seq.nextAnimation == static_cast<int16_t>(standId)) sc += 10;
                if (seq.nextAnimation < 0) sc += 5;
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }

        LOG_DEBUG("Property-based jump discovery: start=", start, " loop=", loop, " end=", end,
                 " scores: start=", bestStart, " end=", bestEnd);
        return std::make_tuple(start, loop, end);
    };

    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    // Build MountAnimSet for MountFSM
    MountFSM::MountAnimSet mountAnims;
    mountAnims.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({anim::FALL, anim::JUMP_START});
    mountAnims.jumpLoop  = discoveredLoop > 0 ? discoveredLoop : findFirst({anim::JUMP});
    mountAnims.jumpEnd   = discoveredEnd > 0 ? discoveredEnd : findFirst({anim::JUMP_END});
    mountAnims.rearUp    = findFirst({anim::MOUNT_SPECIAL, anim::RUN_RIGHT, anim::FALL});
    mountAnims.run       = findFirst({anim::RUN, anim::WALK});
    mountAnims.stand     = findFirst({anim::STAND});
    // Discover flight animations (flying mounts only - may all be 0 for ground mounts)
    //
    // A 3.3.5 flying mount has Fly for moving and Hover for staying put - or
    // Fly for that too, when it has no Hover (the wyvern) - and RunRight and
    // RunLeft for strafing. Those two are the bank it leans into in the air:
    // three or four seconds long, moving nothing, and carried by fliers alone -
    // no ground mount has them - so they are strafing in flight, not on the
    // ground. It has nothing for climbing, diving or backing up, so those stay
    // empty and the FSM falls back.
    mountAnims.flyForward   = findFirst({anim::FLY_FORWARD});
    mountAnims.flyIdle      = findFirst({anim::FLY_IDLE, anim::FLY_FORWARD});
    mountAnims.flyLeft      = findFirst({anim::RUN_LEFT});
    mountAnims.flyRight     = findFirst({anim::RUN_RIGHT});

    // Discover idle fidget animations using proper WoW M2 metadata
    core::Logger::getInstance().debug("Scanning for fidget animations in ", sequences.size(), " sequences");

    core::Logger::getInstance().debug("=== ALL potential fidgets (no metadata filter) ===");
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary) {
            core::Logger::getInstance().debug("  ALL: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " flags=0x", std::hex, seq.flags, std::dec,
                " next=", seq.nextAnimation);
        }
    }

    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool hasFrequency = seq.frequency > 0;
        bool hasReplay = seq.replayMax > 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary && (hasFrequency || hasReplay)) {
            core::Logger::getInstance().debug("  Candidate: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " next=", seq.nextAnimation,
                " speed=", seq.movingSpeed);
        }

        bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        bool isSpecial = (seq.id == 2 || seq.id == 3);

        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration &&
            !isDeathOrWound && !isAttackOrCombat && !isSpecial) {
            bool chainsToStand = (seq.nextAnimation == static_cast<int16_t>(mountAnims.stand)) ||
                                 (seq.aliasNext == mountAnims.stand) ||
                                 (seq.nextAnimation == -1);

            mountAnims.fidgets.push_back(seq.id);
            core::Logger::getInstance().debug("  >> Selected fidget: id=", seq.id,
                (chainsToStand ? " (chains to stand)" : ""));
        }
    }

    if (mountAnims.run == 0) mountAnims.run = mountAnims.stand;

    core::Logger::getInstance().debug("Mount animation set: jumpStart=", mountAnims.jumpStart,
        " jumpLoop=", mountAnims.jumpLoop,
        " jumpEnd=", mountAnims.jumpEnd,
        " rearUp=", mountAnims.rearUp,
        " run=", mountAnims.run,
        " stand=", mountAnims.stand,
        " fidgets=", mountAnims.fidgets.size());

    // Configure the MountFSM.
    //
    // What is passed here is only what was true at this moment. The FSM takes
    // the per-frame answer as well now, and either counts - a taxi sets the
    // mount up before it says the flight has begun, so this one is false on
    // every flight.
    LOG_INFO("Mount configured: displayId=", mountDisplayId, " taxiFlight=", taxiFlight_);
    mountFSM_.configure(mountAnims, taxiFlight_);

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        bool isFlying = taxiFlight_;
        renderer_->getAudioCoordinator()->getMountSoundManager()->onMount(mountDisplayId, isFlying, modelPath);
    }
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

    mountFSM_.clear();

    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->onDismount();
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
        characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll, mountYawRad));
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
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, mountYawRad));
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

        float yawRad = glm::radians(characterYaw);
        float riderPitch = mountPitch_ * 0.35f;
        float riderRoll = mountRoll * 0.35f;
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, yawRad));
    } else {
        mountSeatSmoothingInit_ = false;
        float yawRad = glm::radians(characterYaw);
        glm::mat4 mountRotation = glm::mat4(1.0f);
        mountRotation = glm::rotate(mountRotation, yawRad, glm::vec3(0.0f, 0.0f, 1.0f));
        mountRotation = glm::rotate(mountRotation, mountRoll, glm::vec3(1.0f, 0.0f, 0.0f));
        mountRotation = glm::rotate(mountRotation, mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
        glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
        glm::vec3 playerPos = characterPosition + worldOffset;
        characterRenderer->setInstancePosition(characterInstanceId, playerPos);
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll, yawRad));
    }
}

// ── Mounted animation update (uses MountFSM) ────────────────────────────────

void AnimationController::updateMountedAnimation(float deltaTime) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    // The body's yaw rather than the facing, so mount and rider turn after
    // the player the way a unit on foot does. A taxi has no body lag.
    float characterYaw = (hasMountBodyYaw_ && !taxiFlight_) ? mountBodyYawDeg_
                                                            : renderer_->getCharacterYaw();
    hasMountBodyYaw_ = false;
    auto& mountFSM = mountFSM_;

    // Build MountFSM input
    MountFSM::Input mountIn;
    mountIn.moving = cameraController->isMoving();
    mountIn.movingForward = cameraController->isMovingForward();
    mountIn.movingBackward = cameraController->isMovingBackward();
    mountIn.strafeLeft = cameraController->isStrafingLeft();
    mountIn.strafeRight = cameraController->isStrafingRight();
    mountIn.grounded = cameraController->isGrounded();
    mountIn.jumpKeyPressed = cameraController->isJumpKeyPressed();
    // In the air, not allowed to be: on the ground a flying mount runs.
    mountIn.flying = cameraController->isFlightAirborne();
    mountIn.swimming = cameraController->isSwimming();
    mountIn.ascending = cameraController->isAscending();
    mountIn.descending = cameraController->isDescending();
    mountIn.taxiFlight = taxiFlight_;
    mountIn.deltaTime = deltaTime;
    mountIn.characterYaw = characterYaw;
    // Mount animation state query
    if (mountInstanceId_ > 0 && characterRenderer) {
        mountIn.haveMountState = characterRenderer->getAnimationState(
            mountInstanceId_, mountIn.curMountAnim, mountIn.curMountTime, mountIn.curMountDuration);
    }

    // Evaluate MountFSM
    auto mountOut = mountFSM.evaluate(mountIn);

    // The mount's stride follows the speed the player moves at, the rider's
    // does not move (FUN_007385c0 scales only movement animations).
    if (characterRenderer) {
        if (mountInstanceId_ > 0) {
            characterRenderer->setLocomotionSpeed(mountInstanceId_,
                                                  cameraController->getLocomotionSpeed());
        }
        characterRenderer->setLocomotionSpeed(characterInstanceId, 0.0f);
    }

    // Turning on the spot, the mount shuffles round as a unit on foot does
    // (its own ShuffleLeft/Right), standing on the ground.
    uint32_t shuffleAnim = 0;
    if (turnShuffle_ != body_yaw::TurnShuffle::None && !taxiFlight_ && !mountIn.moving &&
        mountIn.grounded && !mountIn.swimming && mountInstanceId_ > 0 && characterRenderer) {
        const uint32_t want = turnShuffle_ == body_yaw::TurnShuffle::Left ? anim::SHUFFLE_LEFT
                                                                         : anim::SHUFFLE_RIGHT;
        if (characterRenderer->hasAnimation(mountInstanceId_, want)) shuffleAnim = want;
    }

    // Apply mount animation if changed
    if (shuffleAnim != 0) {
        if (!mountIn.haveMountState || mountIn.curMountAnim != shuffleAnim) {
            characterRenderer->playAnimation(mountInstanceId_, shuffleAnim, true);
        }
    } else if (mountOut.mountAnimChanged && mountInstanceId_ > 0 && characterRenderer) {
        characterRenderer->playAnimation(mountInstanceId_, mountOut.mountAnimId, mountOut.mountAnimLoop);
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

    // Handle mount sounds
    auto* mountSoundMgr = renderer_->getAudioCoordinator()->getMountSoundManager();
    if (mountOut.playJumpSound && mountSoundMgr) {
        mountSoundMgr->playJumpSound();
    }
    if (mountOut.playLandSound && mountSoundMgr) {
        mountSoundMgr->playLandSound();
    }
    if (mountOut.playRearUpSound && mountSoundMgr) {
        mountSoundMgr->playRearUpSound();
    }
    if (mountOut.playIdleSound && mountSoundMgr) {
        mountSoundMgr->playIdleSound();
    }
    if (mountOut.triggerMountJump && cameraController) {
        cameraController->triggerMountJump();
    }

    // Apply positioning (uses mountBob and mountRoll from MountFSM)
    // For taxi flights, use external mountRoll_ set by setMountPitchRoll
    // For ground mounts, use MountFSM's computed lean roll
    float finalRoll = taxiFlight_ ? mountRoll_ : mountOut.mountRoll;
    applyMountPositioning(mountOut.mountBob, finalRoll, characterYaw);
}

// ── The player's animation ───────────────────────────────────────────────────

// The player is animated as any unit is: its base animation asked on each
// event of its movement (Application, 0x0073ed10), its swings, emotes,
// kits and wounds as they come, and what follows a one-shot by the
// CharacterRenderer (0x0073b510). Each frame leaves only the speed its
// movement animation plays at (0x007385c0), and the mount's driving.
void AnimationController::updateCharacterAnimation() {
    if (isMounted()) {
        updateMountedAnimation(lastDeltaTime_);
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

// ── Activity SFX state tracking ──────────────────────────────────────────────

void AnimationController::updateSfxState(float deltaTime) {
    sfxStateDriver_.update(deltaTime, renderer_, isMounted(), taxiFlight_,
                           footstepDriver_);
}

} // namespace rendering
} // namespace wowee

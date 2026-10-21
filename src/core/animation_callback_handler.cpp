#include "rendering/animation/melee_anim_chains.hpp"
#include "core/animation_callback_handler.hpp"
#include "core/appearance_composer.hpp"
#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/jump_landing.hpp"
#include "game/game_handler.hpp"
#include "game/spell_classification.hpp"
#include "game/world_packets.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"

#include <cmath>
#include <algorithm>

namespace wowee { namespace core {

AnimationCallbackHandler::AnimationCallbackHandler(
    EntitySpawner& entitySpawner,
    rendering::Renderer& renderer,
    game::GameHandler& gameHandler)
    : entitySpawner_(entitySpawner)
    , renderer_(renderer)
    , gameHandler_(gameHandler)
{
}

void AnimationCallbackHandler::setupCallbacks() {
    // Sprint aura callback - use SPRINT(143) animation when sprint-type buff is active
    gameHandler_.setSprintAuraCallback([this](bool active) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setSprintAuraActive(active);
    });

    // GUID → CharacterRenderer instance for bone-attached spell visuals:
    // local player uses the dedicated character instance, other units the
    // spawner's creature/player instances (0 = not rendered, no attachment).
    gameHandler_.setUnitRenderInstanceResolver([this](uint64_t guid) -> uint32_t {
        if (guid == gameHandler_.getPlayerGuid()) return renderer_.getCharacterInstanceId();
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        return instanceId;
    });

    // NPC/player death callback (online mode) - play death animation
    gameHandler_.setNpcDeathCallback([this](uint64_t guid) {
        entitySpawner_.markCreatureDead(guid);
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) {
            renderer_.getCharacterRenderer()->playAnimation(instanceId, rendering::anim::DEATH, false);
        }
    });

    // NPC/player respawn callback (online mode) - play rise animation then idle
    gameHandler_.setNpcRespawnCallback([this](uint64_t guid) {
        entitySpawner_.unmarkCreatureDead(guid);
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) {
            auto* cr = renderer_.getCharacterRenderer();
            // Play RISE one-shot (auto-returns to STAND when finished), fall back to STAND
            if (cr->hasAnimation(instanceId, rendering::anim::RISE))
                cr->playAnimation(instanceId, rendering::anim::RISE, false);
            else
                cr->playAnimation(instanceId, rendering::anim::STAND, true);
        }
    });

    // NPC/player swing callback (online mode) - play attack animation
    // Probes the model for the best available attack animation:
    //   ATTACK_1H(17) → ATTACK_2H(18) → ATTACK_2H_LOOSE(19) → ATTACK_UNARMED(16)
    gameHandler_.setNpcSwingCallback([this](uint64_t guid) {
        if (!renderer_.getCharacterRenderer()) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) {
            auto* cr = renderer_.getCharacterRenderer();
            const auto attackAnims =
                rendering::anim::meleeAnimChain(rendering::anim::MeleeChain::Generic);
            bool played = false;
            for (uint32_t anim : attackAnims) {
                if (cr->hasAnimation(instanceId, anim)) {
                    cr->playAnimation(instanceId, anim, false);
                    played = true;
                    break;
                }
            }
            if (!played) cr->playAnimation(instanceId, rendering::anim::ATTACK_UNARMED, false);
        }
    });

    // Hit reaction callback - plays one-shot dodge/block/wound animation on the victim
    gameHandler_.setHitReactionCallback([this](uint64_t victimGuid, game::GameHandler::HitReaction reaction) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;

        // Determine animation based on reaction type
        uint32_t animId = rendering::anim::COMBAT_WOUND;
        switch (reaction) {
            case game::GameHandler::HitReaction::DODGE:      animId = rendering::anim::DODGE; break;
            case game::GameHandler::HitReaction::PARRY:      break; // Parry already handled by existing system
            case game::GameHandler::HitReaction::BLOCK:      animId = rendering::anim::BLOCK; break;
            case game::GameHandler::HitReaction::SHIELD_BLOCK: animId = rendering::anim::SHIELD_BLOCK; break;
            case game::GameHandler::HitReaction::CRIT_WOUND: animId = rendering::anim::COMBAT_CRITICAL; break;
            case game::GameHandler::HitReaction::WOUND:      animId = rendering::anim::COMBAT_WOUND; break;
        }

        // A blow taken (0x00755e40 -> 0x00736640): StandWound, CombatWound
        // in a fight, CombatCritical, on the upper body or the whole body as
        // the unit stands.
        const bool isLocalPlayer = (victimGuid == gameHandler_.getPlayerGuid());
        if (reaction == game::GameHandler::HitReaction::WOUND ||
            reaction == game::GameHandler::HitReaction::CRIT_WOUND) {
            uint32_t instanceId = isLocalPlayer ? renderer_.getCharacterInstanceId()
                                                : entitySpawner_.getCreatureInstanceId(victimGuid);
            if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(victimGuid);
            if (instanceId == 0) return;
            const auto wound =
                cr->woundFor(instanceId, reaction == game::GameHandler::HitReaction::CRIT_WOUND);
            if (!wound) return;
            // The player's whole body is its animation controller's.
            auto* ac = isLocalPlayer && !wound->upper ? renderer_.getAnimationController() : nullptr;
            if (ac) ac->triggerHitReaction(wound->anim);
            else cr->playWound(instanceId, *wound);
            return;
        }

        // For local player: use AnimationController state
        if (isLocalPlayer) {
            auto* ac = renderer_.getAnimationController();
            if (ac) {
                uint32_t charInstId = renderer_.getCharacterInstanceId();
                if (charInstId && cr->hasAnimation(charInstId, animId))
                    ac->triggerHitReaction(animId);
            }
            return;
        }

        // For NPCs/other players: direct playAnimation
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(victimGuid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(victimGuid);
        if (instanceId != 0 && cr->hasAnimation(instanceId, animId))
            cr->playAnimation(instanceId, animId, false);
    });

    // Stun state callback - enters/exits STUNNED animation on local player
    gameHandler_.setStunStateCallback([this](bool stunned) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStunned(stunned);
    });

    // Stealth state callback - switches to stealth animation variants
    gameHandler_.setStealthStateCallback([this](bool stealthed) {
        auto* ac = renderer_.getAnimationController();
        if (ac) ac->setStealthed(stealthed);
    });

    // Player health callback - switches to wounded idle when HP < 20%
    gameHandler_.setPlayerHealthCallback([this](uint32_t health, uint32_t maxHealth) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        bool lowHp = (maxHealth > 0) && (health > 0) && (health * 5 <= maxHealth);
        ac->setLowHealth(lowHp);
    });

    // Another unit's jump, as the client plays it for one. MSG_MOVE_JUMP
    // plays JumpStart (FUN_0073ed10, case 0xbb), and when that ends the Jump
    // loop follows (FUN_0073b510), held while the unit is in the air by the
    // sync loops in Application::update. Touching down plays the landing
    // FUN_0073d2b0 picks from the flags the unit lands with: JumpEnd back to
    // its rest animation, JumpLandRun back to Run, or nothing - the sync
    // loop then takes a unit left in an airborne loop back to its locomotion.
    // Taking off into flight plays JumpStart too, unless the unit is falling
    // (Fall playing), and landing from flight JumpLandRun (FUN_0073ed10,
    // case 0x346); the sync loop picks the locomotion after either.
    //
    // A mounted player's jump is its mount's. The client plays a unit's
    // animations on its mount model when it has one (FUN_007385c0 hands the
    // animation to the mount at +0x98c, through the model's fallbacks,
    // FUN_007176f0), and the rider holds the mounted pose (+0xb7c, Mount
    // unless the mount says otherwise) throughout.
    gameHandler_.setUnitJumpCallback([this](uint64_t guid, game::ReportedJump jump) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId != 0) {
            if (const auto* mount = entitySpawner_.getRemotePlayerMount(guid)) {
                instanceId = mount->instanceId;
            }
        }
        if (instanceId == 0) {
            instanceId = entitySpawner_.getCreatureInstanceId(guid);
            // A creature rides as a player does.
            if (const auto* mount = instanceId != 0 ? entitySpawner_.getRemotePlayerMount(guid) : nullptr)
                instanceId = mount->instanceId;
        }
        if (instanceId == 0) return;
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (!entity) return;
        // Don't override Death animation
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) && curAnim == rendering::anim::DEATH) return;
        const bool takeOff =
            jump == game::ReportedJump::TakeOff && curAnim != rendering::anim::FALL;
        if (jump == game::ReportedJump::Launch || takeOff) {
            if (cr->hasAnimation(instanceId, rendering::anim::JUMP_START)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP_START, /*loop=*/false,
                                  rendering::anim::JUMP);
            } else if (cr->hasAnimation(instanceId, rendering::anim::JUMP)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP, /*loop=*/true);
            }
            return;
        }
        if (jump == game::ReportedJump::FlightLand) {
            if (cr->hasAnimation(instanceId, rendering::anim::JUMP_LAND_RUN)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP_LAND_RUN, /*loop=*/false);
            }
            return;
        }
        if (jump != game::ReportedJump::Land) return;
        const uint32_t flags = entity->getReportedMoveFlags();
        const auto& speeds = entity->getMovementSpeeds();
        const rendering::JumpLanding landing = rendering::jumpLandingForFlags(
            flags, game::Entity::speedForFlags(speeds, flags), speeds.walk);
        if (landing == rendering::JumpLanding::End &&
            cr->hasAnimation(instanceId, rendering::anim::JUMP_END)) {
            cr->playAnimation(instanceId, rendering::anim::JUMP_END, /*loop=*/false);
        } else if (landing == rendering::JumpLanding::LandRun &&
                   cr->hasAnimation(instanceId, rendering::anim::JUMP_LAND_RUN)) {
            cr->playAnimation(instanceId, rendering::anim::JUMP_LAND_RUN, /*loop=*/false,
                              rendering::anim::RUN);
        }
    });

    // Unit move-flags callback - updates swimming and walking state from every MSG_MOVE_* packet.
    // This is more reliable than opcode-based hints for cold joins and heartbeats:
    // a player already swimming when we join will have SWIMMING set on the first heartbeat.
    // Walking(4) vs Running(5) is also driven here from the WALKING flag.
    gameHandler_.setUnitMoveFlagsCallback([this](uint64_t guid, uint32_t moveFlags,
                                                 uint32_t mask) {
        // Only what this update speaks about. A full movement block passes a
        // mask of all ones; the spline opcodes each pass the one bit they are
        // named after, so starting to swim no longer says anything about
        // whether the creature is also flying.
        const auto apply = [&](game::MovementFlags flag, auto& state) {
            const uint32_t bit = static_cast<uint32_t>(flag);
            if ((mask & bit) == 0) return;
            if (moveFlags & bit) state[guid] = true;
            else                 state.erase(guid);
        };
        apply(game::MovementFlags::SWIMMING, entitySpawner_.getCreatureSwimmingState());
        apply(game::MovementFlags::WALKING,  entitySpawner_.getCreatureWalkingState());
        apply(game::MovementFlags::FLYING,   entitySpawner_.getCreatureFlyingState());
    });

    // Emote animation callback - play server-driven emote animations on NPCs and other players.
    // isState marks persistent STATE_ emotes (UNIT_NPC_EMOTESTATE or state-type
    // SMSG_EMOTE): they loop until cleared and are retained across model
    // reloads. One-shots play once and resume the retained state loop.
    gameHandler_.setEmoteAnimCallback([this](uint64_t guid, uint32_t emoteAnim, bool isState) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        const bool isCreature = entity && entity->getType() == game::ObjectType::UNIT;
        auto& activeEmotes = entitySpawner_.getCreatureActiveEmotes();
        if (isCreature && isState) {
            // State emotes can arrive while the creature's model is still loading.
            // Retain the raw animation so spawnOnlineCreature can apply it once the
            // correct display is instantiated.
            if (emoteAnim == 0) activeEmotes.erase(guid);
            else                activeEmotes[guid] = emoteAnim;
        }
        // Look up creature instance first, then online players
        uint32_t emoteInstanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (emoteInstanceId != 0) {
            if (emoteAnim == 0) {
                // A state clear returns to idle. A one-shot cancel (SMSG_EMOTE 0)
                // must not stomp a persistent UNIT_NPC_EMOTESTATE work loop -
                // resume it instead (Westfall woodworkers hammer through these).
                if (isState) activeEmotes.erase(guid);
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = rendering::anim::STAND;
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, resumeAnim, true);
            } else if (isState) {
                // A state emote loops the animation its Emotes.dbc row names -
                // STATE_DANCE is 69, the dance itself. 3.3.5 has no separate
                // looping variants to look for.
                activeEmotes[guid] = emoteAnim;
                cr->playAnimation(emoteInstanceId, emoteAnim, true);
            } else {
                // One-shot: play once, then resume the retained state loop (or Stand).
                auto retained = activeEmotes.find(guid);
                uint32_t resumeAnim = 0;
                if (retained != activeEmotes.end() &&
                    cr->hasAnimation(emoteInstanceId, retained->second)) {
                    resumeAnim = retained->second;
                }
                cr->playAnimation(emoteInstanceId, emoteAnim, false, resumeAnim);
            }
            return;
        }
        emoteInstanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (emoteInstanceId != 0) {
            if (emoteAnim == 0) {
                cr->playAnimation(emoteInstanceId, rendering::anim::STAND, true);
            } else if (isState) {
                // Looped, as the NPC branch above does.
                cr->playAnimation(emoteInstanceId, emoteAnim, true);
            } else {
                cr->playAnimation(emoteInstanceId, emoteAnim, false);
            }
        }
    });

    // Other units' stand state (UNIT_FIELD_BYTES_1 byte 0): sit down / lie
    // down / kneel through the transition into the loop, stand up through the
    // transition out. A unit not yet drawn takes its pose at spawn.
    gameHandler_.setUnitStandStateCallback([this](uint64_t guid, uint8_t oldState, uint8_t newState) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) &&
            curAnim == rendering::anim::DEATH) return;
        const auto from = rendering::anim::standStateAnims(oldState);
        const auto to = rendering::anim::standStateAnims(newState);
        uint32_t loopAnim = to.loop;
        if (newState == 0) {
            // Standing up resumes a retained NPC work loop, as the sync loop does.
            auto& activeEmotes = entitySpawner_.getCreatureActiveEmotes();
            auto it = activeEmotes.find(guid);
            if (it != activeEmotes.end() && cr->hasAnimation(instanceId, it->second))
                loopAnim = it->second;
        }
        if (!cr->hasAnimation(instanceId, loopAnim)) loopAnim = rendering::anim::STAND;
        cr->setRestAnimation(instanceId, newState == 0 ? 0 : loopAnim);
        const uint32_t transition = newState == 0 ? from.up : to.down;
        if (transition != 0 && cr->hasAnimation(instanceId, transition)) {
            cr->playAnimation(instanceId, transition, /*loop=*/false, loopAnim);
        } else {
            cr->playAnimation(instanceId, loopAnim, /*loop=*/true);
        }
    });

    // A cast's start and end: what its caster plays is its spell's kits'
    // (0x0073b140) - for the player through its animation controller.
    gameHandler_.setSpellCastAnimCallback([this](uint64_t guid, bool start, bool isChannel,
                                                  game::SpellCastType /*castType*/) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;

        // Determine if this is the local player
        bool isLocalPlayer = false;
        uint32_t instanceId = 0;
        {
            uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId != 0 && guid == gameHandler_.getPlayerGuid()) {
                instanceId = charInstId;
                isLocalPlayer = true;
            }
        }
        if (instanceId == 0) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;

        auto* svs = renderer_.getSpellVisualSystem();
        if (!isLocalPlayer) {
            // Another unit: its spell's kits animate it as they are played
            // on it - the PrecastKit's StartAnimID or AnimID at the cast's
            // start, the CastKit's as it goes (0x0073b140), the cast loop
            // between (0x00724330) and a channel's ChannelKit over all
            // (0x0071e400). The end of a cast whose PrecastKit animates it
            // asks the base animation again (0x007fec00), as does a
            // channel's (0x0073eb50).
            if (start) {
                const auto* cast = gameHandler_.getUnitCastState(guid);
                castSpells_[guid] = cast ? cast->spellId : 0u;
                return;
            }
            const uint32_t spell = castSpells_.count(guid) ? castSpells_[guid] : 0u;
            castSpells_.erase(guid);
            if (isChannel || (svs && svs->castAnimations(spell).animates())) cr->playBaseAnimation(instanceId);
            return;
        }

        if (start) {
            uint32_t currentSpell = gameHandler_.getCurrentCastSpellId();
            bool isFishing = isChannel && game::spellclass::isFishingCast(currentSpell);

            // Helper: pick first animation the model supports from a list
            auto pickFirst = [&](std::initializer_list<uint32_t> ids) -> uint32_t {
                for (uint32_t id : ids)
                    if (cr->hasAnimation(instanceId, id)) return id;
                return 0;
            };

            bool isBandage = false;
            bool isMining = false;
            bool isGathering = false;
            game::spellclass::RestChannelKind restChannel =
                game::spellclass::RestChannelKind::NONE;
            if (currentSpell != 0) {
                gameHandler_.loadSpellNameCache();
                auto it = gameHandler_.spellNameCacheRef().find(currentSpell);
                isBandage = (it != gameHandler_.spellNameCacheRef().end() &&
                             it->second.name.find("Bandage") != std::string::npos);
                if (it != gameHandler_.spellNameCacheRef().end()) {
                    restChannel = game::spellclass::classifyRestChannel(it->second.name);
                }
                if (!isBandage && !isFishing && it != gameHandler_.spellNameCacheRef().end()) {
                    const auto& nm = it->second.name;
                    isMining = (nm == "Mining" || nm == "Smelting");
                    if (!isMining) {
                        isGathering = gameHandler_.isProfessionSpell(currentSpell);
                        if (!isGathering)
                            isGathering = (nm == "Opening" || nm == "Open Lock");
                    }
                }
            }

            if (restChannel != game::spellclass::RestChannelKind::NONE) {
                if (auto* ac = renderer_.getAnimationController()) {
                    if (restChannel == game::spellclass::RestChannelKind::CANNIBALIZE) {
                        // Standing, and looping for the length of the channel.
                        //
                        // Not the food path: that seats the player and swaps
                        // the seated idle, and cannibalize does not sit - the
                        // character stays on their feet over the corpse. Not
                        // the potion path either, which is one swig and done,
                        // where this runs until the channel ends.
                        uint32_t eatAnim = pickFirst({rendering::anim::EMOTE_EAT,
                                                      rendering::anim::EATING_LOOP});
                        if (eatAnim != 0) {
                            ac->startSpellCast(0, eatAnim, /*castLoop=*/true, 0);
                        }
                    } else if (restChannel == game::spellclass::RestChannelKind::POTION ||
                        restChannel == game::spellclass::RestChannelKind::ALCOHOL) {
                        // Potions/alcohol are drunk standing: one EmoteEat swig.
                        uint32_t swigAnim = pickFirst({rendering::anim::EMOTE_EAT});
                        if (swigAnim != 0) {
                            ac->startSpellCast(0, swigAnim, false, 0);
                        }
                    } else {
                        // Food and water keep the player seated (UNIT_FIELD_BYTES_1)
                        // with the plain seated idle, exactly like the real client:
                        // player models ship NO seated eating animation - EmoteEat is
                        // authored standing (root bone stays at stand height) and
                        // looping it while seated pops the model upright. Only
                        // override the seated idle for models that truly have the
                        // dedicated EatingLoop sequence.
                        ac->setSeatedLoopAnimation(
                            pickFirst({rendering::anim::EATING_LOOP}));
                    }
                }
                // Food/water consume sounds repeat from SpellHandler::updateTimers
                // for the whole aura; potions get their single swig sound here.
                if (restChannel == game::spellclass::RestChannelKind::POTION ||
                    restChannel == game::spellclass::RestChannelKind::ALCOHOL) {
                    if (auto* audio = gameHandler_.services().audioCoordinator) {
                        if (auto* ui = audio->getUiSoundManager()) {
                            ui->playDrinking();
                        }
                    }
                }
            } else if (isMining) {
                // The pick is the mining kit's weapon effect (0x0073a6c0).
                uint32_t mineAnim = pickFirst({
                    rendering::anim::ATTACK_1H,
                    rendering::anim::EMOTE_WORK,
                    rendering::anim::EMOTE_USE_STANDING
                });
                if (auto* ac = renderer_.getAnimationController()) {
                    ac->startSpellCast(0, mineAnim ? mineAnim : rendering::anim::STAND, true, 0);
                }
            } else if (isBandage || isGathering) {
                uint32_t useStart = isChannel ? 0 : pickFirst({
                    rendering::anim::USE_STANDING_START,
                    rendering::anim::EMOTE_USE_STANDING_NO_SHEATHE,
                    rendering::anim::EMOTE_USE_STANDING
                });
                uint32_t useLoop = pickFirst({
                    rendering::anim::USE_STANDING_LOOP,
                    rendering::anim::EMOTE_USE_STANDING_NO_SHEATHE,
                    rendering::anim::EMOTE_USE_STANDING,
                    rendering::anim::CHANNEL_CAST_OMNI,
                    rendering::anim::READY_SPELL_OMNI,
                    rendering::anim::STAND
                });
                uint32_t useEnd = isChannel ? 0 : pickFirst({
                    rendering::anim::USE_STANDING_END,
                    rendering::anim::STAND
                });
                if (auto* ac = renderer_.getAnimationController()) {
                    ac->startSpellCast(useStart, useLoop ? useLoop : rendering::anim::STAND, true, useEnd);
                }
            } else if (isFishing && cr->hasAnimation(instanceId, rendering::anim::FISHING_LOOP)) {
                // Fishing is a one-shot pole cast followed by the channel idle.
                // The pole is the main hand, drawn by the channel at the bobber
                // (0x0073a520).
                if (auto* ac = renderer_.getAnimationController()) {
                    uint32_t castAnim = cr->hasAnimation(instanceId, rendering::anim::FISHING_CAST)
                        ? rendering::anim::FISHING_CAST : 0;
                    ac->startSpellCast(castAnim, rendering::anim::FISHING_LOOP, true, 0);
                }
            } else {
            // The spell's kits, as 0x0073b140 plays them: the PrecastKit's
            // StartAnimID once, then its AnimID while the cast runs - a
            // channel's ChannelKit's over everything (0x0071e400) - and the
            // CastKit's AnimID as it goes. A spell whose kits have none
            // animates nothing.
            const auto kits = svs ? svs->castAnimations(currentSpell)
                                  : rendering::SpellVisualSystem::CastAnimations{};
            const bool castTime = gameHandler_.getCastTimeTotal() > 0.0f;
            const uint32_t precastAnim = !isChannel && kits.precastStart >= 0
                                             ? static_cast<uint32_t>(kits.precastStart) : 0u;
            uint32_t castAnim = 0;
            if (isChannel) {
                if (const auto loop = svs ? svs->castLoop(0, currentSpell) : std::nullopt) castAnim = loop->anim;
            } else if (kits.precastAnim > 0 && castTime) {
                castAnim = static_cast<uint32_t>(kits.precastAnim);
            }
            const uint32_t finalizeAnim = !isChannel && kits.castAnim >= 0 ? static_cast<uint32_t>(kits.castAnim) : 0u;
            if (precastAnim != 0 || castAnim != 0 || finalizeAnim != 0) {
                // The animation controller needs a hold between: the start
                // where there is no loop, else the release.
                const uint32_t hold = castAnim != 0 ? castAnim : precastAnim != 0 ? precastAnim : finalizeAnim;
                if (auto* ac = renderer_.getAnimationController())
                    ac->startSpellCast(precastAnim, hold, castAnim != 0, finalizeAnim);
            }
            } // end !isFishing
        } else {
            // Cast/channel ended - plays finalization anim completely then returns to idle
            if (auto* ac = renderer_.getAnimationController()) {
                ac->setSeatedLoopAnimation(0);
                ac->stopSpellCast();
            }
        }
    });

    // Ghost state callback - make player semi-transparent when in spirit form
    gameHandler_.setGhostStateCallback([this](bool isGhost) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t charInstId = renderer_.getCharacterInstanceId();
        if (charInstId == 0) return;
        cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
    });

    // Stand state animation callback - route through AnimationController state machine
    // for proper sit/sleep/kneel transition animations (down → loop → up)
    gameHandler_.setStandStateCallback([this](uint8_t standState) {
        using AC = rendering::AnimationController;

        // The camera controller's two flags, from the one number: movement is
        // blocked while sitting or kneeling, and grounding stands down while
        // the server has the character in a chair.
        //
        // Both used to be set, by two callbacks - and this is one slot, so the
        // one registered later replaced the other. This handler is built long
        // after the world is, so the registration that carried
        // applyServerStandState never ran again: seatedInChair_ stayed false,
        // the chair stayed a floor, and a character sat down in the right pose
        // on top of the furniture rather than in it.
        if (auto* cc = renderer_.getCameraController()) {
            cc->applyServerStandState(standState);
        }

        auto* ac = renderer_.getAnimationController();
        if (!ac) return;

        // Death is special - play directly, not through sit state machine
        if (standState == AC::STAND_STATE_DEAD) {
            auto* cr = renderer_.getCharacterRenderer();
            if (!cr) return;
            uint32_t charInstId = renderer_.getCharacterInstanceId();
            if (charInstId == 0) return;
            cr->playAnimation(charInstId, rendering::anim::DEATH, false);
            return;
        }

        ac->setStandState(standState);
        // Restoration food/water is an aura rather than MSG_CHANNEL_START on
        // supported realms. The aura update starts its loop; standing up ends it.
        // Only stop the cast presentation while restoring - the server also
        // stands a seated player when a real cast begins, and stopping there
        // would kill that cast's animation as it starts.
        if (standState == AC::STAND_STATE_STAND) {
            ac->setSeatedLoopAnimation(0);
            if (gameHandler_.isRestoring()) ac->stopSpellCast();
        }
    });

    // Loot window callback - play kneel/loot animation while looting
    gameHandler_.setLootWindowCallback([this](bool open) {
        auto* ac = renderer_.getAnimationController();
        if (!ac) return;
        if (open) ac->startLooting();
        else      ac->stopLooting();
    });
}

}} // namespace wowee::core

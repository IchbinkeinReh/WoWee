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
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "rendering/animation/jump_landing.hpp"
#include "rendering/unit_model_events.hpp"
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
        // the unit stands - the player's as any unit's.
        uint32_t instanceId = victimGuid == gameHandler_.getPlayerGuid()
                                  ? renderer_.getCharacterInstanceId()
                                  : entitySpawner_.getCreatureInstanceId(victimGuid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(victimGuid);
        if (instanceId == 0) return;
        if (reaction == game::GameHandler::HitReaction::WOUND ||
            reaction == game::GameHandler::HitReaction::CRIT_WOUND) {
            const auto wound =
                cr->woundFor(instanceId, reaction == game::GameHandler::HitReaction::CRIT_WOUND);
            if (wound) cr->playWound(instanceId, *wound);
            return;
        }
        if (cr->hasAnimation(instanceId, animId)) cr->playAnimation(instanceId, animId, false);
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
        // Its own jump sound goes with it, asked of the unit outright as the
        // client's movement handlers ask it (0x0073ed10 cases 0xbb and 0x346,
        // FUN_0073d2b0): its JumpStart as it leaves the ground, its JumpEnd as
        // it comes down. Of the 1306 sound kits 30 name either column, 25 of
        // those the universal flying mount's (SoundEntries 10905 taking off,
        // 10904 landing); no player's kit does, so a character jumping or
        // landing a flying mount is as silent here as the client leaves it.
        const auto jumpSound = [&](uint32_t slot) {
            if (entity->isUnit())
                entitySpawner_.playUnitJumpSound(static_cast<const game::Unit&>(*entity), slot);
        };
        // Don't override Death animation
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) && curAnim == rendering::anim::DEATH) return;
        const bool takeOff =
            jump == game::ReportedJump::TakeOff && curAnim != rendering::anim::FALL;
        if (jump == game::ReportedJump::Launch || takeOff) {
            // The launch's sound goes first, whether or not the model has an
            // animation for it (0x0073ed10 asks it and only then picks one).
            jumpSound(rendering::unit_event::kSlotJumpStart);
            if (cr->hasAnimation(instanceId, rendering::anim::JUMP_START)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP_START, /*loop=*/false,
                                  rendering::anim::JUMP);
            } else if (cr->hasAnimation(instanceId, rendering::anim::JUMP)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP, /*loop=*/true);
            }
            return;
        }
        if (jump == game::ReportedJump::FlightLand) {
            jumpSound(rendering::unit_event::kSlotJumpEnd);
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
        // FUN_0073d2b0 asks the landing sound in the two branches that play a
        // landing animation, and not as the unit goes straight back to its
        // locomotion.
        if (landing == rendering::JumpLanding::End) {
            jumpSound(rendering::unit_event::kSlotJumpEnd);
            if (cr->hasAnimation(instanceId, rendering::anim::JUMP_END))
                cr->playAnimation(instanceId, rendering::anim::JUMP_END, /*loop=*/false);
        } else if (landing == rendering::JumpLanding::LandRun) {
            jumpSound(rendering::unit_event::kSlotJumpEnd);
            if (cr->hasAnimation(instanceId, rendering::anim::JUMP_LAND_RUN)) {
                cr->playAnimation(instanceId, rendering::anim::JUMP_LAND_RUN, /*loop=*/false,
                                  rendering::anim::RUN);
            }
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

    // An emote's voice line. The client asks it of the unit the emote shows
    // on - 0x00746d60's emote-flagged branch, which is also where both emote
    // CVars are read - so what reaches the spawner is the narrated emote's
    // EmotesText id and nothing else.
    gameHandler_.setEmoteSoundCallback([this](uint64_t guid, uint32_t emotesTextId) {
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        entitySpawner_.playUnitEmoteSound(static_cast<const game::Unit&>(*entity), emotesTextId);
    });

    // A reaction going to HATE, which the unit answers out loud. The client
    // resolves the unit first (0x00716b10 asks Unit_C.cpp for one, and plays
    // nothing where there is none or it is not a unit), then hands the slot
    // to the unit itself.
    gameHandler_.setUnitAlertSoundCallback([this](uint64_t guid) {
        auto entity = gameHandler_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        entitySpawner_.playUnitAlertSound(static_cast<const game::Unit&>(*entity));
    });

    // Other units' stand state (UNIT_FIELD_BYTES_1 byte 0), as the client
    // takes its change (0x0073f460 -> 0x0073f060): the base animation, from
    // the state its animation was given for, sits it down, lies it down,
    // kneels it or gets it up, and the transition's end asks its loop
    // (0x0073b510). A unit not yet drawn takes its pose at spawn.
    gameHandler_.setUnitStandStateCallback([this](uint64_t guid, uint8_t /*oldState*/, uint8_t newState) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) &&
            curAnim == rendering::anim::DEATH) return;
        cr->standStateChanged(instanceId, newState);
    });

    // A cast's start and end: what its caster plays is its spell's kits'
    // (0x0073b140), the player's as any unit's.
    gameHandler_.setSpellCastAnimCallback([this](uint64_t guid, bool start, bool isChannel,
                                                  game::SpellCastType /*castType*/) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        const bool isLocalPlayer = guid == gameHandler_.getPlayerGuid();
        uint32_t instanceId = isLocalPlayer ? renderer_.getCharacterInstanceId() : 0;
        if (instanceId == 0 && !isLocalPlayer) instanceId = entitySpawner_.getCreatureInstanceId(guid);
        if (instanceId == 0 && !isLocalPlayer) instanceId = entitySpawner_.getPlayerInstanceId(guid);
        if (instanceId == 0) return;

        // Its spell's kits animate it as they are played on it - the
        // PrecastKit's StartAnimID or AnimID at the cast's start, the
        // CastKit's as it goes (0x0073b140), the cast loop between
        // (0x00724330) and a channel's ChannelKit over all (0x0071e400). The
        // end of a cast whose PrecastKit animates it asks the base animation
        // again (0x007fec00), as does a channel's (0x0073eb50).
        auto* svs = renderer_.getSpellVisualSystem();
        if (start) {
            uint32_t spell = 0;
            if (isLocalPlayer) {
                spell = gameHandler_.getCurrentCastSpellId();
            } else if (const auto* cast = gameHandler_.getUnitCastState(guid)) {
                spell = cast->spellId;
            }
            castSpells_[guid] = spell;
            // A potion or a drink taken standing: one swig's sound.
            if (isLocalPlayer && spell != 0) {
                gameHandler_.loadSpellNameCache();
                const auto it = gameHandler_.spellNameCacheRef().find(spell);
                const auto rest = it != gameHandler_.spellNameCacheRef().end()
                                      ? game::spellclass::classifyRestChannel(it->second.name)
                                      : game::spellclass::RestChannelKind::NONE;
                if (rest == game::spellclass::RestChannelKind::POTION ||
                    rest == game::spellclass::RestChannelKind::ALCOHOL) {
                    if (auto* audio = gameHandler_.services().audioCoordinator) {
                        if (auto* ui = audio->getUiSoundManager()) ui->playDrinking();
                    }
                }
            }
            return;
        }
        const uint32_t spell = castSpells_.count(guid) ? castSpells_[guid] : 0u;
        castSpells_.erase(guid);
        if (isChannel || (svs && svs->castAnimations(spell).animates())) cr->playBaseAnimation(instanceId);
    });

    // Ghost state callback - make player semi-transparent when in spirit form
    gameHandler_.setGhostStateCallback([this](bool isGhost) {
        auto* cr = renderer_.getCharacterRenderer();
        if (!cr) return;
        uint32_t charInstId = renderer_.getCharacterInstanceId();
        if (charInstId == 0) return;
        cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
    });

    // The player's stand state as the server has it (SMSG_STANDSTATE_UPDATE,
    // 0x0073f540 -> 0x0073f060): sat down, laid down, knelt or got up from
    // the state its animation was given for.
    gameHandler_.setStandStateCallback([this](uint8_t standState) {
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
        auto* cr = renderer_.getCharacterRenderer();
        const uint32_t instanceId = renderer_.getCharacterInstanceId();
        if (cr && instanceId != 0) cr->standStateChanged(instanceId, standState);
    });
}

}} // namespace wowee::core

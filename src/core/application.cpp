#include "core/test_frame_shot.hpp"
#include "core/stall_report.hpp"
#include "core/pointer_speed.hpp"
#include "core/platform.hpp"
#include "core/application.hpp"
#include "rendering/frustum.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/model_frame_view.hpp"
#include "core/rt_session_marker.hpp"
#include "core/cache_root.hpp"
#include "core/config_paths.hpp"
#include "core/world_settle.hpp"
#include "core/cinematic_player.hpp"
#include "game/mount_special.hpp"
#include "game/mover_keys.hpp"
#include "game/game_utils.hpp"
#include "game/mover_mode.hpp"
#include "game/dressing_room.hpp"
#include "game/player_display_flags.hpp"
#include "game/protocol_constants.hpp"
#include "game/vehicle_camera.hpp"
#include "game/vehicle_mover.hpp"
#include "core/frame_pacer.hpp"
#include "core/env_flag.hpp"
#include "core/character_paths.hpp"
#include "ui/safe_area.hpp"
#include "ui/settings_schema.hpp"
#include "ui/settings_i18n.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/camera_smooth.hpp"
#include "pipeline/m2_asset_loader.hpp"
#include "core/coordinates.hpp"
#include "core/app_clock.hpp"
#include "ui/minimap_projection.hpp"
#include "core/profiler.hpp"
#include "core/frame_profiler.hpp"
#include "core/npc_interaction_callback_handler.hpp"
#include "core/audio_callback_handler.hpp"
#include "core/entity_spawn_callback_handler.hpp"
#include "core/animation_callback_handler.hpp"
#include "core/transport_callback_handler.hpp"
#include "core/world_entry_callback_handler.hpp"
#include "core/ui_screen_callback_handler.hpp"
#include "game/spell_classification.hpp"
#include "rendering/swim_pitch.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/jump_landing.hpp"
#include "rendering/animation/mover_event.hpp"
#include "rendering/unit_model_events.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/movement_limits.hpp"
#include <bit>
#include <unordered_set>
#include <cmath>
#include <chrono>
#include <limits>
#include <utility>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "core/logger.hpp"
#include "core/memory_monitor.hpp"
#include "rendering/renderer.hpp"
#include "rendering/fishing_line.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/loot_sparkles.hpp"
#include "rendering/vk_context.hpp"
#include "audio/npc_voice_manager.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/weather.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/keybinding_manager.hpp"
#include "rendering/quest_giver_markers.hpp"
#include "audio/ui_sound_manager.hpp"
#include "game/quest_giver_status.hpp"
#include "game/minimap_tracking.hpp"
#include "rendering/footprint_renderer.hpp"
#include "rendering/loading_screen.hpp"
#include "rendering/world_text_renderer.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/audio_engine.hpp"
#include "ui/screen_keyboard_sync.hpp"
#include "ui/android_ime.hpp"
#include "addons/lua_api_registrations.hpp"
#include "audio/audio_coordinator.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_taint.hpp"
#include "addons/lua_api_helpers.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wmo_group_path.hpp"
#include "pipeline/wdt_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "ui/ui_manager.hpp"
#include "ui/map_window.hpp"
#include "ui/touch_controls.hpp"
#include "ui/gamepad_controls.hpp"
#include "core/gamepad.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/data_paths.hpp"
#include "core/data_prompt.hpp"
#include "pipeline/data_stream.hpp"
#include "core/env.hpp"
#include "ui/ui_services.hpp"
#include "auth/auth_handler.hpp"
#include "game/game_handler.hpp"
#include "game/idle_steps.hpp"
#include "game/spell_handler.hpp"
#include "game/chat_handler.hpp"
#include "game/faction_hostility.hpp"
#include "game/car_ride.hpp"
#include "game/transport_manager.hpp"
#include "game/world.hpp"
#include "game/expansion_profile.hpp"
#include "game/packet_parsers.hpp"
#include "pipeline/asset_inventory.hpp"
#include "pipeline/mpq_archive_set.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/spell_icon_paths.hpp"
#include "mcp/gate.hpp"
#ifndef WOWEE_MOBILE
#include "mcp/server.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <set>
#include <filesystem>
#include <fstream>

#include <tuple>
#include <thread>
#ifdef WOWEE_VR
#include "ui/scene_pick.hpp"
#include "vr/vr_runtime.hpp"
#endif
#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace wowee {
namespace core {

namespace {


// Where another unit's jump leaves the sync loop's locomotion choice this
// frame (airborneAnimSync): true while the jump's animations hold it - Fall
// started for a unit in the air without one; refresh set for a unit down
// again but still in an airborne loop, or whose landing a movement packet
// cut short, whose locomotion must be picked although nothing else changed.
// The instance is the one the unit's animations play on: a mounted player's
// mount.
bool jumpHoldsAnimation(rendering::CharacterRenderer& charRenderer, uint32_t instanceId,
                        const game::Entity& entity, bool flying, bool locomotionAsked,
                        bool& refresh) {
    uint32_t curAnim = 0;
    float curT = 0.0f, curDur = 0.0f;
    if (!charRenderer.getAnimationState(instanceId, curAnim, curT, curDur) ||
        curAnim == rendering::anim::DEATH) {
        return false;
    }
    switch (rendering::airborneAnimSync(entity.isAirborne(), curAnim, flying, locomotionAsked)) {
        case rendering::AirborneAnimSync::Hold:
            return true;
        case rendering::AirborneAnimSync::Fall:
            if (charRenderer.hasAnimation(instanceId, rendering::anim::FALL)) {
                charRenderer.playAnimation(instanceId, rendering::anim::FALL, /*loop=*/true);
            }
            return true;
        case rendering::AirborneAnimSync::Refresh:
            refresh = true;
            return false;
        case rendering::AirborneAnimSync::None:
            break;
    }
    return false;
}

// The body another unit is drawn at, as Renderer draws the player's
// (FUN_0073dab0; see body_yaw.hpp), and the shuffle it plays turning on the
// spot. Nothing stamps +0xabc for a unit that is not the player's own, so
// its body takes its facing - the eased one, FUN_00735f60's - whole each
// frame: only strafing turns it aside, SpineLow taking half the look back
// and the head the rest. What it turns standing still, or a turn key it
// holds, shuffles its feet; the shuffle starts only from its stand, as
// FUN_0071de90 turns it down over an emote or a cast and the combat stance
// is asked first (FUN_0071e0d0), and stops back into the stand. Returns
// the body yaw, in the render yaw's frame `facing` is given in.
// The transport a unit stands on, for its body to turn with (turnUnitBody).
uint64_t unitRideGuidOf(game::GameHandler& gh, uint64_t guid) {
    const auto& attached = gh.transportAttachmentsRef();
    const auto it = attached.find(guid);
    return it != attached.end() ? it->second.transportGuid : 0;
}
const game::ActiveTransport* unitRideOf(game::GameHandler& gh, uint64_t guid) {
    const uint64_t rideGuid = unitRideGuidOf(gh, guid);
    auto* transports = gh.getTransportManager();
    return (rideGuid != 0 && transports) ? transports->getTransport(rideGuid) : nullptr;
}

float turnUnitBody(rendering::CharacterRenderer& cr, uint32_t instanceId,
                   EntitySpawner::UnitBody& body, float facing, uint32_t moveFlags,
                   float deltaTime, bool snap, bool mayShuffle, uint32_t mountInstance,
                   const game::ActiveTransport* ride = nullptr, uint64_t rideGuid = 0) {
    namespace by = rendering::body_yaw;
    if (body.instanceId != instanceId) {
        body = {};
        body.instanceId = instanceId;
        body.state.body = by::wrap(facing);
        body.hasSpine = cr.hasKeyBone(instanceId, 4);
        body.hasHead = cr.hasKeyBone(instanceId, 6);
    }
    // A passenger's facing is the transport's plus its own on it
    // (0x004f42a0): as a zeppelin turns in a curve, the facing of everyone
    // standing on its deck turns with it, and so does the body under that
    // facing. Measured in the world, the body lagged the deck's turn and the
    // lag read as a turn on the spot - every passenger shuffled its feet
    // through the curve.
    if (ride && body.rideGuid == rideGuid) {
        body.state.body = by::wrap(body.state.body + core::coords::normalizeAngleRad(ride->facing - body.rideFacing));
    }
    body.rideGuid = ride ? rideGuid : 0;
    body.rideFacing = ride ? ride->facing : 0.0f;
    by::Input in;
    in.facing = facing;
    in.dt = deltaTime;
    in.moveFlags = moveFlags;
    in.catchUpSeconds = 1.0e6f;  // +0xabc never stamped: the whole lag
    in.snap = snap;
    in.hasSpine = body.hasSpine;
    in.hasHead = body.hasHead;
    in.halveSpine = true;
    const by::Result r = by::update(body.state, in);
    // On a mount the spine keeps its pose (+0x98c), the head looks on.
    cr.setInstanceTorsoYaw(instanceId, mountInstance != 0 ? 0.0f : r.spineYaw, r.headYaw);

    // The model it shows, its mount's where it rides (vtable +0xd4), shuffles.
    const uint32_t shown = mountInstance != 0 ? mountInstance : instanceId;
    const by::TurnShuffle want = by::turnShuffle(moveFlags, r.step, mayShuffle);
    body.turning = want != by::TurnShuffle::None;
    body.turningLeft = want == by::TurnShuffle::Left;
    uint32_t wantAnim = want == by::TurnShuffle::Left  ? rendering::anim::SHUFFLE_LEFT
                      : want == by::TurnShuffle::Right ? rendering::anim::SHUFFLE_RIGHT : 0;
    if (wantAnim != 0 && !cr.hasAnimation(shown, wantAnim)) wantAnim = 0;
    uint32_t cur = 0;
    float curTime = 0.0f, curDuration = 0.0f;
    if (cr.getAnimationState(shown, cur, curTime, curDuration)) {
        const bool shuffling = cur == rendering::anim::SHUFFLE_LEFT || cur == rendering::anim::SHUFFLE_RIGHT;
        if (wantAnim != 0 && cur != wantAnim && (cur == rendering::anim::STAND || shuffling)) {
            cr.playAnimation(shown, wantAnim, /*loop=*/true);
        } else if (wantAnim == 0 && shuffling) {
            cr.playAnimation(shown, rendering::anim::STAND, /*loop=*/true);
        }
    }
    return body.state.body;
}

// The movement flags FUN_0073dab0 reads for another unit: its last packet's,
// with forward for one moved along a spline, whose packets carry none, and
// swimming or flying as the sync tracks them.
uint32_t unitBodyFlags(const game::Entity& entity, bool moving, bool swimming, bool flying) {
    namespace by = rendering::body_yaw;
    uint32_t flags = entity.getReportedMoveFlags();
    if (!moving) flags &= ~(by::kForward | by::kBackward | by::kStrafe);
    else if ((flags & (by::kForward | by::kBackward | by::kStrafe)) == 0) flags |= by::kForward;
    if (swimming) flags |= by::kSwimming;
    if (flying) flags |= by::kFlying;
    return flags;
}

// Another unit's swimming or flying tilt (swim_pitch::otherUnit,
// 0x00719a90): the pitch its last movement packet carried, while its flags
// say forward or back.
float unitSwimTilt(const game::GameHandler& gh, const game::Entity& entity, uint64_t guid, bool swimmingOrFlying) {
    namespace by = rendering::body_yaw;
    const bool forwardOrBack = (entity.getReportedMoveFlags() & (by::kForward | by::kBackward)) != 0;
    return rendering::swim_pitch::otherUnit(swimmingOrFlying, forwardOrBack, gh.vehicleRoster().pitch(guid));
}

// UNIT_FIELD_BYTES_1 byte 3: the unit's animation tier (+0xb80, kept by
// 0x007167c0), which 0x007176f0 picks the animations it is asked by.
uint8_t unitAnimationTier(const game::Entity& entity) {
    const uint16_t index = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1);
    return index == 0xFFFF ? 0 : static_cast<uint8_t>(entity.getField(index) >> 24);
}

// What a unit's move spline says of it (+0xbc): flying (0x2000,
// 0x004f5290), falling (0x200) or arcing (0x800; 0x006e9a70, 0x006e9aa0) -
// in the air to 0x00723350 where it falls - and hovering as 0x0071c6c0 has
// it: on a flying spline, else MOVEMENTFLAG_HOVER, else (not on a falling
// one) gravity off (0x400).
void unitSplineState(rendering::upper_body::Unit& u, game::GameHandler& gh, uint64_t guid, uint32_t moveFlags) {
    const uint32_t spline = gh.moveSplineFlags(guid).value_or(0u);
    u.splineFlying = (spline & 0x2000u) != 0;
    u.splineFalling = (spline & 0xa00u) != 0;
    if ((spline & 0x200u) != 0) u.leaping = true;
    const bool fallingSpline = (spline & 0x200u) != 0;
    u.hovering = u.splineFlying || (moveFlags & 0x40000000u) != 0 || (!fallingSpline && (moveFlags & 0x400u) != 0);
}

// What 0x007385c0 reads of another unit for its upper body: its movement
// flags as FUN_0073dab0 reads them, falling, turning on the spot, its stand
// state, its mount, its cast or channel (+0xa38 0x400 is a cast with a cast
// time), its melee target, its ranged weapon out, whether its model moves
// only as a whole (CreatureModelData Flags 0x8), and the pose it rides in.
// Falling far (MOVEMENTFLAG_FALLING_FAR).
constexpr uint32_t kMoveFallingFar = 0x2000;

rendering::upper_body::Unit unitAnimationState(game::GameHandler& gh, EntitySpawner& spawner,
                                               uint64_t guid, const game::Unit& unit, uint32_t moveFlags,
                                               bool turning, std::optional<uint32_t> riderPose) {
    namespace ub = rendering::upper_body;
    ub::Unit u;
    u.moveFlags = moveFlags | (unit.isFallingByFlags() ? ub::kMoveFalling : 0u);
    u.leaping = unit.isAirborne();  // 0x00723350
    u.turningInPlace = turning;
    u.standState = unit.getStandState();
    u.mounted = unit.getMountDisplayId() != 0;
    if (const auto* cast = gh.getUnitCastState(guid)) {
        u.channeling = cast->isChannel;
        u.castTimed = !cast->isChannel && cast->timeTotal > 0.0f;
    }
    u.autoAttacking = gh.getUnitMeleeTarget(guid) != 0;
    u.rangedDrawn = spawner.unitSheathState(guid) == SheathState::Ranged;
    u.wholeBodyOnly = (spawner.creatureModelFlags(unit.getDisplayId()).value_or(0) & 0x8u) != 0;
    u.riding = riderPose.has_value();
    if (riderPose) u.riderPose = *riderPose;
    u.alive = unit.getHealth() > 0;
    // What its base animation reads besides (0x00724500): feigning death,
    // its walk speed, stealth (UNIT_FIELD_BYTES_1 byte 2 0x2), hovering or
    // gravity off, its weapons' ready, and its emote state's animation.
    if (const uint16_t f2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2); f2 != 0xFFFF)
        u.feignDeath = (unit.getField(f2) & 0x1u) != 0;
    if (const uint16_t b1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1); b1 != 0xFFFF)
        u.stealthed = ((unit.getField(b1) >> 16) & 0x2u) != 0;
    u.walkSpeed = unit.getMovementSpeeds().walk;
    unitSplineState(u, gh, guid, unit.getReportedMoveFlags());
    u.meleeReady = spawner.unitMeleeReady(guid);
    u.rangedSubClass = spawner.unitRangedSubClass(guid);
    // Not a state with Emotes Flags 0x2000 - working, mining, dancing - while it is
    // spoken to (0x007171c0: the interaction object, 0x00bd07a8).
    if (const uint32_t emote = unit.getNpcEmoteState(); emote != 0 &&
        (gh.getInteractionObject() != guid ||
         (rendering::AnimationController::getEmoteFlagsByEmotesId(emote) & 0x2000u) == 0)) {
        if (const uint32_t anim = rendering::AnimationController::getEmoteAnimByEmotesId(emote); anim != 0)
            u.emoteStateAnim = anim;
    }
    // UNIT_FIELD_FLAGS: may loot (0x10000000 clear, 0x007222a0), loots
    // (0x400, 0x0071b6b0), in combat (0x800, 0x0071afb0).
    if (const uint16_t f = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS); f != 0xFFFF) {
        const uint32_t flags = unit.getField(f);
        u.canLoot = (flags & 0x10000000u) == 0;
        u.looting = (flags & 0x400u) != 0;
        u.inCombat = (flags & 0x800u) != 0;
    }
    u.inCombat = u.inCombat || u.autoAttacking;
    // A vehicle that casts as a whole (VehicleFlags 0x10000, 0x00756f40).
    u.vehicleCastOnly = (spawner.vehicleFlags(guid) & 0x10000u) != 0;
    // +0xa38 0x200: it began a repeating spell (AttributesEx2 0x20,
    // 0x007fa2e0); nothing clears another unit's.
    auto& body = spawner.getUnitBodies()[guid];
    if (const auto* cast = gh.getUnitCastState(guid);
        cast && cast->casting && (gh.getSpellAttributesEx2(cast->spellId) & 0x20u) != 0)
        body.autoRepeat = true;
    u.autoRepeat = body.autoRepeat;
    return u;
}

// What 0x00724330 and 0x0071e400 read of a unit's spells: the loop its cast
// or channel holds it in (0x007224d0), and the state kit animating it.
void unitKitAnimationState(rendering::upper_body::Unit& u, rendering::SpellVisualSystem* svs, uint64_t guid,
                           uint32_t castSpellId, uint32_t channelSpellId) {
    if (!svs) return;
    // +0xa38 0x400: casting a spell whose PrecastKit animates the unit
    // (0x007fa2e0 -> 0x00715af0).
    u.castTimed = castSpellId != 0 && svs->castAnimations(castSpellId).animates();
    if (const auto loop = svs->castLoop(castSpellId, channelSpellId)) {
        u.castLoopAnim = loop->anim;
        u.castLoopKitFlags = loop->kitFlags;
    }
    if (const auto kit = svs->unitKitAnimation(guid)) {
        u.kitAnim = kit->anim;
        u.kitAnimTiered = kit->tiered;
    }
    if (const auto death = svs->unitDeathKit(guid)) {
        u.deathKitAnim = death->anim;
        u.deathKitFlags = death->kitFlags;
    }
}

uint32_t unitChannelSpell(const game::Entity& entity) {
    const uint16_t index = game::fieldIndex(game::UF::UNIT_CHANNEL_SPELL);
    return index == 0xFFFF ? 0 : entity.getField(index);
}

// A unit on the traced water's waves ("Ride the waves", Renderer::
// updateWaveRide), drawn only: swimming, it rides them where it swims; aboard
// a ship that rides them, it rides with the ship. `drawnAt` is where the sync
// placed its model (on a mount, the rider up on the saddle); nothing is
// written back.
void feedUnitWaveRide(rendering::Renderer& renderer, EntitySpawner& spawner, uint64_t guid, uint32_t instanceId,
                      const game::Unit& unit, const glm::vec3& feet, bool dead) {
    auto* gh = Application::getInstance().getGameHandler();
    if (dead || !gh) return;
    constexpr uint32_t kSwimming = static_cast<uint32_t>(game::MovementFlags::SWIMMING);
    const bool swimming =
        (unit.getReportedMoveFlags() & kSwimming) != 0 || spawner.getCreatureSwimmingState().count(guid) > 0;
    const auto& attachments = gh->transportAttachmentsRef();
    const auto attached = attachments.find(guid);
    const uint64_t transportGuid = attached != attachments.end() ? attached->second.transportGuid : 0;
    const auto* mount = spawner.getRemotePlayerMount(guid);
    renderer.rideWavesUnit(guid, instanceId, mount ? mount->instanceId : 0u, feet, swimming, transportGuid);
}

// Another unit's animation tier and what 0x007385c0 reads of it, onto its
// instance, from what the sync has worked out of how it moves.
// A unit's ripples, within reach of the character: the client's every unit in
// liquid (0x0071cba0), at its own scale and collision height. Out to 150
// yards (rendering::SwimEffects::kUnitRippleRange): at 60 a swimmer across a
// lake or a canal left the water still. Over the traced water, whose sprite
// ripples are not drawn, the unit disturbs the traced surface as the
// character does there (Renderer::unitInRtWater), to the same range. A rider's
// are its mount's, at the mount's feet: at its own, up on the seat, a mount
// wading left the water still.
void feedUnitRipple(rendering::Renderer& renderer, EntitySpawner& spawner, uint64_t guid, uint32_t instanceId,
                    const game::Unit& unit, const glm::vec3& feet, bool moving, bool dead, float deltaTime) {
    // The ride on the waves first: it wants every unit in the sync, not only
    // those in the water within the ripples' reach.
    feedUnitWaveRide(renderer, spawner, guid, instanceId, unit, feet, dead);
    auto* swim = renderer.getSwimEffects();
    auto* water = renderer.getWaterRenderer();
    if (dead || !swim || !water) return;
    const glm::vec3 away = feet - renderer.getCharacterPosition();
    constexpr float kRange = rendering::SwimEffects::kUnitRippleRange;
    if (glm::dot(away, away) > kRange * kRange) return;
    const auto waterZ = rendering::liquid_query::surfaceOverUnit(renderer.liquidWorld(), feet);
    if (!waterZ || *waterZ <= feet.z) return;
    if (!swim->surfaceRipplesShown()) {
        constexpr uint32_t kSwimming = static_cast<uint32_t>(game::MovementFlags::SWIMMING);
        const bool swimming = (unit.getReportedMoveFlags() & kSwimming) != 0 ||
                              spawner.getCreatureSwimmingState().count(guid) > 0;
        renderer.unitInRtWater(guid, instanceId, feet, *waterZ, moving, swimming);
        return;
    }
    const auto size = spawner.unitRippleSize(unit);
    swim->unitInWater(guid, feet, *waterZ, moving, unit.getMoveSpeed(), unit.getModelFacing(), size.first,
                      size.second, deltaTime);
}

void syncUnitAnimation(rendering::CharacterRenderer& cr, game::GameHandler& gh, EntitySpawner& spawner,
                       rendering::SpellVisualSystem* svs, uint64_t guid, uint32_t instanceId, const game::Unit& unit,
                       bool moving, bool swimming, bool flying, uint32_t mountInstance) {
    cr.setAnimationTier(instanceId, unitAnimationTier(unit));
    // The mount under it, which plays what the unit is asked (0x007385c0).
    cr.setRiderMount(instanceId, mountInstance);
    cr.setRiderSeat(instanceId, mountInstance);
    const bool riding = mountInstance != 0;
    const auto& body = spawner.getUnitBodies()[guid];
    auto state = unitAnimationState(gh, spawner, guid, unit, unitBodyFlags(unit, moving, swimming, flying),
                                    body.turning,
                                    riding ? std::optional(spawner.riderPose(guid, instanceId)) : std::nullopt);
    state.turningLeft = body.turningLeft;
    const auto* cast = gh.getUnitCastState(guid);
    unitKitAnimationState(state, svs, guid, cast && cast->casting && !cast->isChannel ? cast->spellId : 0u,
                          unitChannelSpell(unit));
    cr.setUnitAnimationState(instanceId, state);
}

// The height a moving unit is drawn at over the floor found under it. On the
// ground it is set on the floor. Falling, the client moves it by gravity and
// stops it only where it meets the ground (FUN_007618b0 moving it through
// FUN_007612b0's collision): kept above the floor, not pulled down onto it,
// so a jump rises and comes down instead of sliding along the ground.
float heightOverFloor(const game::Entity& entity, float z, float floorZ) {
    return entity.isFallingByFlags() ? std::max(z, floorZ) : floorZ;
}

// Whether a moving unit is drawn on the floor found under it, or where its
// movement puts it.
//
// The client moves another unit each frame in CMovement's update (0x006eac40).
// On a spline it takes the point the spline gives for the time (0x006e9c30 ->
// 0x0098ca00: the curve or the straight line between the server's points,
// its height changed only by a falling or parabolic spline's gravity) and
// 0x006e9e20 sets the unit there as it is - no collision and no floor below
// it - for every unit but the one the player moves. Moved on by its flags
// instead - another player between its packets - the same 0x006e9e20 sends
// it through the collision (0x00762e00 -> 0x007620f0), which keeps it on the
// ground it walks over; the floor found here stands in for that.
//
// A unit moved towards a packet's position is not moved by the client at all
// (it is set where the packet says), so it keeps the packet's height as well.
bool unitOnFloorWhileMoving(const game::Entity& entity) {
    return entity.isMovingByFlags();
}

std::optional<float> movingEntityFloor(rendering::Renderer* renderer,
                                        const glm::vec3& renderPos,
                                        const std::optional<glm::vec3>& previousRenderPos) {
    if (!renderer) return std::nullopt;

    // Server movement Z is the reference surface.  In WMO overlap regions the
    // outdoor heightfield may be a roof many units above a tunnel/interior, so
    // choose the closest reachable floor instead of blindly preferring terrain.
    constexpr float kMaxStepUp = 1.5f;
    constexpr float kMaxGroundDrop = 3.0f;
    const float probeZ = renderPos.z + kMaxStepUp;
    std::optional<float> best;

    auto consider = [&](const std::optional<float>& floor) {
        if (!floor || *floor > probeZ || *floor < renderPos.z - kMaxGroundDrop) return;
        if (!best || std::abs(*floor - renderPos.z) < std::abs(*best - renderPos.z)) {
            best = floor;
        }
    };

    if (auto* terrain = renderer->getTerrainManager()) {
        consider(terrain->getHeightAt(renderPos.x, renderPos.y));
    }
    // Outdoor movers almost always match the terrain heightfield. Avoid the
    // expensive WMO/M2 collision walks in that common case. Tunnel, bridge,
    // and interior overlaps still use full arbitration because raw terrain is
    // not close to the server-provided Z there.
    if (best && std::abs(*best - renderPos.z) <= 0.35f) {
        return best;
    }
    if (auto* wmo = renderer->getWMORenderer()) {
        // Closest to the mover's own feet, so a creature or another player
        // under an overhang is not snapped up to the floor of the level above
        // them - the same flip that pulled the local player between levels.
        consider(wmo->getFloorHeight(renderPos.x, renderPos.y, probeZ, nullptr,
                                     renderPos.z));
    }
    if (auto* m2 = renderer->getM2Renderer()) {
        consider(m2->getFloorHeight(renderPos.x, renderPos.y, probeZ));
    }

    // A jump between floors, named once a second while it is happening.
    //
    // Reported as the character pulled between levels in Undercity - in
    // portals, doorways, overhangs - which is this arbitration flipping between
    // two floors that both sit inside the step-up/drop window near a level
    // transition. Which floor won, from which source, and how far it moved the
    // player is the thing a report cannot carry and this can. Warning level and
    // rate-limited, because the default log is warnings only and a per-frame
    // line would bury everything else.
    if (best && previousRenderPos &&
        std::abs(*best - previousRenderPos->z) > 1.0f) {
        static std::chrono::steady_clock::time_point lastFloorJumpLog{};
        static core::LogBudget floorJumpBudget(12, "Floor jump");
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFloorJumpLog > std::chrono::seconds(1) && floorJumpBudget.take()) {
            lastFloorJumpLog = now;
            std::optional<float> terrainF, wmoF, m2F;
            if (auto* t = renderer->getTerrainManager())
                terrainF = t->getHeightAt(renderPos.x, renderPos.y);
            if (auto* w = renderer->getWMORenderer())
                wmoF = w->getFloorHeight(renderPos.x, renderPos.y, probeZ);
            if (auto* mm = renderer->getM2Renderer())
                m2F = mm->getFloorHeight(renderPos.x, renderPos.y, probeZ);
            LOG_WARNING("Floor jump: player z=", renderPos.z, " prev z=",
                        previousRenderPos->z, " -> chose ", *best,
                        " (terrain=", terrainF ? *terrainF : -99999.0f,
                        " wmo=", wmoF ? *wmoF : -99999.0f,
                        " m2=", m2F ? *m2F : -99999.0f,
                        ") - moved ", *best - previousRenderPos->z);
        }
    }

    // A broader floor candidate is useful on stairs and uneven terrain, but it is
    // ambiguous near overlapping shells or non-collidable authored props. Require
    // continuity with the last rendered ground position before accepting it. This
    // preserves server-authored waypoint height without creature-entry exceptions.
    if (best && std::abs(*best - renderPos.z) > 0.35f) {
        if (!previousRenderPos) return std::nullopt;
        const glm::vec2 planarDelta = glm::vec2(renderPos) - glm::vec2(*previousRenderPos);
        const float maxContinuousStep = 0.35f + glm::length(planarDelta) * 1.5f;
        if (std::abs(*best - previousRenderPos->z) > maxContinuousStep) {
            return std::nullopt;
        }
    }
    return best;
}

/// movingEntityFloor for one unit, asked every frame near the camera and
/// every third frame past 40 yards, where a frame-old floor cannot be seen:
/// in Dalaran every walker below a WMO floor sent a ray through the city a
/// frame, a few hundred of them. A unit that has moved more than a yard and a
/// half since asks at once wherever it is.
std::optional<float> unitMovingFloor(uint64_t guid, rendering::Renderer* renderer,
                                     const glm::vec3& renderPos,
                                     const std::optional<glm::vec3>& previousRenderPos,
                                     uint32_t frameNo) {
    struct Last { glm::vec2 xy; std::optional<float> floor; };
    static std::unordered_map<uint64_t, Last> last;
    if (last.size() > 4096) last.clear();
    const rendering::Camera* camera = renderer ? renderer->getCamera() : nullptr;
    const glm::vec3 eye = camera ? camera->getPosition() : renderPos;
    const glm::vec3 toEye = renderPos - eye;
    const bool distant = glm::dot(toEye, toEye) > 40.0f * 40.0f;
    auto it = last.find(guid);
    if (distant && it != last.end() && ((guid + frameNo) % 3u) != 0) {
        const glm::vec2 moved = glm::vec2(renderPos) - it->second.xy;
        if (glm::dot(moved, moved) < 1.5f * 1.5f) return it->second.floor;
    }
    auto floor = movingEntityFloor(renderer, renderPos, previousRenderPos);
    last[guid] = Last{glm::vec2(renderPos), floor};
    return floor;
}

/// A wheel event's travel in clicks of a clicking wheel, which is what the
/// interface scrolls by.
///
/// macOS reports a trackpad or a Magic Mouse in pixels, and SDL passes those
/// on at a tenth - a "line" per ten pixels - while a clicking wheel comes
/// through as whole clicks. A two-finger swipe with its momentum runs to a
/// hundred of those lines, and a scroll frame moves half a page per notch, so
/// even at a quarter of the scroll speed one swipe went through the quest log
/// ten pages at a time. Elsewhere a trackpad already reports in clicks.
///
/// A precise delta is one with a fraction, which a click never has: SDL rounds
/// those away from zero. Pixels that happen to come to a whole ten do not have
/// one either, so a gesture stays precise for half a second after its last
/// fraction, and momentum keeps it going well past that.
float wheelClicks(float delta, uint64_t timestampNs) {
#if defined(__APPLE__)
    constexpr float kLinesPerClick = 20.0f;
    constexpr uint64_t kGestureGapNs = 500'000'000ull;
    static uint64_t lastPreciseNs = 0;
    const bool fractional = delta != std::trunc(delta);
    if (fractional) lastPreciseNs = timestampNs;
    const bool precise = fractional ||
        (lastPreciseNs != 0 && timestampNs - lastPreciseNs < kGestureGapNs);
    return precise ? delta / kLinesPerClick : delta;
#else
    (void)timestampNs;
    return delta;
#endif
}

/// The thread the main loop runs on, which is the one SDL's Android pump
/// sends the lifecycle events from.
SDL_ThreadID gMainThreadId = 0;
/// A lifecycle change seen on another thread, for the main loop to act on:
/// 0 nothing, 1 background, 2 foreground.
std::atomic<int> gPendingLifecycle{0};

void applyLifecycle(Window* window, bool background, const char* why) {
    rendering::VkContext* vk = window ? window->getVkContext() : nullptr;
    if (background) {
        // SDL leaves the audio running when it is not told to block on pause,
        // and this client tells it not to (see Window::initialize).
        audio::AudioEngine::instance().setSuspended(true);
        if (vk) vk->enterBackground(why);
    } else {
        if (vk) vk->enterForeground(why);
        // As the focus change would leave it: Sound in Background decides
        // only for a window that lost the focus, and a resumed one has it.
        audio::AudioEngine::instance().setSuspended(false);
        // Whether the connection lived through it (WILL_ENTER_FOREGROUND
        // never gets here, so this is once per return).
        if (auto* gh = core::Application::getInstance().getGameHandler()) gh->noteResumedFromBackground();
    }
}

/// Acts on the app going to the background and coming back as SDL sends the
/// events, not when a loop gets round to reading them.
///
/// The main loop is not the only reader of SDL's queue: the world load, the
/// terrain warmup and the loading screen each drain it with a loop of their
/// own and drop whatever is not a quit or a resize. A phone sent to the home
/// screen during a load lost WILL_ENTER_BACKGROUND that way, kept drawing to a
/// surface whose window was gone, and came back to a black screen that never
/// recovered. A watch sees every event whoever polls for it.
///
/// On Android SDL sends these from the pump, which runs on the thread that
/// called SDL_PollEvent - the main thread, between frames - so the Vulkan work
/// can be done here. Anything that arrives from another thread is left for the
/// main loop.
bool SDLCALL lifecycleWatch(void* userdata, SDL_Event* event) {
    bool background = false;
    const char* why = nullptr;
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND: background = true;  why = "WILL_ENTER_BACKGROUND"; break;
        case SDL_EVENT_DID_ENTER_BACKGROUND:  background = true;  why = "DID_ENTER_BACKGROUND";  break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
            LOG_WARNING("SDL_EVENT_WILL_ENTER_FOREGROUND");
            return true;
        case SDL_EVENT_DID_ENTER_FOREGROUND:  background = false; why = "DID_ENTER_FOREGROUND";  break;
        case SDL_EVENT_LOW_MEMORY:
            LOG_WARNING("SDL_EVENT_LOW_MEMORY: the system is short of memory");
            // The ray tracing's guard offers to switch it off (rt_memory_guard).
            if (auto* r = core::Application::getInstance().getRenderer()) r->noteSystemLowMemory();
            // The system may end the app next: what the log holds goes to disk now.
            Logger::getInstance().flushNow();
            return true;
        case SDL_EVENT_TERMINATING:
            LOG_WARNING("SDL_EVENT_TERMINATING: the system is ending the app");
            return true;
        default:
            return true;
    }
    if (SDL_GetCurrentThreadID() != gMainThreadId) {
        LOG_WARNING(why, " arrived off the main thread; handled next frame");
        gPendingLifecycle.store(background ? 1 : 2, std::memory_order_release);
        return true;
    }
    applyLifecycle(static_cast<Window*>(userdata), background, why);
    return true;
}

/// The interface's edit focus and the on-screen keyboard; see
/// ScreenKeyboardSync.
ui::ScreenKeyboardSync gKeyboardSync;

/// Whether the system's keyboard is on the screen: the window's IME inset
/// where the activity reports it, SDL's own answer where it does not.
bool screenKeyboardShown(SDL_Window* sdlWindow) {
    const int reported = ui::reportedImeVisibility();
    if (reported >= 0) return reported == 1;
    return sdlWindow && SDL_ScreenKeyboardShown(sdlWindow);
}

} // namespace

Application* Application::instance = nullptr;

Application::Application() {
    instance = this;
}

Application::~Application() {
    shutdown();
    instance = nullptr;
}

namespace {

/// The interface's text files out of the archives into a folder the addon
/// manager can read: <cache root>/interface_mpq/interface/... (core::
/// getCacheRoot: cache/ in the working directory, the caches folder on iOS,
/// whose working directory is the read-only app bundle). Written again only
/// when the archives change (a stamp of their names and sizes). Returns the
/// root to hand the addon manager.
std::string writeInterfaceFromArchives(const pipeline::AssetManager& am) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(core::getCacheRoot()) / "interface_mpq";
    const auto* set = am.mpqArchives();
    if (!set) return root.string();
    std::string stamp = "v1 " + set->archiveDir() + " " + set->locale() + "\n";
    {
        std::string loc;
        for (const auto& p : pipeline::MpqArchiveSet::discoverArchives(set->archiveDir(), loc)) {
            std::error_code ec;
            stamp += fs::path(p).filename().string() + " " + std::to_string(fs::file_size(p, ec)) + "\n";
        }
    }
    const fs::path stampPath = root / ".stamp";
    {
        std::ifstream in(stampPath, std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (have == stamp) {
            LOG_INFO("Interface files from the archives: up to date in ", root.string());
            return root.string();
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t written = 0, bytes = 0, failed = 0;
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const char* prefix : {"interface\\framexml\\", "interface\\gluexml\\", "interface\\addons\\"}) {
        for (const std::string& name : am.listFiles(prefix)) {
            const size_t dot = name.rfind('.');
            const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
            if (ext != ".lua" && ext != ".xml" && ext != ".toc") continue;
            const auto data = am.readFile(name);
            if (data.empty()) continue;
            std::string rel = name;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const fs::path out = root / rel;
            fs::create_directories(out.parent_path(), ec);
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!o) {
                // Counted as written before, so a folder that takes nothing
                // (an app bundle) read as a success and the interface as empty.
                ++failed;
                continue;
            }
            ++written;
            bytes += data.size();
        }
    }
    if (failed) {
        LOG_ERROR("Interface files from the archives: ", failed, " could not be written into ",
                  fs::absolute(root).string(), " - the interface will be missing them");
        return root.string();   // no stamp: tried again next start
    }
    std::ofstream(stampPath, std::ios::binary | std::ios::trunc) << stamp;
    LOG_WARNING("Interface files from the archives: ", written, " files (", bytes / 1024, " KB) into ",
                fs::absolute(root).string(), " in ",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                " ms");
    return root.string();
}

} // namespace

namespace {

/// The streaming server's file list fetched and its cache made ready: the
/// cache's mirror of the server's folder, or empty with `error` saying why.
/// Network and disk only - safe on a thread of its own while nothing reads
/// the game data yet.
std::string startStreaming(const core::DataStreamChoice& stream, std::string& error) {
    if (stream.url.empty()) {
        error = "no streaming server is set";
        return {};
    }
    pipeline::StreamUrl url;
    if (!pipeline::parseStreamUrl(stream.url, url, error)) return {};
    return pipeline::DataStream::instance().start(
        {stream.url, stream.key}, pipeline::DataStream::defaultCacheRoot(url), error);
}

}  // namespace

bool Application::worldComing() const {
    return loadingScreenUp() || (worldLoader_ && worldLoader_->isLoadingWorld());
}

bool Application::initialize() {
    LOG_INFO("Initializing Wowee Native Client");

    // Initialize memory monitoring for dynamic cache sizing
    core::MemoryMonitor::getInstance().initialize();

#ifdef WOWEE_VR
    // The headset first: without one there is nothing for this exe to do,
    // and the runtime has to be there before Vulkan is, since it makes
    // Vulkan's instance and device (vr/vr_runtime.hpp).
    {
        std::string why;
        if (!vr::Runtime::get().initialize(why)) {
            vr::reportUnavailable(why);
            return false;
        }
    }
#endif

    // Create window
    WindowConfig windowConfig;
    windowConfig.title = "Wowee";
#ifdef WOWEE_VR
    windowConfig.title = "Wowee VR";
#endif
    windowConfig.width = 1280;
    windowConfig.height = 720;
    // Pace rendering to the display by default. The old 240 FPS default kept
    // the main thread near a full core even while the scene was idle.
    windowConfig.vsync = true;
#ifdef WOWEE_VR
    // The interface is laid out at the window's size and shown on the
    // headset's panel at that resolution: a bigger window is a sharper panel.
    windowConfig.width = 1920;
    windowConfig.height = 1080;
#endif

    window = std::make_unique<Window>(windowConfig);
    if (!window->initialize()) {
        LOG_FATAL("Failed to initialize window");
        return false;
    }

    gMainThreadId = SDL_GetCurrentThreadID();
    if (!SDL_AddEventWatch(lifecycleWatch, window.get())) {
        LOG_WARNING("Could not watch for the app's lifecycle events: ", SDL_GetError());
    }

    // Controllers, which are a window's business rather than a renderer's:
    // SDL delivers their events on the same queue, and a pad plugged in later
    // arrives the same way. A client that cannot talk to them still starts.
    {
        auto& pad = core::gamepad();
        pad.init();
        // What a button is bound to is the interface's to say, and the
        // interface is built after this - so the router asks for it on each
        // press rather than holding on to it.
        ui::gamepadControls().setKeyRouter([this](const char* padKey) {
            ui::PadKeyAnswer answer;
            auto* engine = addonManager_ ? addonManager_->getLuaEngine() : nullptr;
            if (!engine) return answer;
            auto outcome = engine->dispatchPadKey(padKey);
            if (outcome.taken) {
                answer.kind = ui::PadKeyAnswer::Kind::Taken;
            } else if (!outcome.command.empty()) {
                answer.kind = ui::PadKeyAnswer::Kind::Command;
                answer.imguiKey = addons::clientImGuiKeyForBinding(outcome.command);
                answer.command = std::move(outcome.command);
            }
            return answer;
        });
        // A mapping file for a pad SDL does not already know. Beside the
        // installed data first, because that is the directory a player can
        // reach without a terminal, then beside the binary for a checkout.
        for (const std::filesystem::path& where :
             {core::userDataRoot() / "gamecontrollerdb.txt",
              std::filesystem::path("assets/gamecontrollerdb.txt")}) {
            std::error_code ec;
            if (!std::filesystem::exists(where, ec)) continue;
            if (pad.addMappingsFromFile(where.string()) >= 0) break;
        }
    }

    // Create renderer
    renderer = std::make_unique<rendering::Renderer>();
    if (!renderer->initialize(window.get())) {
        LOG_FATAL("Failed to initialize renderer");
        return false;
    }
    renderer->setSelectionCirclePlace([this](uint64_t guid) { return getSelectionCircle(guid); });

    // Create and initialize audio coordinator (owns all audio managers)
    audioCoordinator_ = std::make_unique<audio::AudioCoordinator>();
    if (!audioCoordinator_->initialize())
        LOG_WARNING("Audio coordinator initialization failed - game will run without audio");
    renderer->setAudioCoordinator(audioCoordinator_.get());

    // Create UI manager
    uiManager = std::make_unique<ui::UIManager>();
    if (!uiManager->initialize(window.get())) {
        LOG_FATAL("Failed to initialize UI manager");
        return false;
    }

    // The saved anti-aliasing setting, applied here rather than from the game
    // screen's update.
    //
    // GameScreen's constructor has just read settings.cfg, so the value is
    // known - but update() does not run until the game screen is up, which is
    // after character select. A saved setting therefore rebuilt the swapchain,
    // every render pass and every pipeline around fifteen hundred frames into
    // the session, with a world loaded, textures resident and uploads in
    // flight. Here the renderer has drawn nothing yet and there is nothing for
    // the rebuild to disturb.
    uiManager->getGameScreen().applySavedAntiAliasing(renderer.get());
    // And the window's own two, for the same reason and at the same moment:
    // nothing has been drawn, so entering fullscreen here costs no visible
    // change of mode.
    uiManager->getGameScreen().applySavedDisplayMode(window.get());
    // And the buildings' surface bumps: the first world load builds every
    // building it shows with whatever the WMO renderer holds, and the game
    // screen's own application of them waits until that load is over.
    uiManager->getGameScreen().applySavedSurfaceDetail(renderer.get());
    // And the ground clutter's density and distance: the login's own tiles
    // are prepared before the game screen applies anything, and kept the
    // default density until they streamed out and back.
    uiManager->getGameScreen().applySavedGroundClutter(renderer.get());

    // Create subsystems
    authHandler = std::make_unique<auth::AuthHandler>();
    world = std::make_unique<game::World>();

    // Create and initialize expansion registry
    expansionRegistry_ = std::make_unique<game::ExpansionRegistry>();

    // Create DBC layout
    dbcLayout_ = std::make_unique<pipeline::DBCLayout>();

    // Create asset manager
    assetManager = std::make_unique<pipeline::AssetManager>();

    // Populate game services - all subsystems now available
    gameServices_.renderer = renderer.get();
    gameServices_.audioCoordinator = audioCoordinator_.get();
    gameServices_.assetManager = assetManager.get();
    gameServices_.expansionRegistry = expansionRegistry_.get();

    // Create game handler with explicit service dependencies
    gameHandler = std::make_unique<game::GameHandler>(gameServices_);

    // Try to get WoW data path from environment variable
    const char* dataPathEnv = std::getenv("WOW_DATA_PATH");
    std::string dataPath = dataPathEnv ? dataPathEnv : "./Data";

    // The "streaming" data mode: the original archives read from a server
    // through a local chunk cache (pipeline/data_stream). Chosen by a stored
    // server (core::getDataStream) unless WOWEE_DATA_MODE says otherwise, or
    // by WOWEE_DATA_MODE=streaming. The cache's mirror of the server's folder
    // is then read as the game folder, exactly as a local install.
    {
        const char* modeEnv = std::getenv("WOWEE_DATA_MODE");
        const core::DataStreamChoice stream = core::getDataStream();
        streamChosen_ = core::streamingChosen(modeEnv ? modeEnv : "", stream.url);
        if (streamChosen_) {
            std::string error;
            const std::string mirror = startStreaming(stream, error);
            if (applyStreamStart(mirror, error)) dataPath = mirror;
        }
    }

    openGameData(dataPath, /*atStartup=*/true);

    // Set up UI callbacks
    setupUICallbacks();

    LOG_INFO("Application initialized successfully");
    running = true;
    return true;
}

bool Application::applyStreamStart(const std::string& mirror, const std::string& error) {
    // The cache's clear is asked for once (data_stream reset its row in
    // settings.cfg); the settings were read before this, so the panel's
    // copy goes back to 0 too, or its next save would ask again.
    if (uiManager) uiManager->getGameScreen().getSettingsPanel().setSettingValue("streamcacheclear", "0");
    if (mirror.empty()) {
        // Kept for the data screen, which says it - this used to reach the
        // log only, and the screen simply came back.
        streamError_ = error.empty() ? std::string("the server gave no answer") : error;
        LOG_ERROR("Streaming game data: ", streamError_);
        return false;
    }
    streamError_.clear();
    // Everything that looks the data folder up again finds the same.
    core::setEnvVar("WOW_DATA_PATH", mirror.c_str(), /*overwrite=*/true);
    if (!std::getenv("WOWEE_DATA_MODE")) core::setEnvVar("WOWEE_DATA_MODE", "mpq", /*overwrite=*/true);
    return true;
}

bool Application::startStreamingNow() {
    if (streamStartThread_.joinable()) return true;   // already on its way
    // Opened already (a folder found, then forgotten): what the interface
    // was built on cannot be swapped under it. A restart reads the server.
    if (assetManager && assetManager->isInitialized()) return false;
    streamError_.clear();
    streamStartLoading_ = false;
    streamStartDone_.store(false, std::memory_order_relaxed);
    const core::DataStreamChoice stream = core::getDataStream();
    // Its own thread: an address that does not answer takes the socket's
    // whole timeout, and the screen says what it is waiting for meanwhile.
    streamStartThread_ = std::thread([this, stream] {
        std::string error;
        std::string mirror = startStreaming(stream, error);
        streamStartMirror_ = std::move(mirror);
        streamStartFailure_ = std::move(error);
        streamStartDone_.store(true, std::memory_order_release);
    });
    return true;
}

Application::StreamStart Application::getStreamStart() const {
    if (!streamStartThread_.joinable()) return StreamStart::Idle;
    return streamStartLoading_ ? StreamStart::Loading : StreamStart::Connecting;
}

void Application::pollStreamStart() {
    if (!streamStartThread_.joinable() || !streamStartDone_.load(std::memory_order_acquire)) return;
    if (!streamStartLoading_ && streamStartFailure_.empty() && !streamStartMirror_.empty()) {
        // One frame that says the archives are being opened, before the
        // frames that are not drawn while they are.
        streamStartLoading_ = true;
        return;
    }
    streamStartThread_.join();
    streamStartLoading_ = false;
    const std::string mirror = std::move(streamStartMirror_);
    const std::string error = std::move(streamStartFailure_);
    streamStartMirror_.clear();
    streamStartFailure_.clear();
    if (!applyStreamStart(mirror, error)) return;   // the screen says why
    streamChosen_ = true;

    // What the start would have done had the server been there then: the
    // archives opened, everything built on them, the callbacks wired. Here,
    // between frames - update() runs before the interface's frame is begun.
    openGameData(mirror, /*atStartup=*/false);
    setupUICallbacks();
    if (!assetManager || !assetManager->isInitialized()) {
        streamError_ = "the streamed archives could not be opened";
        LOG_ERROR("Streaming game data: ", streamError_);
        return;
    }
    // The server's archives are the data now, whatever a forgotten folder
    // choice would have said.
    dataPrompt_ = core::DataPrompt::None;
    LOG_WARNING("Streaming game data: started from the data screen, no restart");
    setState(AppState::AUTHENTICATION);
}

void Application::openGameData(const std::string& dataPath, bool atStartup) {
    // The original client's archives (Data\*.MPQ) instead of an extraction:
    // the asset manager reads them in place, and the expansion tables - which
    // ship with this client, not with the game - are read from this client's
    // own Data/ (relative to the working directory, like its shaders). An
    // extraction in the same folder wins unless WOWEE_DATA_MODE=mpq.
    const char* dataModeEnv = std::getenv("WOWEE_DATA_MODE");
    const std::string dataMode = dataModeEnv ? dataModeEnv : "";
    const bool mpqData = dataMode != "extracted" && pipeline::MpqArchiveSet::findArchiveDir(dataPath) &&
                         (dataMode == "mpq" || !core::holdsExtraction(dataPath));
    const std::string tablesRoot = mpqData ? std::string("Data") : dataPath;
    if (mpqData) {
        LOG_WARNING("Game data: the client's MPQ archives in ", dataPath, " (tables from ",
                    std::filesystem::absolute("Data").string(), ")");
    }

    // The client's own expansion tables, into the extraction it is about to
    // read them from. Without this an extraction in the per-user directory -
    // where the asset builder writes one - had no expansion.json, so no
    // expansion was found, no assets opened, and a login went no further. See
    // syncClientTables.
    if (!mpqData) {
        std::vector<std::string> failures;
        const int copied = core::syncClientTables("Data", dataPath, &failures);
        if (copied > 0) {
            LOG_WARNING("Copied ", copied, " of this client's expansion tables into ",
                        dataPath);
        }
        for (const std::string& f : failures) {
            LOG_WARNING("Could not write ", f, " - that expansion may not be found");
        }
    }

    // Scan for available expansion profiles
    expansionRegistry_->initialize(tablesRoot);
    if (mpqData) {
        // The language the server answers in: the archives' locale folder.
        std::string archiveDir, locale;
        if (pipeline::MpqArchiveSet::findArchiveDir(dataPath, &archiveDir)) {
            pipeline::MpqArchiveSet::discoverArchives(archiveDir, locale);
        }
        if (!locale.empty()) expansionRegistry_->setClientLocale(locale);
    }

    // Say what is actually installed, before anything tries to use it.
    //
    // With nothing extracted the client used to start anyway, report no
    // expansions, fall back to a hardcoded default and fail somewhere far from
    // the cause - a missing texture, a model that would not load, a DBC lookup
    // returning nothing. None of it said there were no assets, and none of it
    // said where it had looked.
    assetInventory_ = pipeline::takeInventory(dataPath);
    if (mpqData) {
        // The archives are the whole install; the inventory describes
        // extractions and has nothing to say about them - nor has the login
        // screen, which reads its trouble text on its own.
        assetInventory_.fromArchives = true;
    } else if (assetInventory_.anyUsable()) {
        // At the level these logs carry, because this is the line that answers
        // the most common question anyone asks of them. One per installed set,
        // and the sets nobody has built are not mentioned at all.
        for (const pipeline::AssetSet& set : assetInventory_.sets) {
            if (set.usable() || set.anyAssets) LOG_WARNING("Assets: ", set.summary());
        }
    } else {
        LOG_WARNING(assetInventory_.troubleText());
#ifdef WOWEE_HAVE_ASSET_PANEL
        // Assigned rather than set through setState: several of the
        // subsystems its entry actions touch do not exist yet this early,
        // and setState returns early when the state is unchanged anyway.
        // The rest of initialize() still runs - it is all null-tolerant, and
        // the login screen is what comes after a build and a reopen.
        if (atStartup) {
            state = AppState::FIRST_RUN;
            LOG_WARNING("No assets found - opening the asset builder instead of the login screen");
        }
#endif
    }
    // Without the asset builder (iOS, which has no screen of its own in front
    // of the client the way Android has DataFolderActivity) the client asks
    // where the data is instead of opening a login nobody can get through.
    {
#ifdef WOWEE_HAVE_ASSET_PANEL
        // WOWEE_DIAG_NO_BUILDER=1: act as a build without the asset builder,
        // so a desktop shows the phones' data screen and goes through their
        // start path - the only way to run that path on a machine at hand.
        const bool builder = !core::envFlagEnabled("WOWEE_DIAG_NO_BUILDER", false);
#else
        constexpr bool builder = false;
#endif
        const std::string chosen = core::getCustomDataRoot();
        std::error_code chosenEc;
        const bool chosenPresent = !chosen.empty() && std::filesystem::is_directory(chosen, chosenEc);
        dataPrompt_ = core::dataPromptAtStartup(core::kPreLaunchDataMask, builder,
                                                mpqData || assetInventory_.anyUsable(), chosen,
                                                chosenPresent, streamChosen_ && !streamError_.empty());
        if (dataPrompt_ != core::DataPrompt::None && atStartup) {
            state = AppState::DATA_FOLDER;
            LOG_WARNING(dataPrompt_ == core::DataPrompt::StreamFailed
                            ? "Streaming failed - showing the data screen with what went wrong"
                            : "No game data found - asking where it is instead of opening the login screen");
        }
    }

    // Ask whether there is a newer WoWee, on a thread of its own. Started
    // here rather than earlier so a build that has just been told it has no
    // assets is not also waiting on a socket; it answers into the login
    // screen whenever it answers, and nothing waits for it.
    updateCheck_.start();

    // Load the tables this expansion's protocol is described by.
    if (gameHandler && expansionRegistry_) {
        if (auto* profile = expansionRegistry_->getActive()) {
            loadExpansionTables(*profile);
        }
    }

    // Try expansion-specific asset path first, fall back to base Data/
    std::string assetPath = dataPath;
    if (expansionRegistry_) {
        auto* profile = expansionRegistry_->getActive();
        if (profile && !profile->dataPath.empty()) {
            // Enable expansion-specific CSV DBC lookup (Data/expansions/<id>/db/*.csv).
            assetManager->setExpansionDataPath(profile->dataPath);

            std::string expansionManifest = profile->dataPath + "/manifest.json";
            if (!mpqData && std::filesystem::exists(expansionManifest)) {
                assetPath = profile->dataPath;
                LOG_INFO("Using expansion-specific asset path: ", assetPath);
                // Register base Data/ as fallback so world terrain files are
                // found even when the expansion path only contains overrides.
                // Named, so a base extracted from another client is refused
                // rather than quietly answering for this one.
                if (assetPath != dataPath) {
                    assetManager->setBaseFallbackPath(dataPath, profile->id);
                }
            }
        }
    }

    LOG_INFO("Attempting to load WoW assets from: ", assetPath);
    if (assetManager->initialize(assetPath)) {
        LOG_INFO("Asset manager initialized successfully");

        // Fonts, now the archives are open and still before any frame is
        // drawn - the only moment the glyph atlas can take another face
        // without being rebuilt.
        //
        // It used to run above this, which meant it could only ever see loose
        // files. An install that never extracted its data keeps every font
        // inside the MPQs, so the search found nothing and the client fell
        // back to ImGui's built-in face - the same build reading the game's
        // own fonts on one machine and not on another. assetPath, not the raw
        // dataPath: extract_assets.sh always writes with --expansion-subdir,
        // so fonts live under expansions/<id>/ alongside everything else.
        // assetPath first, then dataPath: an extraction made with
        // --expansion-subdir keeps its fonts under expansions/<id>/, and one
        // made without keeps them in the base Data. Both exist in the wild.
        if (uiManager) {
            uiManager->loadInterfaceFont(assetPath, assetManager.get());
            uiManager->loadInterfaceFont(dataPath, assetManager.get());
            uiManager->setCursorAssets(assetManager.get());
        }

        // Renderer creation precedes AssetManager creation, so DBC-driven
        // lighting must be initialized here rather than in Renderer::initialize.
        if (renderer && renderer->getLightingManager() &&
            !renderer->getLightingManager()->initialize(assetManager.get())) {
            LOG_WARNING("Lighting manager initialization failed; using fallback lighting");
        }

        // Eagerly load creature display DBC lookups so first spawn doesn't stall
        entitySpawner_ = std::make_unique<EntitySpawner>(
            renderer.get(), assetManager.get(), gameHandler.get(), &gameServices_);
        entitySpawner_->initialize();
        // The units' particle twins posed once the units are animated each
        // frame (Renderer::update), not before it.
        renderer->setParticleTwinPoser([this] {
            if (entitySpawner_) entitySpawner_->poseParticleTwins();
        });
        // A move into or out of a seat waits on the vehicle's animation
        // (0x0074c040).
        gameHandler->setVehicleMoveHoldCallback([this](uint64_t mover, uint64_t transport, uint8_t seat) {
            return entitySpawner_ && entitySpawner_->holdVehicleMove(mover, transport, seat);
        });
        gameHandler->setVehicleSeatMovedCallback([this](uint64_t guid) {
            if (entitySpawner_) entitySpawner_->vehicleSeatMoved(guid);
        });
        // The player's placed fall (0x006eccf0), into the camera controller.
        // The camera off a vehicle's frame as it goes (0x00604a70(0, 0)).
        gameHandler->setCameraTransportClearedCallback([this](uint64_t transportGuid) {
            if (entitySpawner_) entitySpawner_->localCameraLeavesTransport(transportGuid);
        });
        // Nothing moves the player in what is not loaded yet (0x007a55e0).
        if (renderer && renderer->getCameraController()) {
            renderer->getCameraController()->setObjectComing(
                [this](const glm::vec3& lo, const glm::vec3& hi, bool models) {
                    // Nor before it is in the world: the client's player is
                    // made by the realm's create, and what it stands on comes
                    // with that. Behind the loading screen it fell for as long
                    // as the create took.
                    if (gameHandler && gameHandler->getState() == game::WorldState::IN_WORLD &&
                        !gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid()))
                        return true;
                    return entitySpawner_ && entitySpawner_->objectComingIn(lo, hi, models);
                });
        }
        gameHandler->setPlacedFallCallback([this] {
            if (renderer && renderer->getCameraController()) renderer->getCameraController()->queuePlacedFall();
            ++playerSplineMoves_;
        });
        gameHandler->setFallEndsCallback([this] {
            ++playerSplineMoves_;
            if (renderer && renderer->getCameraController()) renderer->getCameraController()->endFall();
        });
        // SMSG_OVERRIDE_LIGHT into the lighting (0x007f10a0, 0x007f11a0).
        gameHandler->setOverrideLightCallback([this](uint32_t areaLight, uint32_t overrideLight, uint32_t ms) {
            if (renderer && renderer->getLightingManager())
                renderer->getLightingManager()->overrideLight(areaLight, overrideLight, ms);
        });

        appearanceComposer_ = std::make_unique<AppearanceComposer>(
            renderer.get(), assetManager.get(), gameHandler.get(),
            entitySpawner_.get());

        // Wire AppearanceComposer to UI components (Phase A singleton breaking)
        if (uiManager) {
            uiManager->setAppearanceComposer(appearanceComposer_.get());
            
            // Wire all services to UI components (Phase B singleton breaking)
            ui::UIServices uiServices;
            uiServices.window = window.get();
            uiServices.renderer = renderer.get();
            uiServices.assetManager = assetManager.get();
            uiServices.gameHandler = gameHandler.get();
            uiServices.expansionRegistry = expansionRegistry_.get();
            uiServices.addonManager = addonManager_.get();  // May be nullptr here, re-wire later
            uiServices.audioCoordinator = audioCoordinator_.get();
            uiServices.entitySpawner = entitySpawner_.get();
            uiServices.appearanceComposer = appearanceComposer_.get();
            uiServices.worldLoader = worldLoader_.get();
            uiManager->setServices(uiServices);
        }

        // Ensure the main in-world CharacterRenderer can load textures immediately.
        // Previously this was only wired during terrain initialization, which meant early spawns
        // (before terrain load) would render with white fallback textures (notably hair).
        if (renderer && renderer->getCharacterRenderer()) {
            renderer->getCharacterRenderer()->setAssetManager(assetManager.get());
        }

        // Load transport paths from TransportAnimation.dbc and TaxiPathNode.dbc
        if (gameHandler && gameHandler->getTransportManager()) {
            gameHandler->getTransportManager()->loadTransportAnimationDBC(assetManager.get());
            gameHandler->getTransportManager()->loadTransportRotationDBC(assetManager.get());
            gameHandler->getTransportManager()->loadTaxiPathNodeDBC(assetManager.get());
            gameHandler->getTransportManager()->loadTransportPhysicsDBC(assetManager.get());
        }

        // Initialize addon system
        addonManager_ = std::make_unique<addons::AddonManager>();
        // Bindings ask the interface whether someone is typing before they
        // fire. Set here rather than at the call sites because every panel
        // polls its own key from inside its own draw - there are many askers
        // and one answer.
        ui::setTypedInputProbe(
            [this]() -> bool {
                if (!addonManager_ || !addonsLoaded_) return false;
                auto* engine = addonManager_->getLuaEngine();
                return engine != nullptr && engine->editBoxHasFocus();
            },
            [this]() -> bool {
                if (!addonManager_ || !addonsLoaded_) return false;
                auto* engine = addonManager_->getLuaEngine();
                return engine != nullptr && engine->editBoxLeavesArrows((SDL_GetModState() & SDL_KMOD_ALT) != 0);
            });
        addons::LuaServices luaSvc;
        luaSvc.window            = window.get();
        luaSvc.audioCoordinator  = audioCoordinator_.get();
        luaSvc.expansionRegistry = expansionRegistry_.get();
        luaSvc.requestReloadUI = [this]() { reloadUiPending_ = true; };
        luaSvc.openSettings = [uim = uiManager.get()](const std::string& tab) {
            if (uim) uim->getGameScreen().openSettings(tab.empty() ? nullptr : tab.c_str());
        };
        // FrameXML's options panels, driving this client's own settings.
        //
        // One table rather than a hook per setting. Every row is a CVar the
        // Blizzard panels already have a control for, and a field this client
        // already had - they simply had never been introduced.
        luaSvc.getClientSetting = [uim = uiManager.get()](const std::string& key) {
            return uim ? uim->getGameScreen().getSettingsPanel().settingValue(key) : std::string{};
        };
        luaSvc.quitApplication = [this]() {
            if (!mcp::gate::quitAllowed()) return;
            LOG_INFO("Exit Game with no session to leave - closing");
            if (window) window->setShouldClose(true);
        };
        luaSvc.setActionBarPage = [uim = uiManager.get()](int page) {
            if (uim) uim->getGameScreen().actionBarPanel().setMainActionBarPage(page);
        };
        luaSvc.setZoneMusicNoDelay = [this](bool noDelay) {
            if (audioCoordinator_) audioCoordinator_->setZoneMusicNoDelay(noDelay);
        };
        luaSvc.setGroundDetailDistance = [this](float yards) {
            if (!renderer) return;
            renderer->setGroundClutterDistance(yards);
        };
        luaSvc.setAnisotropyLimit = [this](float limit) {
            if (auto* window = this->window.get()) {
                if (auto* ctx = window->getVkContext()) ctx->setAnisotropyLimit(limit);
            }
        };
        luaSvc.setEnvironmentDetail = [this](float detail) {
            if (!renderer) return;
            if (auto* m2 = renderer->getM2Renderer()) m2->setEnvironmentDetail(detail);
        };
        luaSvc.setParticleDensity = [this](float density) {
            if (!renderer) return;
            if (auto* m2 = renderer->getM2Renderer()) m2->setParticleDensity(density);
        };
        luaSvc.setWeatherDensity = [this](float level) {
            if (!renderer) return;
            if (auto* weather = renderer->getWeather())
                weather->setDensityLevel(static_cast<int>(std::lround(level)));
        };
        luaSvc.settingUnavailable = [this](const std::string& key) -> std::string {
            if (!renderer || !ui::settingNeedsRayTracing(key)) return {};
            const auto* profile = expansionRegistry_ ? expansionRegistry_->getActive() : nullptr;
            return renderer->rtUnavailableReason(profile && ui::settingsLocaleGerman(profile->locale));
        };
        luaSvc.setClientSetting = [uim = uiManager.get()](const std::string& key,
                                                          const std::string& value) {
            if (!uim) return;
            auto& gs = uim->getGameScreen();
            auto& panel = gs.getSettingsPanel();
            const std::string before = panel.settingValue(key);
            if (!panel.setSettingValue(key, value)) return;
            // The stored CVars are put back at every entry: written then only
            // where one differs, not once for each.
            if (!addons::replayingStoredCVars() || panel.settingValue(key) != before) gs.saveSettingsSoon();
        };

        // Every schema key must be answered, or its control is drawn and inert.
        //
        // The schema names the settings, the panel answers them, and the two are
        // in different files - so a setting added to one and not the other gives
        // a checkbox that does nothing, with nothing to say why. Checked once at
        // startup rather than left to be noticed.
        {
            std::size_t count = 0;
            const auto* schema = ui::clientSettingsSchema(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (schema[i].store[0] != '\0') continue;  // answered by the game's own store
                if (luaSvc.getClientSetting(schema[i].key).empty()) {
                    LOG_WARNING("Setting '", schema[i].key, "' (", schema[i].label,
                                ") is in the schema but nothing answers it - its "
                                "control in the interface options will do nothing");
                }
            }
        }

        // FrameXML's Sound options, driving this client's own audio settings.
        // Its sliders are 0..1 and these are percentages, so the conversion
        // happens here rather than in the binding.
        luaSvc.getAudioSetting = [uim = uiManager.get()](const std::string& key) -> float {
            if (!uim) return 0.0f;
            auto& sp = uim->getGameScreen().getSettingsPanel();
            if (key == "master")  return static_cast<float>(sp.pendingMasterVolume) / 100.0f;
            if (key == "music")   return static_cast<float>(sp.pendingMusicVolume) / 100.0f;
            if (key == "ambient") return static_cast<float>(sp.pendingAmbientVolume) / 100.0f;
            if (key == "enableall") return sp.soundMuted_ ? 0.0f : 1.0f;
            return 0.0f;
        };
        luaSvc.setAudioSetting = [uim = uiManager.get(), this](const std::string& key, float v) {
            if (!uim) return;
            auto& gs = uim->getGameScreen();
            auto& sp = gs.getSettingsPanel();
            const int pct = static_cast<int>(std::lround(v * 100.0f));
            const auto values = [&sp] {
                return std::tuple(sp.pendingMasterVolume, sp.pendingMusicVolume, sp.pendingAmbientVolume, sp.soundMuted_);
            };
            const auto before = values();
            if (key == "master") {
                sp.pendingMasterVolume = pct;
                // Raising it means sound is wanted, the same as the slider in
                // the client's own panel does.
                if (pct > 0) sp.soundMuted_ = false;
            } else if (key == "music")   { sp.pendingMusicVolume = pct;
            } else if (key == "ambient") { sp.pendingAmbientVolume = pct;
            } else if (key == "enableall") { sp.soundMuted_ = (v <= 0.0f);
            } else { return; }
            sp.applyAudioVolumes(audioCoordinator_.get());
            if (!addons::replayingStoredCVars() || values() != before) gs.saveSettingsSoon();
        };
        luaSvc.reapplyAudioVolumes = [uim = uiManager.get(), this]() {
            if (!uim) return;
            uim->getGameScreen().getSettingsPanel().applyAudioVolumes(audioCoordinator_.get());
        };
        luaSvc.runMacroText = [uim = uiManager.get(), gh = gameHandler.get(), this](const std::string& body) {
            // The interface's own edit box runs it; the chat panel's commands
            // where there is none.
            if (addonManager_ && addonsLoaded_ && addonManager_->getLuaEngine()->runMacro(body)) return;
            if (uim && gh) uim->getGameScreen().getChatPanel().executeMacroText(*gh, body);
        };
        luaSvc.clientChatCommandNames = [uim = uiManager.get()]() {
            std::vector<std::string> out;
            if (uim) out = uim->getGameScreen().getChatPanel().registryCommandNames();
            return out;
        };
        luaSvc.runClientChatCommand = [uim = uiManager.get(), gh = gameHandler.get()](
                const std::string& alias, const std::string& args) -> bool {
            if (!uim || !gh) return false;
            return uim->getGameScreen().getChatPanel().runRegistryCommand(*gh, alias, args);
        };
        luaSvc.getGamma = [uim = uiManager.get()]() -> float {
            return uim ? uim->getGameScreen().getGamma() : 1.0f;
        };
        luaSvc.setGamma = [uim = uiManager.get()](float g) {
            if (uim) uim->getGameScreen().setGamma(g);
        };
        // The world map's own data, for the interface's map. The client keeps
        // the zones, their overlays and the area POIs and drew them only
        // itself; FrameXML's map has readers for exactly these and had nothing
        // behind them.
        luaSvc.getMapFileName = [r = renderer.get()]() -> std::string {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->currentMapFolder() : std::string();
        };
        luaSvc.getMapOverlays = [this, r = renderer.get()]() {
            std::vector<addons::LuaServices::MapOverlay> out;
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap) return out;
            if (gameHandler)
                wmap->setServerExplorationMask(gameHandler->getPlayerExploredZoneMasks(),
                                               gameHandler->hasPlayerExploredZoneMasks());
            const std::string folder = wmap->currentMapFolder();
            for (const auto& o : wmap->currentOverlays()) {
                addons::LuaServices::MapOverlay m;
                // The whole path up to the tile number, because that is what
                // the interface does with it: WorldMapFrame_Update appends the
                // index and nothing else, so a bare DBC name asks for
                // "CHILLWINDPOINT1" and finds nothing.
                m.texture = folder.empty()
                                ? o.textureName
                                : "Interface\\WorldMap\\" + folder + "\\" + o.textureName;
                m.width = o.texWidth;
                m.height = o.texHeight;
                m.offsetX = o.offsetX;
                m.offsetY = o.offsetY;
                out.push_back(std::move(m));
            }
            return out;
        };
        luaSvc.getMapLandmarks = [this, r = renderer.get()]() {
            std::vector<addons::LuaServices::MapLandmark> out;
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap) return out;
            // Shown or not: what has been explored was the map's only once it
            // had been drawn.
            if (gameHandler)
                wmap->setServerExplorationMask(gameHandler->getPlayerExploredZoneMasks(),
                                               gameHandler->hasPlayerExploredZoneMasks());
            const auto worldState = [this](uint32_t id) -> uint32_t {
                if (!gameHandler) return 0u;
                const auto& states = gameHandler->getWorldStates();
                const auto it = states.find(id);
                return it != states.end() ? it->second : 0u;
            };
            for (const auto& l : wmap->currentLandmarks(worldState)) {
                addons::LuaServices::MapLandmark m;
                m.name = l.name;
                m.description = l.description;
                m.icon = static_cast<int>(l.icon);
                m.x = l.x;
                m.y = l.y;
                m.flags = l.flags;
                out.push_back(std::move(m));
            }
            // The gossip point after them (0x00546310 -> 0x007f3be0), with
            // its icon whatever the map.
            if (const auto* g = gameHandler ? gameHandler->gossipPoint() : nullptr) {
                const glm::vec3 wire = core::coords::canonicalToServer(glm::vec3(g->x, g->y, 0.0f));
                if (const auto uv = wmap->mapPointOf(gameHandler->getCurrentMapId(), wire.x, wire.y)) {
                    addons::LuaServices::MapLandmark m;
                    m.name = g->name;
                    m.icon = static_cast<int>(g->icon);
                    m.x = uv->x;
                    m.y = uv->y;
                    m.flags = g->flags;
                    out.push_back(std::move(m));
                }
            }
            return out;
        };
        luaSvc.getMapZoneNameAt = [r = renderer.get()](float u, float v) -> std::string {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->zoneNameAtMapPoint(u, v) : std::string();
        };
        luaSvc.getMapHighlightAt = [r = renderer.get()](float u, float v) {
            addons::LuaServices::MapHighlightAnswer a;
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap) return a;
            const auto h = wmap->mapHighlightAt(u, v);
            a.name = h.name;
            a.fileName = h.fileName;
            a.texCoordX = h.texCoordX; a.texCoordY = h.texCoordY;
            a.textureX = h.textureX;   a.textureY = h.textureY;
            a.scrollX = h.scrollX;     a.scrollY = h.scrollY;
            return a;
        };
        luaSvc.clickMapPoint = [r = renderer.get()](float u, float v) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->clickMapPoint(u, v) : false;
        };
        luaSvc.mapUVForWorldPos = [this, r = renderer.get()](float x, float y, float z, float& u, float& v) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap || !gameHandler) return false;
            const glm::vec3 wire = core::coords::canonicalToServer(glm::vec3(x, y, z));
            const auto uv = wmap->mapPointOf(gameHandler->getCurrentMapId(), wire.x, wire.y, wire.z);
            if (!uv) return false;
            u = uv->x;
            v = uv->y;
            return true;
        };
        luaSvc.mapUVForPlace = [r = renderer.get()](uint32_t mapId, float x, float y, float z, float& u, float& v) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap) return false;
            const glm::vec3 wire = core::coords::canonicalToServer(glm::vec3(x, y, z));
            const auto uv = wmap->mapPointOf(mapId, wire.x, wire.y, wire.z);
            if (!uv) return false;
            u = uv->x;
            v = uv->y;
            return true;
        };
        luaSvc.mapUVForShownPos = [r = renderer.get()](float x, float y, float z, float& u, float& v) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->mapUVForCanonical(x, y, z, u, v) : false;
        };
        luaSvc.getMapPoiFrame = [r = renderer.get()]() -> addons::LuaServices::MapPoiFrame {
            addons::LuaServices::MapPoiFrame out;
            auto* wmap = r ? r->getWorldMap() : nullptr;
            if (!wmap) return out;
            const auto frame = wmap->currentPoiFrame();
            out.valid = frame.valid;
            out.zoneLevel = frame.zoneLevel;
            out.dungeon = frame.dungeon;
            out.worldMapAreaId = frame.worldMapAreaId;
            out.mapId = frame.mapId;
            out.dungeonFloor = frame.dungeonFloor;
            out.locLeft = frame.locLeft;
            out.locRight = frame.locRight;
            return out;
        };
        luaSvc.zoomMapOut = [r = renderer.get()]() {
            if (auto* wmap = r ? r->getWorldMap() : nullptr) wmap->zoomOutOneLevel();
        };
        luaSvc.getMapContinentNames = [r = renderer.get()]() -> std::vector<std::string> {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->continentNames() : std::vector<std::string>();
        };
        luaSvc.getMapZoneNames = [r = renderer.get()](int continent) -> std::vector<std::string> {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->zoneNames(continent) : std::vector<std::string>();
        };
        luaSvc.setMapByIndex = [r = renderer.get()](int continent, int zone) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap && wmap->showMap(continent, zone);
        };
        luaSvc.getMapContinentIndex = [r = renderer.get()]() -> int {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->currentContinentIndex() : 0;
        };
        luaSvc.getMapZoneIndex = [r = renderer.get()]() -> int {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->currentZoneIndex() : 0;
        };
        luaSvc.canZoomMapOut = [r = renderer.get()]() -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap && wmap->canZoomOut();
        };
        // At once, shown or not (0x00547170): an addon calls this and reads
        // GetPlayerMapPosition on the next line.
        luaSvc.showPlayerMapZone = [r = renderer.get()]() {
            if (!r) return;
            r->syncWorldMapToPlayer();
            // SetMapToCurrentZone says so itself.
            if (auto* wmap = r->getWorldMap()) (void)wmap->takeViewChanged();
        };
        luaSvc.getMapWorldAreaId = [r = renderer.get()]() -> uint32_t {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->currentWorldMapAreaId() : 0u;
        };
        luaSvc.setMapWorldAreaId = [r = renderer.get()](uint32_t id) {
            if (auto* wmap = r ? r->getWorldMap() : nullptr) wmap->showWorldMapArea(id);
        };
        luaSvc.getMapDungeonLevelCount = [r = renderer.get()]() -> int {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->dungeonLevelCount() : 0;
        };
        luaSvc.getMapDungeonLevel = [r = renderer.get()]() -> int {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap ? wmap->currentDungeonLevel() : 0;
        };
        luaSvc.setMapDungeonLevel = [r = renderer.get()](int level) -> bool {
            auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap && wmap->selectDungeonLevel(level);
        };
        luaSvc.mapDungeonUsesTerrainMap = [r = renderer.get()]() -> bool {
            const auto* wmap = r ? r->getWorldMap() : nullptr;
            return wmap && wmap->dungeonUsesTerrainMap();
        };
        luaSvc.getLiveZoneId = [r = renderer.get()]() -> uint32_t {
            return r ? r->getCurrentZoneId() : 0u;
        };
        luaSvc.getZoneTexts = [r = renderer.get()]() {
            addons::LuaServices::ZoneTexts out;
            if (const auto* t = r ? r->getZoneTexts() : nullptr) {
                out.known = true;
                out.zoneId = t->zoneId;
                out.subzoneId = t->subzoneId;
                out.zone = t->zone;
                out.realZone = t->realZone;
                out.subzone = t->subzone;
                out.minimap = t->minimap;
            }
            return out;
        };
        luaSvc.isOnOutdoorPvpObjective = [r = renderer.get()]() -> bool {
            return r && r->isOnOutdoorPvpObjective();
        };
        luaSvc.takeScreenshot = [uim = uiManager.get()]() {
            if (uim) uim->getGameScreen().takeScreenshot();
        };
        luaSvc.toggleAutoRun = [r = renderer.get()]() {
            if (auto* cc = r ? r->getCameraController() : nullptr) cc->toggleAutoRun();
        };
        luaSvc.pressJump = [r = renderer.get()]() {
            if (auto* cc = r ? r->getCameraController() : nullptr) cc->pressBindingJump();
        };
        luaSvc.setBindingControl = [r = renderer.get()](uint32_t bit, bool held) {
            if (auto* cc = r ? r->getCameraController() : nullptr) cc->setBindingControl(bit, held);
        };
        luaSvc.cameraZoom = [r = renderer.get()](float yards) {
            if (auto* cc = r ? r->getCameraController() : nullptr) cc->zoomStep(yards);
        };
        luaSvc.getCameraController = [r = renderer.get()]() -> rendering::CameraController* {
            return r ? r->getCameraController() : nullptr;
        };
        luaSvc.getCamera = [r = renderer.get()]() -> const rendering::Camera* {
            return r ? r->getCamera() : nullptr;
        };
        luaSvc.interactWithTarget = [uim = uiManager.get(), gh = gameHandler.get()]() {
            if (uim && gh) uim->getGameScreen().interactWithTarget(*gh);
        };
        luaSvc.toggleSheath = [this]() {
            if (appearanceComposer_) appearanceComposer_->toggleSheath();
        };
        luaSvc.setBindingTurn = [r = renderer.get()](int direction, bool held) {
            auto* cc = r ? r->getCameraController() : nullptr;
            if (!cc) return;
            if (direction < 0) cc->setBindingTurnLeft(held);
            else cc->setBindingTurnRight(held);
        };
        luaSvc.getBarberStyleInfo = [uim = uiManager.get(), gh = gameHandler.get()](
                int selector, std::string& name, std::string& description, uint32_t& cost,
                bool& isCurrent) -> bool {
            if (!uim || !gh) return false;
            ui::WindowManager::BarberStyleInfo info;
            if (!uim->getGameScreen().windowManager().barberStyleInfo(*gh, selector, info)) return false;
            name = std::move(info.name);
            description = std::move(info.description);
            cost = info.cost;
            isCurrent = info.isCurrent;
            return true;
        };
        luaSvc.setNextBarberStyle = [uim = uiManager.get(), gh = gameHandler.get()](
                int selector, int direction) {
            if (uim && gh) {
                uim->getGameScreen().windowManager().barberCycleStyle(*gh, selector, direction);
            }
        };
        luaSvc.getBarberTotalCost = [uim = uiManager.get(), gh = gameHandler.get()]() -> uint32_t {
            if (!uim || !gh) return 0;
            return uim->getGameScreen().windowManager().barberTotalCostCopper(*gh);
        };
        luaSvc.barberCanAlterSkin = [uim = uiManager.get(), gh = gameHandler.get()]() -> bool {
            return uim && gh && uim->getGameScreen().windowManager().barberCanAlterSkin(*gh);
        };
        luaSvc.barberReset = [uim = uiManager.get(), gh = gameHandler.get()]() {
            if (uim && gh) uim->getGameScreen().windowManager().barberResetSelections(*gh);
        };
        luaSvc.barberApply = [uim = uiManager.get(), gh = gameHandler.get()]() {
            if (uim && gh) uim->getGameScreen().windowManager().barberApplySelection(*gh);
        };
        luaSvc.isPlayerIndoors = [r = renderer.get()]() -> bool {
            return r && r->isPlayerIndoors();
        };
        luaSvc.getFullscreen = [uim = uiManager.get()]() -> bool {
            return uim ? uim->getGameScreen().getFullscreen() : false;
        };
        luaSvc.setFullscreen = [uim = uiManager.get()](bool on) {
            if (uim) uim->getGameScreen().setFullscreen(on);
        };
        luaSvc.getResolutionIndex = [uim = uiManager.get()]() -> int {
            return uim ? uim->getGameScreen().getResolutionIndex() : 0;
        };
        luaSvc.getAntiAliasingIndex = [uim = uiManager.get()]() -> int {
            return uim ? uim->getGameScreen().getAntiAliasingIndex() : 0;
        };
        luaSvc.setAntiAliasingIndex = [uim = uiManager.get()](int i) {
            if (uim) uim->getGameScreen().setAntiAliasingIndex(i);
        };
        luaSvc.setResolutionIndex = [uim = uiManager.get()](int i) {
            if (uim) uim->getGameScreen().setResolutionIndex(i);
        };
        // Gathered once, on the first ask rather than at startup: it is a walk
        // of the whole manifest, and most sessions never open an icon picker.
        luaSvc.listIconTextures = [am = assetManager.get()]() -> const std::vector<std::string>& {
            static std::vector<std::string> icons;
            static bool built = false;
            if (built) return icons;
            built = true;
            if (!am) return icons;
            // Manifest keys are normalised: lower case, backslashes.
            static const std::string kPrefix = "interface\\icons\\";
            // From the manifest or the archives' listfiles, whichever is the
            // source.
            // The file name with its extension: the client's lists take only
            // a .blp or a .tga (0x00565550), and drop the extension themselves.
            for (const std::string& path : am->listFiles(kPrefix)) {
                const std::string name = path.substr(kPrefix.size());
                if (!name.empty() && name.find('\\') == std::string::npos) icons.push_back(name);
            }
            // A grid the player scrolls through, so the order has to be stable
            // between one frame and the next - the manifest is a hash map and
            // is not.
            std::sort(icons.begin(), icons.end());
            LOG_INFO("Icon picker: ", icons.size(), " icons from the manifest");
            return icons;
        };
        // The widget renderer needs the asset manager for Interface\ art and the
        // device to upload it; both exist by now.
        widgetRenderer_.initialize(assetManager.get(),
                                   window ? window->getVkContext() : nullptr);
        widgetRenderer_.setQuestAreaDrawer([this](ImDrawList* list) {
            if (auto* map = renderer ? renderer->getWorldMap() : nullptr) map->drawQuestAreas(list);
        });
        if (addonManager_->initialize(gameHandler.get(), luaSvc)) {
            // FrameXML and the AddOns folder are loose directories on disk, not
            // assets reached through the manifest, and they belong to the
            // installation rather than to whichever expansion happens to supply
            // asset overrides. assetPath is the expansion's directory whenever
            // one carries its own manifest - and an overlay holding models and
            // a DBC has no interface/ at all, so taking it here meant the whole
            // Blizzard interface stopped loading the moment any overlay
            // existed, with one warning to say so.
            //
            // The expansion still wins if it does ship an interface, which is
            // what an expansion-specific FrameXML would be for.
            std::string interfaceRoot = dataPath;
            if (assetManager->isMpqMode()) {
                // The interface's scripts are inside the archives; the addon
                // manager reads a folder. They are written once into
                // cache/interface_mpq (the Lua, XML and TOC files of FrameXML,
                // GlueXML and the Blizzard addons) and read from there.
                interfaceRoot = writeInterfaceFromArchives(*assetManager);
            } else if (assetPath != dataPath) {
                std::error_code ec;
                for (const char* cased : {"/interface", "/Interface"}) {
                    if (std::filesystem::is_directory(assetPath + cased, ec)) {
                        interfaceRoot = assetPath;
                        break;
                    }
                }
            }
            std::string addonsDir = interfaceRoot + "/interface/AddOns";
            addonManager_->setFrameXmlDir(interfaceRoot + "/interface/FrameXML");
            addonManager_->scanAddons(addonsDir);
            // What CMSG_AUTH_SESSION says this client has, and what is done
            // with the server's answer (D5: 0x005f5440, 0x005f79a0).
            if (gameHandler) {
                gameHandler->setAuthAddons(addonManager_->authSessionAddons());
                gameHandler->setAddonInfoCallback([this] {
                    if (addonManager_) addonManager_->saveAddonUrls();
                });
            }
            // Wire Lua errors to UI error display
            addonManager_->getLuaEngine()->setLuaErrorCallback([gh = gameHandler.get()](const std::string& err) {
                // Not addUIError: that reports by firing an event, which runs
                // script, which is how a script error came to report itself.
                if (gh) gh->addScriptError(err);
            });
            // The game-menu button opens this client's settings panel.
            addonManager_->getLuaEngine()->setOpenSettingsCallback([this] {
                if (auto* uim = uiManager.get()) uim->getGameScreen().openSettings();
            });
            // No second announcement of a chat message.
            //
            // This registered a callback that fired CHAT_MSG_* itself, off its
            // own hand-written table of chat types, with two arguments -
            // message and sender. The chat handler already announces every
            // message with the twelve the interface reads, so each line went out
            // twice: once complete, and once so short that
            // ChatFrame_MessageEventHandler raised on strlen(arg4) before it
            // drew anything. That raise is the one that filled the log.
            //
            // The table was the other half of it: fifty chat types have names
            // and this knew thirty-two, so the eighteen it had never heard of -
            // money, experience, honour, reputation, the channel notices -
            // produced no event here at all.
            //
            // What it did better, the channel's index, moved into the handler's
            // own firing.
            gameHandler->setUnitTooltipCallback([this](uint64_t guid) {
                if (addonManager_ && addonsLoaded_)
                    if (auto* engine = addonManager_->getLuaEngine()) engine->unitTooltipChanged(guid);
            });
            // Wire generic game events to addon dispatch
            gameHandler->setAddonEventCallback([this](const std::string& event, const std::vector<std::string>& args) {
                if (addonManager_ && addonsLoaded_) {
                    addonManager_->fireEvent(event, args);
                }
            });
            gameHandler->setCombatLogEventCallback(
                [this](const std::vector<game::combat_log::Arg>& args, bool filtered) {
                    if (addonManager_ && addonsLoaded_) addonManager_->fireCombatLogEvent(args, filtered);
                });
            // The same route for an event whose arguments are not all strings.
            // COMBAT_TEXT_UPDATE is the one that needs it: its payload is
            // compared (`arg3 > 0`), and a numeric string there does not
            // compare, it raises.
            gameHandler->setAddonEventValuesCallback(
                [this](const std::string& event, const std::vector<game::combat_log::Arg>& args) {
                    if (addonManager_ && addonsLoaded_) addonManager_->fireEventValues(event, args);
                });
            // How a keybinding reaches the frame that replaced this client's own
            // window. Without it, a key toggled a window nothing draws.
            gameHandler->setInterfaceCommandCallback([this](const std::string& lua) {
                if (addonManager_ && addonsLoaded_) {
                    addonManager_->runInterfaceCommand(lua);
                }
            });
            // The same route, for a question rather than an instruction.
            gameHandler->setInterfaceQueryCallback([this](const std::string& expr) {
                if (!addonManager_ || !addonsLoaded_) return false;
                return addonManager_->interfaceCommandBoolean(expr);
            });
            // Wire spell icon path resolver for Lua API (GetSpellInfo, UnitBuff icon, etc.)
            {
                auto spellIconPaths  = std::make_shared<std::unordered_map<uint32_t, std::string>>();
                auto spellIconIds    = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
                auto loaded          = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                // Shared, because two resolvers now read these maps and either
                // may be asked first. Left inside the spell resolver, a
                // spellbook tab asking for an icon id before any spell had
                // asked for one would have found the table empty and answered
                // nothing, which is the sort of thing that only shows up when
                // the tabs happen to be built first.
                auto ensureLoaded = std::make_shared<std::function<void()>>(
                    [spellIconPaths, spellIconIds, loaded, am]() {
                    if (!am || !am->isInitialized() || *loaded) return;
                    // Initialised, not merely present: loadDBC answers nullptr
                    // before the assets are up, and latching on that left both
                    // tables empty for the rest of the session.
                    *loaded = true;
                    pipeline::loadSpellIconPaths(am, *spellIconPaths);
                    auto spellDbc = am->loadDBC("Spell.dbc");
                    const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
                    if (spellDbc && spellDbc->isLoaded()) {
                        uint32_t fieldCount = spellDbc->getFieldCount();
                        uint32_t iconField = 133; // WotLK default
                        uint32_t idField = 0;
                        if (spellL) {
                            try {
                                uint32_t layoutId = (*spellL)["ID"];
                                uint32_t layoutIcon = (*spellL)["IconID"];
                                if (layoutId < fieldCount && layoutIcon < fieldCount) {
                                    iconField = layoutIcon;
                                    idField = layoutId;
                                }
                            } catch (...) {}
                        }
                        for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                            uint32_t id = spellDbc->getUInt32(i, idField);
                            uint32_t iconId = spellDbc->getUInt32(i, iconField);
                            if (id > 0 && iconId > 0) (*spellIconIds)[id] = iconId;
                        }
                    }
                });

                gameHandler->setSpellIconPathResolver(
                    [spellIconPaths, spellIconIds, ensureLoaded](uint32_t spellId) -> std::string {
                    (*ensureLoaded)();
                    auto iit = spellIconIds->find(spellId);
                    if (iit == spellIconIds->end()) return {};
                    auto pit = spellIconPaths->find(iit->second);
                    if (pit == spellIconPaths->end()) return {};
                    return pit->second;
                });

                // Straight to the artwork, for callers that already hold an
                // icon id rather than a spell - SkillLine.dbc names one for
                // each spellbook tab.
                gameHandler->setIconPathResolver(
                    [spellIconPaths, ensureLoaded](uint32_t iconId) -> std::string {
                    if (iconId == 0) return {};
                    (*ensureLoaded)();
                    auto pit = spellIconPaths->find(iconId);
                    return pit == spellIconPaths->end() ? std::string{} : pit->second;
                });
            }
            // Wire item icon path resolver: displayInfoId -> "Interface\\Icons\\INV_..."
            {
                auto iconNames = std::make_shared<std::unordered_map<uint32_t, std::string>>();
                auto loaded    = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                gameHandler->setItemIconPathResolver([iconNames, loaded, am](uint32_t displayInfoId) -> std::string {
                    if (!am || displayInfoId == 0) return {};
                    if (!*loaded && am->isInitialized()) {
                        // Initialised, not merely present: loadDBC answers
                        // nullptr before the assets are up, and latching on
                        // that left this table empty for the session.
                        *loaded = true;
                        auto dbc = am->loadDBC("ItemDisplayInfo.dbc");
                        const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                        if (dbc && dbc->isLoaded()) {
                            uint32_t iconField = dispL ? (*dispL)["InventoryIcon"] : 5;
                            for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
                                uint32_t id = dbc->getUInt32(i, 0); // field 0 = ID
                                std::string name = dbc->getString(i, iconField);
                                if (id > 0 && !name.empty()) (*iconNames)[id] = name;
                            }
                            LOG_INFO("Loaded ", iconNames->size(), " item icon names from ItemDisplayInfo.dbc");
                        }
                    }
                    auto it = iconNames->find(displayInfoId);
                    if (it == iconNames->end()) return {};
                    return "Interface\\Icons\\" + it->second;
                });
            }
            // Wire spell data resolver: spellId -> {castTimeMs, minRange, maxRange}
            {
                auto castTimeMap = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
                auto rangeMap    = std::make_shared<std::unordered_map<uint32_t, std::pair<float,float>>>();
                auto spellCastIdx = std::make_shared<std::unordered_map<uint32_t, uint32_t>>(); // spellId→castTimeIdx
                auto spellRangeIdx = std::make_shared<std::unordered_map<uint32_t, uint32_t>>(); // spellId→rangeIdx
                struct SpellCostEntry { uint32_t manaCost = 0; uint8_t powerType = 0; uint32_t perSecond = 0; };
                auto spellCostMap = std::make_shared<std::unordered_map<uint32_t, SpellCostEntry>>();
                auto loaded = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                gameHandler->setSpellDataResolver([castTimeMap, rangeMap, spellCastIdx, spellRangeIdx, spellCostMap, loaded, am](uint32_t spellId) -> game::GameHandler::SpellDataInfo {
                    if (!am) return {};
                    if (!*loaded && am->isInitialized()) {
                        // Initialised, not merely present: loadDBC answers
                        // nullptr before the assets are up, and latching on
                        // that left this table empty for the session.
                        *loaded = true;
                        // Load SpellCastTimes.dbc
                        auto ctDbc = am->loadDBC("SpellCastTimes.dbc");
                        if (ctDbc && ctDbc->isLoaded()) {
                            for (uint32_t i = 0; i < ctDbc->getRecordCount(); ++i) {
                                uint32_t id = ctDbc->getUInt32(i, 0);
                                int32_t base = static_cast<int32_t>(ctDbc->getUInt32(i, 1));
                                if (id > 0 && base > 0) (*castTimeMap)[id] = static_cast<uint32_t>(base);
                            }
                        }
                        // Load SpellRange.dbc
                        const auto* srL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellRange") : nullptr;
                        uint32_t minRField = srL ? (*srL)["MinRange"] : 1;
                        uint32_t maxRField = srL ? (*srL)["MaxRange"] : 4;
                        auto rDbc = am->loadDBC("SpellRange.dbc");
                        if (rDbc && rDbc->isLoaded()) {
                            for (uint32_t i = 0; i < rDbc->getRecordCount(); ++i) {
                                uint32_t id = rDbc->getUInt32(i, 0);
                                float minR = rDbc->getFloat(i, minRField);
                                float maxR = rDbc->getFloat(i, maxRField);
                                if (id > 0) (*rangeMap)[id] = {minR, maxR};
                            }
                        }
                        // Load Spell.dbc: extract castTimeIndex and rangeIndex per spell
                        auto sDbc = am->loadDBC("Spell.dbc");
                        const auto* spL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
                        if (sDbc && sDbc->isLoaded()) {
                            uint32_t idF = spL ? (*spL)["ID"] : 0;
                            // From the file's shape - Classic and Turtle named
                            // this 15, which is RequiresSpellFocus and zero for
                            // every spell.
                            uint32_t ctF = pipeline::detectSpellTimingFields(sDbc.get(), spL)
                                               .castingTimeIndex;
                            uint32_t rF  = spL ? (*spL)["RangeIndex"] : 132;
                            uint32_t ptF = UINT32_MAX, mcF = UINT32_MAX;
                            if (spL) {
                                try { ptF = (*spL)["PowerType"]; } catch (...) {}
                                try { mcF = (*spL)["ManaCost"]; } catch (...) {}
                            }
                            const uint32_t psF = spL ? spL->tryField("ManaPerSecond") : UINT32_MAX;
                            uint32_t fc = sDbc->getFieldCount();
                            for (uint32_t i = 0; i < sDbc->getRecordCount(); ++i) {
                                uint32_t id = sDbc->getUInt32(i, idF);
                                if (id == 0) continue;
                                uint32_t ct = sDbc->getUInt32(i, ctF);
                                uint32_t ri = sDbc->getUInt32(i, rF);
                                if (ct > 0) (*spellCastIdx)[id] = ct;
                                if (ri > 0) (*spellRangeIdx)[id] = ri;
                                // Extract power cost
                                uint32_t mc = (mcF < fc) ? sDbc->getUInt32(i, mcF) : 0;
                                uint8_t  pt = (ptF < fc) ? static_cast<uint8_t>(sDbc->getUInt32(i, ptF)) : 0;
                                const uint32_t ps = (psF < fc) ? sDbc->getUInt32(i, psF) : 0;
                                if (mc > 0 || ps > 0) (*spellCostMap)[id] = {.manaCost = mc, .powerType = pt, .perSecond = ps};
                            }
                        }
                        LOG_INFO("SpellDataResolver: loaded ", spellCastIdx->size(), " cast indices, ",
                                 spellRangeIdx->size(), " range indices");
                    }
                    game::GameHandler::SpellDataInfo info;
                    auto ciIt = spellCastIdx->find(spellId);
                    if (ciIt != spellCastIdx->end()) {
                        auto ctIt = castTimeMap->find(ciIt->second);
                        if (ctIt != castTimeMap->end()) info.castTimeMs = ctIt->second;
                    }
                    auto riIt = spellRangeIdx->find(spellId);
                    if (riIt != spellRangeIdx->end()) {
                        auto rIt = rangeMap->find(riIt->second);
                        if (rIt != rangeMap->end()) {
                            info.minRange = rIt->second.first;
                            info.maxRange = rIt->second.second;
                        }
                    }
                    auto mcIt = spellCostMap->find(spellId);
                    if (mcIt != spellCostMap->end()) {
                        info.manaCost = mcIt->second.manaCost;
                        info.powerType = mcIt->second.powerType;
                        info.manaPerSecond = mcIt->second.perSecond;
                    }
                    return info;
                });
            }
            // Wire random property/suffix name resolver for item display
            {
                auto propNames   = std::make_shared<std::unordered_map<int32_t, std::string>>();
                auto propLoaded  = std::make_shared<bool>(false);
                auto* amPtr = assetManager.get();
                gameHandler->setRandomPropertyNameResolver([propNames, propLoaded, amPtr](int32_t id) -> std::string {
                    if (!amPtr || id == 0) return {};
                    // Initialised, not merely present: loadDBC answers nullptr
                    // before the assets are up, and latching on that left the
                    // suffix names empty for the rest of the session.
                    if (!*propLoaded && amPtr->isInitialized()) {
                        *propLoaded = true;
                        // The display name ("of the Bear" / "of Strength"). Field 1 unless the
                        // expansion's layout says otherwise: WotLK's ItemRandomProperties keeps
                        // an untranslated internal name there and the localized one at 7.
                        const auto* layouts = pipeline::getActiveDBCLayout();
                        auto nameField = [layouts](const char* table) -> uint32_t {
                            const auto* l = layouts ? layouts->getLayout(table) : nullptr;
                            return l ? (*l)["Name"] : 1u;
                        };
                        // ItemRandomProperties.dbc: positive IDs
                        if (auto dbc = amPtr->loadDBC("ItemRandomProperties.dbc"); dbc && dbc->isLoaded()) {
                            const uint32_t nameF = nameField("ItemRandomProperties");
                            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                int32_t rid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                std::string name = dbc->getString(r, nameF);
                                if (!name.empty() && rid > 0) (*propNames)[rid] = name;
                            }
                        }
                        // ItemRandomSuffix.dbc: keyed as negative IDs
                        if (auto dbc = amPtr->loadDBC("ItemRandomSuffix.dbc"); dbc && dbc->isLoaded()) {
                            const uint32_t nameF = nameField("ItemRandomSuffix");
                            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                int32_t rid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                std::string name = dbc->getString(r, nameF);
                                if (!name.empty() && rid > 0) (*propNames)[-rid] = name;
                            }
                        }
                    }
                    auto it = propNames->find(id);
                    return (it != propNames->end()) ? it->second : std::string{};
                });
            }
            // Wire random-suffix/property stat resolver. Reproduces the server's roll from the
            // same DBCs: a suffix (negative id) scales each SpellItemEnchantment STAT amount by
            // AllocationPct*suffixFactor/10000; a property (positive id) uses the enchant's fixed
            // amount. Caches all three tables on first use.
            {
                struct EnchEffect { uint32_t type; uint32_t arg; int32_t minAmount; };
                auto suffixMap = std::make_shared<std::unordered_map<int32_t, std::vector<std::pair<uint32_t,uint32_t>>>>();
                auto propMap   = std::make_shared<std::unordered_map<int32_t, std::vector<uint32_t>>>();
                auto enchMap   = std::make_shared<std::unordered_map<uint32_t, std::vector<EnchEffect>>>();
                auto loaded    = std::make_shared<bool>(false);
                auto* amPtr = assetManager.get();
                gameHandler->setRandomStatResolver(
                    [suffixMap, propMap, enchMap, loaded, amPtr](int32_t id, uint32_t suffixFactor)
                        -> std::vector<game::GameHandler::RandomStatBonus> {
                    std::vector<game::GameHandler::RandomStatBonus> out;
                    if (!amPtr || id == 0) return out;
                    if (!*loaded && amPtr->isInitialized()) {
                        // Initialised, not merely present: loadDBC answers
                        // nullptr before the assets are up, and latching on
                        // that left this table empty for the session.
                        *loaded = true;
                        // SpellItemEnchantment: enchId -> up to 3 (type, statArg, minAmount).
                        // Arg (stat type) is the 3 fields before Name; the effect-type array
                        // precedes the amount block(s) - 2 blocks (Min+Max) on TBC/WotLK, 1 on Vanilla.
                        if (auto dbc = amPtr->loadDBC("SpellItemEnchantment.dbc"); dbc && dbc->isLoaded()) {
                            const auto* sieL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
                            const uint32_t fc = dbc->getFieldCount();
                            const uint32_t nameF = pipeline::detectEnchantmentNameField(dbc.get(), sieL);
                            if (nameF >= 12 && nameF < fc) {
                                const uint32_t argBase = nameF - 3;
                                const bool singleAmount = fc < 34;  // Vanilla/Turtle: no separate Max array
                                const uint32_t effBase = argBase - (singleAmount ? 6u : 9u);
                                const uint32_t minBase = effBase + 3u;
                                for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                    uint32_t enchId = dbc->getUInt32(r, 0);
                                    if (enchId == 0) continue;
                                    std::vector<EnchEffect> effs;
                                    for (uint32_t s = 0; s < 3; ++s) {
                                        uint32_t type = dbc->getUInt32(r, effBase + s);
                                        if (type == 0) continue;
                                        effs.push_back({.type = type,
                                                        .arg = dbc->getUInt32(r, argBase + s),
                                                        .minAmount = dbc->getInt32(r, minBase + s)});
                                    }
                                    if (!effs.empty()) (*enchMap)[enchId] = std::move(effs);
                                }
                            }
                        }
                        // ItemRandomSuffix: enchant array at field 19, AllocationPct follows; N=(fc-19)/2.
                        if (auto dbc = amPtr->loadDBC("ItemRandomSuffix.dbc"); dbc && dbc->isLoaded()) {
                            const uint32_t fc = dbc->getFieldCount();
                            if (fc > 21 && (fc - 19u) % 2u == 0u) {
                                const uint32_t n = (fc - 19u) / 2u;
                                for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                    int32_t sid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                    if (sid <= 0) continue;
                                    std::vector<std::pair<uint32_t,uint32_t>> ens;
                                    for (uint32_t k = 0; k < n; ++k) {
                                        uint32_t ench = dbc->getUInt32(r, 19u + k);
                                        uint32_t pct  = dbc->getUInt32(r, 19u + n + k);
                                        if (ench != 0) ens.emplace_back(ench, pct);
                                    }
                                    if (!ens.empty()) (*suffixMap)[sid] = std::move(ens);
                                }
                            }
                        }
                        // ItemRandomProperties: up to 5 enchant ids at fields 2..6 (fixed amount).
                        if (auto dbc = amPtr->loadDBC("ItemRandomProperties.dbc"); dbc && dbc->isLoaded()) {
                            if (dbc->getFieldCount() > 6) {
                                for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                    int32_t pid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                    if (pid <= 0) continue;
                                    std::vector<uint32_t> ens;
                                    for (uint32_t k = 2; k <= 6; ++k) {
                                        uint32_t ench = dbc->getUInt32(r, k);
                                        if (ench != 0) ens.push_back(ench);
                                    }
                                    if (!ens.empty()) (*propMap)[pid] = std::move(ens);
                                }
                            }
                        }
                    }
                    auto addEnchant = [&](uint32_t enchId, int32_t computedAmount, bool useComputed) {
                        auto eit = enchMap->find(enchId);
                        if (eit == enchMap->end()) return;
                        for (const auto& e : eit->second) {
                            if (e.type != 5) continue;  // ITEM_ENCHANTMENT_TYPE_STAT only
                            int32_t amount = (useComputed && e.minAmount == 0) ? computedAmount : e.minAmount;
                            if (amount != 0) out.push_back({e.arg, amount});
                        }
                    };
                    if (id < 0) {
                        auto sit = suffixMap->find(-id);
                        if (sit != suffixMap->end())
                            for (const auto& [ench, pct] : sit->second) {
                                // 0x00707650: the share as a float, rounded
                                // the client's way - twice it less a half, to
                                // the nearest, halved.
                                const float share = static_cast<float>(pct) * 0.0001f *
                                                    static_cast<float>(suffixFactor);
                                const int32_t amount = static_cast<int32_t>(
                                    std::nearbyint(share + share - 0.5f)) >> 1;
                                addEnchant(ench, amount, true);
                            }
                    } else {
                        auto pit = propMap->find(id);
                        if (pit != propMap->end())
                            for (uint32_t ench : pit->second) addEnchant(ench, 0, false);
                    }
                    return out;
                });
            }
            LOG_INFO("Addon system initialized, found ", addonManager_->getAddons().size(), " addon(s)");
        } else {
            LOG_WARNING("Failed to initialize addon system");
            addonManager_.reset();
        }

        // Initialize world loader (handles terrain streaming, world preload, map transitions)
        worldLoader_ = std::make_unique<WorldLoader>(
            *this, renderer.get(), assetManager.get(), gameHandler.get(),
            entitySpawner_.get(), appearanceComposer_.get(), window.get(),
            world.get(), addonManager_.get());

        // Re-wire UIServices now that all services (addonManager_, worldLoader_) are available
        if (uiManager) {
            ui::UIServices uiServices;
            uiServices.window = window.get();
            uiServices.renderer = renderer.get();
            uiServices.assetManager = assetManager.get();
            uiServices.gameHandler = gameHandler.get();
            uiServices.expansionRegistry = expansionRegistry_.get();
            uiServices.addonManager = addonManager_.get();
            uiServices.audioCoordinator = audioCoordinator_.get();
            uiServices.entitySpawner = entitySpawner_.get();
            uiServices.appearanceComposer = appearanceComposer_.get();
            uiServices.worldLoader = worldLoader_.get();
            uiManager->setServices(uiServices);
        }

        // Start background preload for last-played character's world.
        // Warms the file cache so terrain tile loading is faster at Enter World.
        {
            auto lastWorld = worldLoader_->loadLastWorldInfo();
            if (lastWorld.valid) {
                worldLoader_->startWorldPreload(lastWorld.mapId, lastWorld.mapName, lastWorld.x, lastWorld.y);
            }
        }

    } else {
        LOG_WARNING("Failed to initialize asset manager - asset loading will be unavailable");
        LOG_WARNING("Set WOW_DATA_PATH environment variable to your WoW Data directory");
    }

    // If the archives never opened, the fonts were not tried at all. Loose
    // files are still worth a look, and this is still before the first frame.
    if (uiManager) {
        uiManager->loadInterfaceFont(assetPath, nullptr);
        uiManager->loadInterfaceFont(dataPath, nullptr);
    }
}

void Application::run() {
    ZoneScopedN("Application::run");
    LOG_INFO("Starting main loop");

    // Do not pin the main thread. The shared render pool is created lazily
    // from this thread, and OS threads inherit their creator's affinity mask
    // on Linux. Pinning here silently confined every later render worker to
    // CPU 0 and defeated all command-recording parallelism.
    //
    // One thing does set an affinity mask, and only when the player asks for
    // it: the client's own processAffinityMask cvar, read once at startup and
    // handed straight to the OS - a process mask, which applies to the threads
    // already running as well (0x004022e0 is the cvar's change callback, with the screen's own warning that "changes require restart
    // to take effect"; 0x0076fe00 is GetCurrentProcess and
    // SetProcessAffinityMask, and nothing else). The mask is held as a plain
    // number, as the client's own parse reads it - 3 allows the first two
    // cores. Every thread made afterwards inherits it, which is what a player
    // narrowing the game onto fewer cores is asking for; a mask naming cores
    // the machine does not have is refused by the OS, which is said out loud
    // rather than left as a game that quietly runs on all of them.

    if (const unsigned long long mask =
            std::strtoull(addons::storedCVarValue("processAffinityMask", "0").c_str(), nullptr, 10);
        mask != 0) {
#if defined(_WIN32)
        if (SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) {
            LOG_INFO("Process affinity set to 0x", std::hex, mask, std::dec,
                     " by the processAffinityMask cvar");
        } else {
            LOG_WARNING("processAffinityMask 0x", std::hex, mask, std::dec,
                        " was refused - it names cores this machine does not have");
        }
#else
        // Not on Linux or macOS: <windows.h> is not this build's, and the
        // client's own call is the Windows one.
        LOG_WARNING("processAffinityMask is set; the mask is a Windows call, and this build "
                    "leaves cores to the scheduler");
#endif
    }

#ifndef WOWEE_MOBILE
    if (mcpOptions_.enabled) mcp_ = mcp::Server::start(*this, mcpOptions_);
#endif

    frameProfileEnabled_ = core::envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    if (frameProfileEnabled_) {
        LOG_WARNING("Frame timing profile enabled (WOWEE_FRAME_PROFILE=1) - "
                    "the per-stage breakdown will be reported at warning");
    }
    // The live breakdown: WOWEE_PROFILE=1, or /profile and Ctrl+F12 later.
    core::FrameProfiler::get().configureFromEnvironment();

    // Integer nanoseconds off a monotonic clock, and the finest sleep the
    // platform will give for as long as the loop runs. See frame_pacer.hpp
    // for what each was costing at high refresh rates.
    FramePacer pacer;
    const SleepPrecisionScope sleepPrecision;
    beatWatchdog();
    stall_report::initMainThread();
    std::atomic<int64_t>& watchdogHeartbeatMs = watchdogHeartbeatMs_;
    // Signal flag: watchdog sets this when a stall is detected, main loop
    // handles the actual SDL calls. SDL2 video functions must only be called
    // from the main thread (the one that called SDL_Init); calling them from
    // a background thread is UB on macOS (Cocoa) and unsafe on other platforms.
    std::atomic<bool> watchdogRequestRelease{false};
    // std::jthread would express this, but libc++ on the macOS CI image has no
    // <stop_token>, so the flag and the thread stay separate and a guard does
    // what jthread's destructor would: stop and join on every exit from this
    // scope, including an exception. That replaces a try/catch whose only job
    // was to run the same three lines before rethrowing, plus a second copy of
    // them after the loop.
    std::atomic<bool> watchdogRunning{true};
    std::thread watchdogThread([&watchdogRunning, &watchdogHeartbeatMs, &watchdogRequestRelease]() {
        bool signalledForCurrentStall = false;
        int stallReports = 0;
        while (watchdogRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t lastBeatMs = watchdogHeartbeatMs.load(std::memory_order_acquire);
            const int64_t stallMs = nowMs - lastBeatMs;

            if (stallMs > 1500) {
                if (!signalledForCurrentStall) {
                    watchdogRequestRelease.store(true, std::memory_order_release);
                    LOG_WARNING("Main-loop stall detected (", stallMs,
                                "ms) - requesting mouse capture release");
                    signalledForCurrentStall = true;
                }
                // What it was doing, without the logger - which the main
                // loop flushes, so a loop that never comes back loses the
                // warning above (stall_report.hpp). Again at 5 and 15 s, to
                // tell a long frame from one that is not going to end.
                const int64_t marks[] = {1500, 5000, 15000};
                while (stallReports < 3 && stallMs > marks[stallReports]) {
                    stall_report::write(stallMs, ++stallReports);
                }
            } else {
                signalledForCurrentStall = false;
                stallReports = 0;
            }
        }
    });

    // Stops and joins on the way out of this scope, however that happens.
    struct WatchdogStop {
        std::atomic<bool>& running;
        std::thread& thread;
        ~WatchdogStop() {
            running.store(false, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } watchdogStop{watchdogRunning, watchdogThread};

    while (running && !window->shouldClose()) {
        const auto frameStart = std::chrono::steady_clock::now();
        beatWatchdog();
        // What a burst of warnings left in the buffer. A warning no longer
        // writes itself to disk one line at a time - see Logger::flushIfStale -
        // so this is what keeps the tail of one from sitting there.
        Logger::getInstance().flushIfStale();
        if (settleScreen_) stepWorldSettle(frameStart);

        // Handle watchdog mouse-release request on the main thread where
        // SDL video calls are safe (required by SDL2 threading model).
        if (watchdogRequestRelease.exchange(false, std::memory_order_acq_rel)) {
            // SDL3 captures per window rather than globally. The focused window is the one the player is pointing at, and relative mode means nothing for any other - so that is the one to ask.
            SDL_SetWindowRelativeMouseMode(SDL_GetKeyboardFocus(), false);
            SDL_ShowCursor();
            if (window && window->getSDLWindow()) {
                SDL_SetWindowMouseGrab(window->getSDLWindow(), false);
            }
            if (renderer && renderer->getCameraController()) {
                renderer->getCameraController()->releaseMouseCapture();
            }
            LOG_WARNING("Watchdog: force-released mouse capture on main thread");
        }

        // Hold the frame to the cap, if one is set.
        //
        // Before the delta time is taken, so the wait is part of the frame
        // it paces rather than a stall the next one has to absorb. Sleep
        // granularity is a millisecond or so, which is close enough for a
        // cap and far cheaper than spinning.
#ifndef WOWEE_VR
        if (window) {
            // Which of the two caps this frame is held to. The client keeps a
            // limit for the window being the active one and a usually lower
            // one for when it is not, and takes the background one only where
            // it is at least as strict (0x006836d0). This project's settings
            // row picks the foreground limit; the cvars of the client's names
            // say the same thing the client's own options write, and a player
            // can set those directly - and a cap under eight frames is lifted
            // to eight either way. A window without the keyboard is not the
            // active one; neither is an app the system has put in the
            // background, which is the same test the sound suspension uses.
            //
            // The client registers maxFPS "200" and maxFPSBk "30" (0x00760000
            // registration, defaults at 0x00a38bd0 / 0x00a2d568): an install
            // that never set the background cap still drops to 30 behind
            // another window. Read twice a second rather than every frame -
            // two store lookups and a parse per frame for numbers that change
            // when a player types a command.
            const int setting = window->frameCap();
            static int cvarLimit = 0;
            static int backgroundLimit = 30;
            static std::chrono::steady_clock::time_point capsReadAt{};
            if (const auto now = std::chrono::steady_clock::now(); now - capsReadAt > std::chrono::milliseconds(500)) {
                capsReadAt = now;
                cvarLimit = std::atoi(addons::storedCVarValue("maxFPS", "200").c_str());
                backgroundLimit = std::atoi(addons::storedCVarValue("maxFPSBk", "30").c_str());
            }
            const bool inBackground = window->getVkContext() && window->getVkContext()->inBackground();
            const bool active = !inBackground && SDL_GetKeyboardFocus() != nullptr;
            WOWEE_PROFILE_SCOPE("sleep: frame cap", Idle);
            pacer.waitForCap(capInEffect(cvarLimit > 0 ? cvarLimit : setting, backgroundLimit, active));
        }
#endif

        // Delta, and the start of the frame the cap above will pace. Taken
        // together from one clock reading so the two cannot disagree about
        // where the frame began.
        const std::int64_t deltaNs = pacer.tickNs();
        // Capped, so a stall - a breakpoint, a swapchain rebuild, a machine
        // coming back from sleep - does not teleport everything that
        // integrates over it.
        const float deltaTime = FramePacer::toSeconds(deltaNs);
        // But the units on their moves go by the clock, as the client's
        // splines do (GameHandler::setUnitClock).
        if (gameHandler) gameHandler->setUnitClock(std::chrono::steady_clock::now());
        // And the player's own steps, up to a second of them.
        if (auto* cc = renderer ? renderer->getCameraController() : nullptr)
            cc->setFrameSeconds(FramePacer::toSeconds(deltaNs, 1.0f));

        // A lost GPU device, from the loss until it is rebuilt: the session
        // kept up, nothing drawn or uploaded, and the rebuild tried when
        // there is a window and DeviceRecovery's delay has passed.
        if (rendering::VkContext* vk = window->getVkContext(); renderer && vk && vk->isDeviceLost()) {
            runWhileDeviceLost(*vk, deltaTime);
            core::FrameProfiler::get().endFrame();
            continue;
        }

        if (renderer && renderer->getCameraController() && ImGui::GetIO().WantCaptureMouse) {
            renderer->getCameraController()->releaseMouseCapture();
        }

        // Events and input, up to the update. A scope held in an optional
        // rather than a block, because what it covers is a hundred lines of
        // loop body with its own continues and breaks.
        std::optional<core::ProfileScope> eventsScope;
        if (core::FrameProfiler::enabled()) eventsScope.emplace("events + input", core::ProfileKind::Cpu);

        // Poll events
        //
        // Cleared here rather than after the draw, because the flag only
        // ever describes the iteration it was set in: the pump sets it and
        // the draw, further down this same iteration, is the only reader.
        ui::clearInterfaceConsumedKeys();
        ui::ageChatSlashEcho();

        // A lifecycle change the watch saw from another thread.
        if (const int pending = gPendingLifecycle.exchange(0, std::memory_order_acq_rel)) {
            applyLifecycle(window.get(), pending == 1, "deferred lifecycle event");
        }

        // Ask for text input while one of the interface's edit boxes has the
        // keyboard - and, where the keyboard is one the system draws on the
        // screen, keep the two in step.
        //
        // SDL2 delivered SDL_TEXTINPUT from the start and nothing had to ask.
        // SDL3 does not - and ImGui's backend calls SDL_StopTextInput every
        // time one of *its* fields loses focus - so after the migration a chat
        // box opened on slash and then took nothing: the slash is inserted by
        // the code that opens the box, and not one character after it arrived.
        // Asserted per frame rather than on a focus change because the backend
        // can turn it off again at any point.
        //
        // On a phone the same assertion was the problem: the player put the
        // keyboard away, the chat box kept the focus, and this raised the
        // keyboard again. A keyboard that goes away now takes the focus with
        // it - the box's ClearFocus, so ChatEdit_OnEditFocusLost deactivates
        // the chat as Escape would - and a box that lets the focus go puts the
        // keyboard away.
        if (window && addonManager_ && addonsLoaded_) {
            if (auto* engine = addonManager_->getLuaEngine()) {
                SDL_Window* sdlWindow = window->getSDLWindow();
                ui::ScreenKeyboardSync::Inputs in;
                in.hasScreenKeyboard = SDL_HasScreenKeyboardSupport();
                in.editFocused = engine->editBoxHasFocus();
                in.frameListens = !in.editFocused && engine->frameListensForChars();
                in.keyboardShown = in.hasScreenKeyboard && screenKeyboardShown(sdlWindow);
                in.textInputActive = sdlWindow && SDL_TextInputActive(sdlWindow);
                in.otherWantsText = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
                const auto act = gKeyboardSync.update(in);
                if (act.clearFocus) {
                    LOG_INFO("On-screen keyboard closed; the edit box gives up the focus");
                    engine->setEditFocus(0);
                }
                if (act.stopTextInput && sdlWindow) {
                    LOG_INFO("Edit box lost the focus; putting the on-screen keyboard away");
                    SDL_StopTextInput(sdlWindow);
                }
                if (act.startTextInput && sdlWindow) SDL_StartTextInput(sdlWindow);
            }
        }

#ifndef WOWEE_MOBILE
        // The MCP server's calls, where the command line asked for one: before
        // the events are read, so that what they press is this frame's.
        // A call worked on is the player's hands at work, as a key press is:
        // a long MCP wait does not run into the idle logout (0x00b499a4).
        if (mcp_ && mcp_->pump(deltaTime)) Input::getInstance().noteHardwareEvent();
#endif

        SDL_Event event;
        auto nextEvent = [](SDL_Event& e) {
#ifdef WOWEE_MOBILE
            // A left press the touch controls kept back and have now let
            // through, ahead of anything SDL queued after it.
            if (ui::touchControls().takeReplay(e)) return true;
#endif
            return SDL_PollEvent(&e);
        };
        while (nextEvent(event)) {
#ifndef WOWEE_MOBILE
            if (mcp_ && mcp_->filterEvent(event)) continue;
#endif
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_WHEEL ||
                event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_FINGER_DOWN ||
                event.type == SDL_EVENT_FINGER_MOTION)
                Input::getInstance().noteHardwareEvent();
            // Sound in Background. Off in the real client and off here: losing
            // focus silences the client rather than playing on behind whatever
            // the player switched to. Read at the moment focus changes, so
            // clearing the box takes effect on the next alt-tab and not the
            // next restart.
            //
            // Whether any of this client's windows has the keyboard, not
            // whether this one does: clicking the map window takes focus from
            // the game's, and that is not switching away from the game. Ahead
            // of the map window's dispatch below, which keeps its events.
            if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                const bool focused = SDL_GetKeyboardFocus() != nullptr ||
                                     event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                // Sound_EnableSoundWhenGameIsInBG decides whether the game is
                // heard while another window has the focus (0x004c5dc0).
                const bool playInBackground =
                    addons::storedCVarValue("Sound_EnableSoundWhenGameIsInBG", "0") != "0";
                // An app the system has sent to the background is silent
                // whatever the box says: SDL takes the focus away as it goes,
                // and that is not the player switching windows.
                const bool appInBackground =
                    window && window->getVkContext() && window->getVkContext()->inBackground();
                audio::AudioEngine::instance().setSuspended(
                    appInBackground || (!focused && !playInBackground));
                // mouseSpeed, the pointer's speed while the game has the focus
                // and the desktop's own otherwise (0x0086a2da; core/pointer_speed).
                core::pointer_speed::setMouseSpeed(static_cast<float>(std::atof(
                    addons::storedCVarValue("mouseSpeed", core::pointer_speed::desktopDefault()).c_str())));
                core::pointer_speed::setActive(focused && !appInBackground);
            }

            // The map window's own events stop here. A click on the map is not
            // a click on the world behind the game's window, and neither the
            // interface nor the camera may act on it.
            if (mapWindow_ && mapWindow_->handleEvent(event)) continue;

            // The right button lets go of what the cursor holds before
            // anything else hears of it, and that is all it does: no frame,
            // no camera and no world gets the press (0x0051fa50, which
            // 0x00495470 asks first). Not over this client's own windows.
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
                state == AppState::IN_GAME && addonManager_ && addonsLoaded_ &&
                !ImGui::GetIO().WantCaptureMouse && addonManager_->getLuaEngine() &&
                addonManager_->getLuaEngine()->rightPressPutsCursorDown()) {
                Input::getInstance().setMouseButtonSpent(SDL_BUTTON_RIGHT, true);
                continue;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
                Input::getInstance().setMouseButtonSpent(SDL_BUTTON_RIGHT, false);

            // Connected and disconnected, which is all the pad needs from the
            // queue - its sticks and buttons are sampled once a frame rather
            // than accumulated out of events.
            core::gamepad().handleEvent(event);
#ifdef WOWEE_MOBILE
            // The stick and the pinch read the finger events SDL sends
            // alongside the mouse ones. Every panel, slider and action button
            // keeps working through the mouse SDL already makes of the first
            // finger - but its left button is held back until it is known
            // whether the press is a tap, a push or a long press (a right
            // click, with no left click in it), and comes back through
            // nextEvent() when it is let through.
            {
                int tw = 0, th = 0;
                if (window) SDL_GetWindowSize(window->getSDLWindow(), &tw, &th);
                if (ui::touchControls().handleEvent(event, tw, th)) continue;
            }

            // A fold, a rotation, split screen, a move to another display, or
            // a safe area that grew or shrank. The swapchain follows the pixel
            // size, the interface the density and the safe area, and a finger
            // held through it is no longer where it was.
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                event.type == SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED ||
                event.type == SDL_EVENT_WINDOW_RESIZED) {
                if (window && window->getVkContext() &&
                    (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                     event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)) {
                    window->getVkContext()->markSwapchainDirty();
                }
                if (uiManager) uiManager->refreshInterfaceScale();
                ui::touchControls().displayChanged();
            }

            // Back is Escape. A phone has no Escape key, and Escape is what
            // closes the map, the bags and every dialog, and opens the game
            // menu - so without this the map could be opened and not closed.
            if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                event.key.scancode == SDL_SCANCODE_AC_BACK) {
                event.key.scancode = SDL_SCANCODE_ESCAPE;
                event.key.key = SDLK_ESCAPE;
            }

#endif
            // Pass event to UI manager first
            if (uiManager) {
                uiManager->processEvent(event);
            }

            // Pass mouse events to camera controller (skip when UI has mouse focus)
            if (renderer && renderer->getCameraController() && !ImGui::GetIO().WantCaptureMouse) {
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    renderer->getCameraController()->processMouseMotion(event.motion);
                }
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    // The middle and the side buttons over the world are keys
                    // to the binding tables, BUTTON3 to BUTTON5 (0x0055e340),
                    // with the modifiers held - over a frame they are its
                    // clicks.
                    if ((event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_X1 ||
                         event.button.button == SDL_BUTTON_X2) &&
                        addonManager_ && addonsLoaded_ && !ImGui::GetIO().WantTextInput) {
                        if (auto* engine = addonManager_->getLuaEngine()) {
                            const SDL_Keymod mods = SDL_GetModState();
                            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                            (void)engine->dispatchBindingKey(
                                event.button.button == SDL_BUTTON_MIDDLE ? addons::LuaEngine::kBindingKeyButton3
                                : event.button.button == SDL_BUTTON_X1   ? addons::LuaEngine::kBindingKeyButton4
                                                                         : addons::LuaEngine::kBindingKeyButton5,
                                down && (mods & SDL_KMOD_SHIFT) != 0, down && (mods & SDL_KMOD_CTRL) != 0,
                                down && (mods & SDL_KMOD_ALT) != 0, down, false);
                        }
                    }
                    renderer->getCameraController()->processMouseButton(event.button);
                }
                else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    // The interface gets first refusal, and only where a
                    // frame under the cursor asked for the wheel. Zooming
                    // the camera while scrolling a quest log is what
                    // happens without the check, and a frame that did not
                    // ask must not swallow it either.
                    bool takenByUi = false;
                    if (addonManager_ && addonManager_->getLuaEngine()) {
                        // ImGui's cursor, not SDL's, because that is what
                        // the rest of the widget dispatch is fed and the
                        // two need not agree on a scaled display. Flipped
                        // to bottom-origin for the same reason the button
                        // dispatch is: the tree measures upward.
                        const ImGuiIO& mio = ImGui::GetIO();
                        takenByUi = addonManager_->getLuaEngine()->dispatchMouseWheel(
                            mio.MousePos.x, mio.DisplaySize.y - mio.MousePos.y,
                            wheelClicks(event.wheel.y, event.wheel.timestamp));
                    }
                    // Then the wheel's binding: the camera's zoom unless the
                    // modifiers held make it another.
                    if (!takenByUi && event.wheel.y != 0.0f && addonManager_ && addonsLoaded_ &&
                        addonManager_->getLuaEngine()) {
                        const SDL_Keymod mods = SDL_GetModState();
                        takenByUi = addonManager_->getLuaEngine()->dispatchWheelBinding(
                            event.wheel.y > 0.0f, (mods & SDL_KMOD_SHIFT) != 0, (mods & SDL_KMOD_CTRL) != 0,
                            (mods & SDL_KMOD_ALT) != 0);
                    }
                    if (!takenByUi) {
                        renderer->getCameraController()->processMouseWheel(
                            static_cast<float>(event.wheel.y));
                    }
                }
            }

            // Handle window events
            if (event.type == SDL_EVENT_QUIT) {
                window->setShouldClose(true);
            }
            else if ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)) {
                // Closing the game's window quits. SDL sends QUIT only when the
                // last window closes, so with the map window open, closing this
                // one said nothing but this.
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    window->setShouldClose(true);
                }
                if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                    int newWidth = event.window.data1;
                    int newHeight = event.window.data2;
                    window->setSize(newWidth, newHeight);
                    // Mark swapchain dirty so it gets recreated at the correct size
                    if (window->getVkContext()) {
                        window->getVkContext()->markSwapchainDirty();
                    }
                    // Vulkan viewport set in command buffer, not globally
                    if (renderer && renderer->getCamera() && newHeight > 0) {
                        renderer->getCamera()->setAspectRatio(static_cast<float>(newWidth) / newHeight);
                    }
                    // Notify addons so UI layouts can adapt to the new size
                    if (addonManager_)
                        addonManager_->fireEvent("DISPLAY_SIZE_CHANGED");
                }
            }
            // Typed text, when an edit box is listening for it - or, when none
            // is, whichever frame has asked for the keyboard.
            //
            // A dialog reads its digits from OnChar and not from OnKeyDown:
            // StackSplitFrame's key handler passes numbers straight through on
            // purpose, so the amount could only be reached with the arrows and
            // typing "12" did nothing at all.
            else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (addonManager_ && addonsLoaded_) {
                    if (auto* engine = addonManager_->getLuaEngine()) {
                        if (engine->editBoxHasFocus()) {
                            // Unless it is the slash that opened this box,
                            // arriving after it took focus - see
                            // noteChatOpenedBySlash.
                            if (!ui::consumeChatSlashEcho(event.text.text)) {
                                engine->dispatchText(event.text.text);
                            }
                        } else {
                            engine->dispatchFrameChar(event.text.text);
                        }
                    }
                }
            }
            // Debug controls
            else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                // Shift, control and alt announce themselves. The interface
                // watches these to swap what a tooltip shows and what a
                // click will do - item comparison appears on shift, and an
                // action button's self-cast indicator on alt. Four frames
                // listen and none had ever been told.
                if (addonManager_ && addonsLoaded_) {
                    const char* modName = nullptr;
                    switch (event.key.key) {
                        case SDLK_LSHIFT: modName = "LSHIFT"; break;
                        case SDLK_RSHIFT: modName = "RSHIFT"; break;
                        case SDLK_LCTRL:  modName = "LCTRL";  break;
                        case SDLK_RCTRL:  modName = "RCTRL";  break;
                        case SDLK_LALT:   modName = "LALT";   break;
                        case SDLK_RALT:   modName = "RALT";   break;
                        default: break;
                    }
                    // Repeats are not changes: holding shift sends a stream
                    // of key-downs and the interface would rebuild every
                    // tooltip in the game for each one.
                    if (modName && event.key.repeat == 0) {
                        // Said once for shift going down, upstream of any
                        // tooltip. IsModifiedClick(COMPAREITEMS) answered false
                        // through two runs in which the key was pressed, which
                        // is either a key this client never sees or a question
                        // nothing asks again once the tooltip is already up.
                        // This separates them: a line here and no matching
                        // true from the gate means the event arrives and
                        // nothing acts on it.
                        static bool saidShift = false;
                        if (!saidShift && event.type == SDL_EVENT_KEY_DOWN &&
                            (modName[1] == 'S')) {
                            saidShift = true;
                            LOG_WARNING("MODIFIER_STATE_CHANGED fired for ",
                                        modName, " down - the client sees shift");
                        }
                        addonManager_->fireEvent(
                            "MODIFIER_STATE_CHANGED",
                            {modName, event.type == SDL_EVENT_KEY_DOWN ? "1" : "0"});
                    }
                }
            }
            // The release of a key that went down on a SPELL, ITEM, MACRO or
            // CLICK binding, which acts as the key comes up (0x00563150). The
            // press is what recorded it, so a release nothing pressed is
            // passed by.
            if (event.type == SDL_EVENT_KEY_UP && addonManager_ && addonsLoaded_) {
                if (auto* engine = addonManager_->getLuaEngine()) {
                    // A frame listening for keys has the release too, as it
                    // had the press: its OnKeyUp, and no binding. The movie
                    // frame acts on the release (MovieFrame_OnKeyUp: Escape
                    // closes it, Space and Enter stop the movie), and with
                    // only presses reaching frames the movie could not be
                    // skipped. Its own press's alone, whatever has the
                    // keyboard by then (0x00494490).
                    if (engine->dispatchFrameKey(event.key.key, false)) continue;
                    (void)engine->dispatchBindingKey(event.key.key, false, false, false,
                                                     false, false);
                }
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                // An addon's edit box takes the keystroke before anything
                // else looks at it. Otherwise typing into one would also
                // walk the character, and backspace would trip a keybind.
                if (addonManager_ && addonsLoaded_) {
                    if (auto* engine = addonManager_->getLuaEngine()) engine->takeKeyboard();
                    if (auto* engine = addonManager_->getLuaEngine();
                        engine && engine->editBoxHasFocus() &&
                        !engine->keyIsTheGames(event.key.key, event.key.mod)) {
                        // Command counts as control here, because this is the
                        // copy-and-paste modifier and on a Mac that is the one
                        // every other program uses. Windows and Linux send
                        // control; both are taken, so the gesture is the one
                        // the player already knows on whichever they are on.
                        const bool ctrl =
                            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
                        // Said before the dispatch, because dispatching is
                        // what lets go of the focus that the check above
                        // just used - ask afterwards and the box no longer
                        // admits to having taken anything.
                        //
                        // Only the three that let go. Every other key
                        // leaves the box focused, so the poll's own typing
                        // guard still answers for them.
                        switch (event.key.key) {
                            case SDLK_ESCAPE:
                                ui::noteInterfaceConsumedKey(ImGuiKey_Escape);
                                // The first of the three ways a press ends
                                // early, and the correct one: an edit box
                                // had the keyboard, so Escape closed the
                                // box and means nothing further. WoW stops
                                // here too.
                                LOG_WARNING("Escape: taken in the pump by a "
                                            "focused edit box; it closes the "
                                            "box and stops");
                                break;
                            case SDLK_RETURN:
                            case SDLK_KP_ENTER:
                                ui::noteInterfaceConsumedKey(ImGuiKey_Enter);
                                break;
                            case SDLK_TAB:
                                ui::noteInterfaceConsumedKey(ImGuiKey_Tab);
                                break;
                            default: break;
                        }
                        const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                        // The box's until it is let go of, also where the box
                        // gives the keyboard up before that.
                        if (!event.key.repeat) Input::getInstance().setKeySpent(event.key.scancode);
                        engine->dispatchKey(event.key.key, ctrl, shift);
                        continue;
                    }
                    // No edit box, but a dialog may still be listening -
                    // the colour picker, the stack splitter, the coin
                    // pickup. The key is only swallowed when one of them
                    // actually takes it, so with nothing up the movement
                    // keys and every binding carry on exactly as before.
                    if (auto* engine = addonManager_->getLuaEngine()) {
                        if (engine->dispatchFrameKey(event.key.key, true, event.key.repeat)) {
                            // Taken, so not also acted on by the ImGui side
                            // of the chain (GameScreen reads the same press):
                            // a key a frame takes goes nowhere else (0x004943c0
                            // stops at the one that does, 0x0048fb80). The
                            // movie frame's Escape went on to toggle the game
                            // menu, and the world map's M, which closes it in
                            // its OnKeyDown, opened it again.
                            ui::noteInterfaceConsumedKeycode(static_cast<int>(event.key.key));
                            // Nor by what reads the keyboard itself, as long
                            // as it is held: moving and jumping are bindings
                            // in the client, and the character walked and
                            // jumped under the open world map. One held from
                            // before goes on until it is let go of.
                            if (!event.key.repeat) Input::getInstance().setKeySpent(event.key.scancode);
                            // Escape says so, because "Escape does nothing"
                            // is a live report and this is one of the three
                            // ways the press can end before the chain that
                            // decides what it means ever runs. At warning:
                            // the default log carries nothing else, so an
                            // info line here is a line nobody will ever
                            // see. See the pair in GameScreen.
                            if (event.key.key == SDLK_ESCAPE) {
                                LOG_WARNING("Escape: taken in the pump by a "
                                            "frame listening for keys; the "
                                            "chain below never runs");
                            }
                            continue;
                        }
                    }
                    // Then whatever the interface has bound to the key.
                    //
                    // Last of the three, because a focused edit box and an
                    // open dialog both outrank a binding - which is WoW's
                    // order too. It declines for anything this client
                    // performs itself, so the keys that already work are
                    // untouched; what it adds is every command the client
                    // has no path for, which until now could be bound in
                    // the interface's own key-binding panel and then never
                    // honoured by any press.
                    // Not while one of this client's own text fields is
                    // being typed into: a letter there is text, not Z.
                    if (auto* engine = addonManager_->getLuaEngine();
                        engine && !ImGui::GetIO().WantTextInput) {
                        const SDL_Keymod mods = SDL_GetModState();
                        if (engine->dispatchBindingKey(
                                event.key.key,
                                (mods & SDL_KMOD_SHIFT) != 0,
                                (mods & SDL_KMOD_CTRL) != 0,
                                (mods & SDL_KMOD_ALT) != 0, true,
                                event.key.repeat)) {
                            // Not the client's own panels' as well.
                            ui::noteInterfaceConsumedKeycode(static_cast<int>(event.key.key));
                            // The fourth way a press can end in the pump - an
                            // interface key binding claimed it - which had no
                            // line. For the DEFAULT Escape this does not fire:
                            // Escape binds to TOGGLEGAMEMENU, which
                            // clientActsOnBinding lists, so dispatchBindingKey
                            // yields (returns false) and the poll chain below
                            // runs. It fires only if Escape has been rebound to
                            // a FrameXML command a binding script handles - in
                            // which case *that* is why the game menu never
                            // opens, and this line names it. So it is a real
                            // signal for a rebound Escape, not the default one.
                            if (event.key.key == SDLK_ESCAPE) {
                                LOG_WARNING("Escape: taken in the pump by an "
                                            "interface key binding (rebound off "
                                            "TOGGLEGAMEMENU); the game-menu chain "
                                            "never runs");
                            }
                            continue;
                        }
                    }
                }
                // The third way, and the ordinary one: nothing in the
                // interface wanted it, so the poll further down decides.
                // Said as well as the other two, because the whole value of
                // these lines is that exactly one of them appears per
                // press - silence would mean the key never arrived at all,
                // and that is a different fault in a different place.
                if (event.key.key == SDLK_ESCAPE) {
                    LOG_WARNING("Escape: through the pump untaken; the chain "
                                "below decides");
                }
                // Skip non-function-key input when UI (chat) has keyboard focus
                bool uiHasKeyboard = ImGui::GetIO().WantCaptureKeyboard ||
                                     ui::interfaceTakingTypedInput();
                auto sc = event.key.scancode;
                bool isFKey = (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12);
                if (uiHasKeyboard && !isFKey) {
                    continue;  // Let ImGui handle the keystroke
                }

                // The development keys are not built into a release.
                //
                // They are not bindings the player chose and not anything
                // the interface knows about, so a stray F-key silently
                // changing how the world is drawn is a bug report about
                // rendering rather than about a keystroke. Independent ifs
                // rather than a chain, so either can be compiled out on its
                // own; the scancodes are mutually exclusive anyway.
#ifndef NDEBUG
                // F1: Toggle performance HUD
                if (event.key.scancode == SDL_SCANCODE_F1) {
                    if (renderer && renderer->getPerformanceHUD()) {
                        renderer->getPerformanceHUD()->toggle();
                        bool enabled = renderer->getPerformanceHUD()->isEnabled();
                        LOG_INFO("Performance HUD: ", enabled ? "ON" : "OFF");
                    }
                }
                // No F4 shadow toggle: shadows are a setting on the
                // Graphics page (off by default, as the client's
                // extShadowQuality 0).
#endif
                // Ctrl+F12: the frame profiler. Not behind NDEBUG like the
                // HUD above - the build that is slow is the release one, and
                // it is the one this has to answer for. With Ctrl, because
                // a bare F-key belongs to the player's bindings.
                if (event.key.scancode == SDL_SCANCODE_F12 && event.key.repeat == 0 &&
                    (event.key.mod & SDL_KMOD_CTRL)) {
                    core::FrameProfiler::get().toggle();
                }
            }
        }

        if (window->shouldClose()) {
            break;
        }

        // The pad, before the keyboard is sampled: it drives the client
        // through core::Input's virtual keys, and Input::update is what turns
        // those into a press. Applied after that read, every button would be
        // one frame late and a tap of one could be missed entirely.
        core::gamepad().update();
        namePadKeysForInterface();
        ui::gamepadControls().setInWorld(state == AppState::IN_GAME);
        ui::gamepadControls().setWindow(window ? window->getSDLWindow() : nullptr);
        if (renderer && renderer->getCameraController()) {
            auto* cam = renderer->getCameraController();
            ui::gamepadControls().setCameraController(cam);
            ui::gamepadControls().update(deltaTime);
            cam->setSteering(ui::gamepadControls().isSteering());
        } else {
            ui::gamepadControls().setCameraController(nullptr);
            ui::gamepadControls().update(deltaTime);
        }

#ifdef WOWEE_VR
        // The headset's frame begins here, before the update: xrWaitFrame
        // paces the loop, and the controllers' input is applied where the
        // pad's is - before Input samples its virtual keys and before ImGui
        // opens its frame.
        {
            WOWEE_PROFILE_SCOPE("vr: wait frame + input", Wait);
            vr::Runtime::get().beginFrame(renderer.get(), window ? window->getSDLWindow() : nullptr,
                                          state == AppState::IN_GAME, deltaTime);
            if (vr::Runtime::get().exitRequested()) running = false;
        }
#endif

        // Update input
        Input::getInstance().update();
        eventsScope.reset();

        // Update application state
        // An update cut short by running out of memory never opened the
        // interface's frame (UIManager::update, its last stage): this
        // iteration draws nothing, rather than hand ImGui the last frame's
        // draw lists over an interface the logout has just taken down.
        bool updateCutShort = false;
        try {
            FrameMark;
            WOWEE_PROFILE_SCOPE("update", Cpu);
            update(deltaTime);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::update (state=", static_cast<int>(state),
                      ", dt=", deltaTime, "): ", e.what());
            if (!leaveWorldOutOfMemory("update", e.what(), false)) throw;
            updateCutShort = true;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::update (state=", static_cast<int>(state),
                      ", dt=", deltaTime, "): ", e.what());
            throw;
        }
        if (window->shouldClose()) {
            break;
        }
        // Render
        if (!updateCutShort) try {
            render();
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::render (state=", static_cast<int>(state), "): ", e.what());
            if (!leaveWorldOutOfMemory("render", e.what(), true)) throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::render (state=", static_cast<int>(state), "): ", e.what());
            throw;
        }
        // Swap buffers
        try {
            WOWEE_PROFILE_SCOPE("swapBuffers", Cpu);
            window->swapBuffers();
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during swapBuffers: ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during swapBuffers: ", e.what());
            throw;
        }

        processDeferredLeaveWorld();

        // An upload that ran out of memory since the last frame: whatever it
        // was for is missing from the world, and more will follow it.
        if (const auto upload = rendering::lastUploadFailure(); upload.serial != uploadFailureSeen_) {
            uploadFailureSeen_ = upload.serial;
            const std::string what = std::to_string(upload.bytes) + " bytes, VkResult " +
                                     std::to_string(static_cast<int>(upload.result));
            leaveWorldOutOfMemory("GPU upload", what.c_str(), false);
        }

        // A lost GPU device: rebuilt with everything drawn with it while the
        // session carries on, here between frames where nothing is recording
        // - or, past what DeviceRecovery allows, the end of the run.
        if (rendering::VkContext* vk = window->getVkContext(); renderer && vk) {
            // WOWEE_SIMULATE_DEVICE_LOSS=<seconds>: the device taken for lost
            // that long into the run, once, so the rebuild can be watched on a
            // machine whose driver never loses it.
            static const double simulateLossAt = [] {
                const char* v = std::getenv("WOWEE_SIMULATE_DEVICE_LOSS");
                return (v && *v) ? std::atof(v) : -1.0;
            }();
            static bool simulatedLoss = false;
            if (simulateLossAt >= 0.0 && !simulatedLoss && core::appTimeSeconds() >= simulateLossAt) {
                simulatedLoss = true;
                vk->simulateDeviceLoss();
            }
            if (vk->isDeviceLost()) handleDeviceLoss(*vk);
        }

        // Pace from the start of the frame we just completed. Using deltaTime
        // here measured the previous frame, and relying only on FIFO present
        // still allowed the main thread to saturate a core on high-refresh or
        // compositor-managed displays. VSync defaults to a conservative 60 Hz;
        // disabling it retains the existing 240 Hz ceiling.
        const auto targetFrame = window->isVsyncEnabled()
            ? std::chrono::microseconds(16667)
            : std::chrono::microseconds(4167);
        const auto deadline = frameStart + targetFrame;
        const auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            WOWEE_PROFILE_SCOPE("sleep: frame pacing", Idle);
            std::this_thread::sleep_until(deadline);
        }
        // Last thing in the loop, so the frame it closes is the whole of it,
        // sleeps included.
        core::FrameProfiler::get().endFrame();
    }

    LOG_INFO("Main loop ended");
}

void Application::namePadKeysForInterface() {
    // What the interface calls the pad's buttons, in the pad's own words.
    //
    // FrameXML reads a key's display name out of _G["KEY_"..key], and 3.3.5
    // predates controllers entirely - so with the pad's defaults now in the
    // binding table, the Key Bindings panel showed rows reading "PAD3" where
    // the thing in the player's hands says Square. The names come from the
    // same table the settings panel lists, so a Switch pad reads Y where a
    // PlayStation one reads Square.
    //
    // Written when the pad changes rather than every frame: it is a dozen
    // globals, and the only thing that moves them is a different controller.
    auto& pad = core::gamepad();
    const auto kind = pad.isConnected() ? pad.kind() : core::Gamepad::Kind::Unknown;
    if (kind == padKeyNamesFor_) return;
    padKeyNamesFor_ = kind;
    if (!pad.isConnected()) return;
    auto* engine = addonManager_ ? addonManager_->getLuaEngine() : nullptr;
    if (!engine) {
        // Tried again next frame: the interface is built after the pad is
        // opened, and this is worth nothing until it exists.
        padKeyNamesFor_ = core::Gamepad::Kind::Unknown;
        return;
    }

    std::string code;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        const auto button = static_cast<SDL_GamepadButton>(b);
        const char* key = ui::padKeyName(button);
        const char* label = ui::padButtonLabel(button, kind);
        if (!key || !*key || !label || !*label) continue;
        code += "KEY_";
        code += key;
        code += " = \"";
        code += label;
        code += "\"\n";
    }
    if (!code.empty()) engine->executeString(code);
}

void Application::updateMapWindow() {
    if (!uiManager || !renderer || !window) return;
    auto& gameScreen = uiManager->getGameScreen();
    auto& settings = gameScreen.getSettingsPanel();

    // Closed with its own button: that is the player turning it off.
    if (mapWindow_ && mapWindow_->takeClosedByPlayer()) {
        mapWindow_->close();
        settings.showMapWindow_ = false;
        gameScreen.saveSettings();
    }

    // Only in the world. Logging out closes it, and it opens again where it
    // was left when the next character comes in.
    const bool inWorld = (state == AppState::IN_GAME);
    const bool wanted = settings.showMapWindow_ && inWorld;
    const bool open = mapWindow_ && mapWindow_->isOpen();
    if (wanted && !open && !mapWindowFailed_) {
        if (!mapWindow_) mapWindow_ = std::make_unique<ui::MapWindow>();
        if (!mapWindow_->open(window->getSDLWindow(), renderer.get(), window->getVkContext(),
                              assetManager.get(), uiManager.get())) {
            mapWindowFailed_ = true;
        }
    } else if (!wanted && open) {
        mapWindow_->close();
    }
    if (!settings.showMapWindow_) mapWindowFailed_ = false;

    if (mapWindow_ && mapWindow_->isOpen()) {
        mapWindow_->buildFrame(inWorld, renderer->getCharacterPosition(),
                               renderer->getCharacterYaw(),
                               gameHandler ? gameHandler->getWorldStateZoneId() : 0);
    }
}

void Application::shutdown() {
#ifndef WOWEE_MOBILE
    // First: its calls read everything that is taken down below.
    mcp_.reset();
#endif
    // A streaming start from the data screen still waiting on the server:
    // its thread writes into this object.
    if (streamStartThread_.joinable()) streamStartThread_.join();
    // The desktop's own pointer speed back (core/pointer_speed).
    core::pointer_speed::restore();
    if (pipeline::DataStream::instance().active()) {
        pipeline::DataStream::instance().saveUse();
        const auto t = pipeline::DataStream::instance().totals();
        LOG_WARNING("STREAM: at exit: fetched ", t.fetchedBytes, " bytes in ", t.requests,
                    " requests this run; cache holds ", t.localBytes, " of ", t.mpqBytes,
                    " MPQ bytes (", t.totalBytes, " listed)",
                    t.failures ? "; " + std::to_string(t.failures) + " reads failed, the last: " + t.lastFailure
                               : std::string());
    }
    // A clean exit: the next start has nothing to warn of (rt_session_marker).
    rt_session::clear(getConfigRoot() + "/rt_session.marker");
    LOG_DEBUG("Shutting down application...");

    // Quitting from the world takes the interface down as logging out does
    // (0x00528f00): PLAYER_LEAVING_WORLD, PLAYER_LOGOUT and the saved
    // variables, while the world and the character are still there to ask
    // about. PLAYER_LOGOUT is raised nowhere else, so /quit, Exit Game and
    // closing the window left addons that write their saved variables on it
    // with nothing saved. Nothing when the interface is already down.
    unloadInterface();

    // Before the window, whose destructor takes SDL down with it.
    if (window) SDL_RemoveEventWatch(lifecycleWatch, window.get());
    ui::gamepadControls().setKeyRouter(nullptr);
    core::gamepad().shutdown();

    // Before the renderer it records through and the device it draws with.
    if (mapWindow_) mapWindow_->close();
    difficultyLoadingScreen_.reset();

    // Hide the window immediately so the OS doesn't think the app is frozen
    // during the (potentially slow) resource cleanup below.
    if (window && window->getSDLWindow()) {
        SDL_HideWindow(window->getSDLWindow());
    }

    // Stop background world preloader before destroying AssetManager
    if (worldLoader_) {
        worldLoader_->cancelWorldPreload();
    };

    // End the session while the renderer is still alive. disconnect() fires a
    // despawn callback per entity, and those release creature and player
    // models through EntitySpawner's renderer pointer. Left to ~GameHandler,
    // which runs after renderer.reset(), that pointer is eleven lines of
    // freed memory - the "Exit Game during a logout" path arrived here still
    // holding a world full of entities and crashed on the first one. Calling
    // it now empties the entity list, so the destructor's own disconnect()
    // has nothing left to fire.
    if (gameHandler) {
        gameHandler->disconnect();
    }

    // The portrait's offscreen view holds Vulkan resources and is registered
    // with the renderer, so it has to let go before the renderer does. Left to
    // member destruction order it would free images against a device that has
    // already gone.
    shutdownPortraits();

    // For the same reason, and it was never being done: ImGui's Vulkan backend
    // holds a pipeline, its layout and descriptor set layout, two shader
    // modules, a sampler, a command pool, the font atlas image with its view
    // and memory, and the per-frame vertex and index buffers. Nineteen objects,
    // which is exactly what vkDestroyDevice reported as leaked on every run.
    //
    // UIManager::shutdown() calls ImGui_ImplVulkan_Shutdown and was reached
    // from nowhere; its destructor is defaulted, so nothing released them. It
    // has to run here rather than at member destruction, because the backend
    // waits on the device and frees against it.
    if (uiManager) {
        uiManager->shutdown();
    }

    // What the block upload actually saved this session, rather than what the
    // asset survey predicted it would. Reported here because it is a whole
    // session's total and the walk decides which textures that covers.
    //
    // Latched because shutdown() runs twice on the way out - once from main and
    // again from ~Application - and the counters are static, so unlike every
    // member below they survive the first pass and would report themselves a
    // second time.
    static bool tallyReported = false;
    if (const auto tally = rendering::VkTexture::blockUploadTally();
        tally.textures > 0 && !std::exchange(tallyReported, true)) {
        const double savedPct =
            100.0 * (1.0 - static_cast<double>(tally.blockBytes) /
                               static_cast<double>(tally.decodedBytes));
        LOG_INFO("Block texture upload: ", tally.textures, " textures, ",
                 tally.blockBytes / (1024 * 1024), " MB uploaded vs ",
                 tally.decodedBytes / (1024 * 1024), " MB decoded (",
                 std::lround(savedPct), "% saved)");
    }

    // Explicitly shut down the renderer before destroying it - this ensures
    // all sub-renderers free their VMA allocations in the correct order,
    // before VkContext::shutdown() calls vmaDestroyAllocator().
    LOG_DEBUG("Shutting down renderer...");
    if (renderer) {
        renderer->shutdown();
    }
    LOG_DEBUG("Renderer shutdown complete, resetting...");
    renderer.reset();

    // Shutdown audio coordinator after renderer (renderer may reference audio during shutdown)
    if (audioCoordinator_) {
        audioCoordinator_->shutdown();
    }
    audioCoordinator_.reset();

    // Before the game handler. The Lua engine holds a raw pointer to it: its
    // destructor hands the handler an empty account-data bridge, the bridge
    // it installed captures the engine, and the state keeps the handler in
    // its registry for the bindings that ask about the session. Left to
    // member destruction in ~Application, all of that ran after
    // gameHandler.reset() below - the destructor wrote into a freed
    // GameHandler, which usually went unnoticed and sometimes ended a clean
    // quit in SIGSEGV. The interface itself is already down (unloadInterface
    // above), so this only saves what is left and closes the state.
    LOG_DEBUG("Resetting addonManager...");
    addonManager_.reset();

    LOG_DEBUG("Resetting world...");
    world.reset();
    LOG_DEBUG("Resetting gameHandler...");
    gameHandler.reset();
    gameServices_ = {};
    LOG_DEBUG("Resetting authHandler...");
    authHandler.reset();
    LOG_DEBUG("Resetting assetManager...");
    assetManager.reset();
    LOG_DEBUG("Resetting uiManager...");
    uiManager.reset();
    LOG_DEBUG("Resetting window...");
    window.reset();

    running = false;
    LOG_DEBUG("Application shutdown complete");
}

void Application::beginWorldSettle(std::unique_ptr<rendering::LoadingScreen> screen) {
    endWorldSettle("superseded");
    if (!screen || !uiManager) {
        if (screen) screen->shutdown();
        return;
    }
    settleScreen_ = std::move(screen);
    settleStart_ = settleLastFrame_ = std::chrono::steady_clock::now();
    settleCalmFrames_ = 0;
    settleTiming_ = false;
    rendering::LoadingScreen* drawn = settleScreen_.get();
    uiManager->setForegroundOverlay([drawn]() { drawn->drawOverlay(); });
}

void Application::stepWorldSettle(std::chrono::steady_clock::time_point frameStart) {
    if (!settleScreen_) return;
    if (state != AppState::IN_GAME) {
        endWorldSettle("left the world");
        return;
    }
    // Nobody walks off behind a loading screen.
    if (renderer && renderer->getCameraController()) renderer->getCameraController()->suppressMovementFor(0.1f);
    // Timed from the first frame in the world, not from the load's end: the
    // interface loads between the two, and took up to 25 s here - which, on
    // the clock, ran out the cap before a single frame had been drawn.
    if (!settleTiming_) {
        settleTiming_ = true;
        settleStart_ = settleLastFrame_ = frameStart;
        return;
    }
    const float frameMs = std::chrono::duration<float, std::milli>(frameStart - settleLastFrame_).count();
    settleLastFrame_ = frameStart;
    const float elapsed = std::chrono::duration<float>(frameStart - settleStart_).count();
    // Settled: a run of frames at an ordinary pace with nothing left queued
    // to spawn. Capped, as a machine that never reaches that pace must still
    // get in, and a crowd that never stops arriving must not hold it out.
    constexpr float kCalmFrameMs = core::world_settle::kCalmFrameMs;
    const bool queued = entitySpawner_ && entitySpawner_->hasWorkPending();
    settleCalmFrames_ = (frameMs <= kCalmFrameMs && !queued) ? settleCalmFrames_ + 1 : 0;
    const auto* water = renderer ? renderer->getWaterRenderer() : nullptr;
    const char* why = core::world_settle::verdict(elapsed, settleCalmFrames_, water && water->rtWaterPending());
    if (why) endWorldSettle(why);
}

void Application::endWorldSettle(const char* why) {
    if (!settleScreen_) return;
    if (uiManager) uiManager->setForegroundOverlay({});
    LOG_WARNING("World entry: settled behind the loading screen in ",
             std::chrono::duration<float>(std::chrono::steady_clock::now() - settleStart_).count(), " s (", why, ")");
    // Takes the world's sound hold with it.
    settleScreen_->shutdown();
    settleScreen_.reset();
}

void Application::shutdownPortraits() {
    unitPortrait_.shutdown(renderer.get());
    for (auto& face : faceViews_)
        if (face) face->shutdown(renderer.get());
    facePictures_.assign(facePictures_.size(), std::string());
    for (auto& [id, figure] : unitFigures_)
        if (figure.view) figure.view->shutdown(renderer.get());
}

void Application::handleDeviceLoss(rendering::VkContext& vk) {
    using Action = rendering::DeviceRecovery::Action;
    const double now = core::appTimeSeconds();
    deviceRecovery_.deviceLost(now);
    switch (deviceRecovery_.next(vk.canRebuildDevice(), now)) {
        case Action::None:
            return;
        case Action::Wait:
            // In the background, back without a native window yet, or between
            // one rebuild that did not take and the next. Nothing is drawn or
            // uploaded meanwhile, the session is kept up (runWhileDeviceLost),
            // and this is asked again next time round the loop.
            return;
        case Action::GiveUp:
            LOG_ERROR("GPU device lost ", deviceRecovery_.lossesInWindow(), " times in ",
                      static_cast<int>(rendering::DeviceRecovery::kLossWindowSeconds),
                      "s, or would not come back - exiting application");
            window->setShouldClose(true);
            return;
        case Action::Rebuild:
            if (!recoverFromDeviceLoss()) {
                LOG_ERROR("GPU device lost and could not be rebuilt - exiting application");
                window->setShouldClose(true);
            }
            return;
    }
}

void Application::runWhileDeviceLost(rendering::VkContext& vk, float deltaTime) {
    // Only a quit is acted on, as the loading screen does. The app going to
    // the background and back is seen by the lifecycle watch whoever polls,
    // and reaches the context from there; nothing else has anything to act
    // on while there is no device.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            window->setShouldClose(true);
            return;
        }
    }

    // The session, and only the session: packets read and handled, the ping
    // and the rest of what the game handler sends on its own clock. Nothing
    // that draws or uploads - the world, the spawn queues, the renderer, the
    // interface - runs against a device that is gone; what arrives for them
    // is picked up by the world reload after the rebuild. A world that has
    // disconnected is left for the full update to take down once there is a
    // device again.
    switch (state) {
        case AppState::AUTHENTICATION:
        case AppState::REALM_SELECTION:
            if (authHandler) authHandler->update(deltaTime);
            break;
        case AppState::CHARACTER_CREATION:
        case AppState::CHARACTER_SELECTION:
            if (gameHandler) gameHandler->update(deltaTime);
            break;
        case AppState::IN_GAME:
            if (gameHandler && gameHandler->getState() != game::WorldState::DISCONNECTED) {
                gameHandler->update(deltaTime);
            }
            break;
        default:
            break;
    }

    handleDeviceLoss(vk);
    if (!vk.isDeviceLost()) return;

    // Said once a second, so the log shows a client that is waiting rather
    // than one that has stopped.
    static auto lastSaid = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSaid > std::chrono::seconds(1)) {
        lastSaid = now;
        LOG_INFO("Not drawing: device lost", vk.inBackground() ? ", in the background" : "");
    }
    // Reading the socket does not need frame rate, and a phone in someone's
    // pocket should not spend a core on it.
    SDL_Delay(50);
}

void Application::releaseGpuForDeviceRebuild() {
    // The order is shutdown()'s, for the same reasons: whatever holds a
    // preview or a texture goes before the UI backend, the UI backend before
    // the renderer, and the renderer before the context. Nothing outside the
    // GPU is touched - the connection, the entities, the interface's state
    // and the player's position all stay.
    if (mapWindow_) mapWindow_->close();
    difficultyLoadingScreen_.reset();
    // A loading screen still up over a world settling in holds a picture and
    // bar made on this device. Kept, the world reload ended it as superseded
    // and destroyed those on the new device - a crash in the driver.
    endWorldSettle("device lost");
    ui::gamepadControls().setCameraController(nullptr);
    ui::touchControls().setCameraController(nullptr);

    // Every render instance is about to be gone. The spawner forgets the ones
    // it made while their renderers can still be asked to drop them; the
    // entities they drew stay with the game handler and are handed back to it
    // when the world is loaded again (WorldLoader::reloadAfterDeviceLoss).
    if (gameHandler && gameHandler->getTransportManager()) {
        auto* tm = gameHandler->getTransportManager();
        tm->setWMORenderer(nullptr);
        tm->setM2Renderer(nullptr);
        // Each transport names the instance it moves, and the rebuilt
        // renderers hand those numbers out again to other models. The
        // transports themselves stay - the server has not taken them, and a
        // passenger is still aboard - and go on along their routes on the
        // server's clock, bound to new models as their game objects are
        // spawned again (processPendingTransportRegistrations).
        tm->releaseRenderInstances("device lost: renderers rebuilt");
    }
    if (entitySpawner_) entitySpawner_->resetAllState();
    playerCharacterSpawned = false;
    npcsSpawned = false;
    highlightedInstance_ = 0;
    highlightedKind_ = 0;

    shutdownPortraits();
    if (uiManager) uiManager->releaseGpu();
    if (renderer) renderer->shutdown();
    // The context itself is torn down by its own rebuild.
}

bool Application::recoverFromDeviceLoss() {
    rendering::VkContext* vk = window ? window->getVkContext() : nullptr;
    if (!vk || !renderer) return false;

    // Released once, on the first attempt. One that does not take leaves it
    // released, and the next is tried from the main loop when DeviceRecovery
    // says, with the session kept up in between (runWhileDeviceLost).
    if (!gpuReleasedForRebuild_) {
        LOG_WARNING("GPU device lost - rebuilding the device and everything drawn with it "
                    "(recovery ", deviceRecovery_.recoveries() + 1, " this run)");
        deviceRebuildStarted_ = std::chrono::steady_clock::now();
        releaseGpuForDeviceRebuild();
        gpuReleasedForRebuild_ = true;
    }

    // Android may have sent the app to the background since the last try;
    // the context keeps it in the background if so.
    if (!vk->rebuildAfterDeviceLoss()) {
        const double delay = deviceRecovery_.rebuildFailed(core::appTimeSeconds());
        if (deviceRecovery_.next(true, core::appTimeSeconds()) ==
            rendering::DeviceRecovery::Action::GiveUp) {
            return false;
        }
        LOG_WARNING("Trying the device again in ", delay, "s (attempt ",
                    deviceRecovery_.attempts() + 1, ")");
        return true;
    }
    gpuReleasedForRebuild_ = false;
    deviceRecovery_.rebuilt();
    const bool inWorld = state == AppState::IN_GAME && gameHandler && gameHandler->isInWorld() &&
                         worldLoader_;

    // shutdown() let go of it with everything else.
    renderer->setAudioCoordinator(audioCoordinator_.get());
    if (!renderer->initialize(window.get())) {
        LOG_ERROR("Renderer would not initialize on the rebuilt device");
        return false;
    }
    renderer->setSelectionCirclePlace([this](uint64_t guid) { return getSelectionCircle(guid); });
    if (uiManager) uiManager->restoreGpu();

    // The world, loaded again where the player is. The session never left it:
    // the server is still sending, the entities are all known, and only what
    // drew them is new.
    if (inWorld) {
        // Spawns that arrived while there was no device were queued for
        // renderers that are gone; the reload hands the spawner every entity
        // the session knows instead.
        if (entitySpawner_) entitySpawner_->clearAllQueues();
        const auto& movement = gameHandler->getMovementInfo();
        const glm::vec3 server = core::coords::canonicalToServer(
            glm::vec3(movement.x, movement.y, movement.z));
        worldLoader_->reloadAfterDeviceLoss(gameHandler->getCurrentMapId(),
                                            server.x, server.y, server.z);
        // The state did not change, so setState did not wire the new camera
        // controller and spell visual system.
        if (state == AppState::IN_GAME) connectWorldHooks();
    }
    if (uiManager) uiManager->getGameScreen().reapplySettingsAfterGpuRebuild(renderer.get());
    // The stored CVars as well, which reach the model renderers, the weather
    // and the camera only by being acted on - at startup once the interface
    // is loaded, which it still is.
    if (addonManager_ && addonsLoaded_) addonManager_->reapplyStoredCVars();

    LOG_WARNING("GPU device rebuilt in ",
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - deviceRebuildStarted_).count(),
                "ms", inWorld ? ", world reloaded" : "");
    return true;
}

void Application::setState(AppState newState) {
    if (state == newState) {
        return;
    }

    LOG_INFO("State transition: ", static_cast<int>(state), " -> ", static_cast<int>(newState));
    state = newState;
    // The idle minutes of a character count from its entering the world.
    Input::getInstance().noteHardwareEvent();

    // Handle state transitions
    switch (newState) {
        case AppState::FIRST_RUN:
            // Nothing to enter: the screen builds what is missing and the
            // client is reopened afterwards.
            break;
        case AppState::DATA_FOLDER:
            // Nothing to enter either: the screen only looks and chooses, and
            // the client is reopened to use what it found.
            break;
        case AppState::AUTHENTICATION:
            // Show auth screen
            break;
        case AppState::REALM_SELECTION:
            // Show realm screen
            break;
        case AppState::CHARACTER_CREATION:
            // Show character create screen
            break;
        case AppState::CHARACTER_SELECTION:
            // Show character screen
            if (uiManager && assetManager) {
                uiManager->getCharacterScreen().setAssetManager(assetManager.get());
            }
            // A backstop. Leaving the world goes through performLeaveWorld,
            // which takes the interface down there; this catches any other way
            // of arriving here with it still standing, and does nothing when
            // it is already down.
            unloadInterface();
            // Ensure no stale in-world player model leaks into the next login attempt.
            // If we reuse a previously spawned instance without forcing a respawn, appearance (notably hair) can desync.
            npcsSpawned = false;
            playerCharacterSpawned = false;
            wasAutoAttacking_ = false;
            if (worldLoader_) worldLoader_->resetLoadedMap();
            spawnedPlayerGuid_ = 0;
            spawnedAppearanceBytes_ = 0;
            spawnedFacialFeatures_ = 0;
            if (renderer && renderer->getCharacterRenderer()) {
                uint32_t oldInst = renderer->getCharacterInstanceId();
                if (oldInst > 0) {
                    renderer->setCharacterFollow(0);
                    if (auto* ac = renderer->getAnimationController()) ac->clearMount();
                    renderer->getCharacterRenderer()->removeInstance(oldInst);
                }
            }
            break;
        case AppState::IN_GAME:
            connectWorldHooks();
            break;
        case AppState::DISCONNECTED:
            // Back to auth
            break;
    }
}

// What the world's renderers and the game handler are wired with on entering
// the world: the camera controller's movement and stance callbacks, the spell
// visual system's resolvers, and the game handler's callbacks into them.
//
// Run by setState on the way in, and again by recoverFromDeviceLoss - the
// camera controller and the spell visual system are made again with the
// renderer, and the state does not change, so setState would not run it.
namespace {
// A siege missile's collision (0x006fcaa0, 0x006fc4a0, 0x006fbff0): each
// unit's place, scale, CreatureModelData collision columns, kind, the most
// of its run, swim and flight speeds, and the vehicle it rides or is - of
// every unit, or of those `only` names.
std::vector<rendering::SpellVisualSystem::CollisionUnit> missileCollisionUnits(
    game::GameHandler* gameHandler, pipeline::AssetManager* assetManager, EntitySpawner* entitySpawner,
    rendering::Renderer* renderer, const std::vector<uint64_t>* only) {
    std::vector<rendering::SpellVisualSystem::CollisionUnit> units;
    if (!gameHandler) return units;
    auto& entities = gameHandler->getEntityManager();
    const uint64_t mover = gameHandler->activeMover();
    const bool hasMover = mover != 0 && entities.getEntity(mover) != nullptr;
    auto displays = assetManager ? assetManager->loadDBCOptional("CreatureDisplayInfo.dbc") : nullptr;
    auto models = assetManager ? assetManager->loadDBCOptional("CreatureModelData.dbc") : nullptr;
    const auto known = [&entities](uint64_t g) { return entities.getEntity(g) != nullptr; };
    const auto& creatures = gameHandler->getCreatureInfoCache();
    const auto add = [&](uint64_t guid, const game::Entity& e) {
        if (!e.isUnit()) return;
        const auto& unit = static_cast<const game::Unit&>(e);
        rendering::SpellVisualSystem::CollisionUnit c;
        c.guid = guid;
        // 0x00717fc0: where it stands.
        if (guid == gameHandler->getPlayerGuid() && renderer)
            c.shape.position = renderer->getCharacterPosition();
        else
            c.shape.position =
                core::coords::canonicalToRender(glm::vec3(e.getX(), e.getY(), e.getZ()));
        // Its scale (vtable +0x7c) and CreatureModelData row (0x00717a20).
        c.shape.scale = entitySpawner ? entitySpawner->unitModelScale(guid) : 1.0f;
        const int32_t display = displays ? displays->findRecordById(unit.getDisplayId()) : -1;
        if (display >= 0 && models && models->getFieldCount() > 27) {
            const int32_t model = models->findRecordById(displays->getUInt32(static_cast<uint32_t>(display), 1));
            if (model >= 0) {
                const auto row = static_cast<uint32_t>(model);
                c.shape.hasModelData = true;
                c.shape.collisionHeight = models->getFloat(row, 15);
                c.shape.missileRadius = models->getFloat(row, 25);
                c.shape.missilePush = models->getFloat(row, 26);
                c.shape.missileRaise = models->getFloat(row, 27);
            }
        }
        c.kind.isPlayer = e.getType() == game::ObjectType::PLAYER;
        if (!c.kind.isPlayer) {
            const auto it = creatures.find(unit.getEntry());
            c.kind.collidesWithMissiles =
                it != creatures.end() &&
                (it->second.typeFlags & rendering::missile_collision::kTypeFlagCollidesWithMissiles) != 0;
        }
        c.kind.hasMover = hasMover;
        c.kind.moverMayAttack = hasMover && gameHandler->unitMayAttack(mover, guid);
        const auto& speeds = e.getMovementSpeeds();
        c.maxSpeed = std::max(std::max(speeds.run, speeds.swim), speeds.flight);
        // 0x00748040 for a passenger, else 0x0074c650(0).
        if (const auto seat = gameHandler->vehicleRoster().passenger(guid))
            c.vehicle = seat->vehicleGuid;
        else
            c.vehicle = gameHandler->vehicleRoster().rootVehicle(guid, 0, known);
        units.push_back(c);
    };
    if (only) {
        for (uint64_t guid : *only)
            if (const auto e = entities.getEntity(guid)) add(guid, *e);
    } else {
        for (const auto& [guid, e] : entities.getEntities())
            if (e) add(guid, *e);
    }
    return units;
}
}  // namespace

void Application::connectClickToMoveHooks() {
    auto* cc = renderer ? renderer->getCameraController() : nullptr;
    if (cc) {
        // Click-to-move, steered by the game's state and moved by the camera
        // with the keys' own packets (0x007317a0).
        cc->setClickToMoveSteer([this](float facingYawDeg, float dt, bool mouseSteering,
                                       const rendering::CameraController::ClickToMoveMoverIn& mover) {
            rendering::CameraController::ClickToMoveSteer out;
            if (!gameHandler) return out;
            game::GameHandler::ClickToMoveMoverIn in;
            in.pitchRad = mover.pitchRad;
            in.liquidAbove = mover.liquidAbove;
            in.swimDepth = mover.swimDepth;
            const auto st = gameHandler->stepClickToMove(facingYawDeg, dt, mouseSteering, in);
            out.active = st.active;
            out.facingYawDeg = st.facingYawDeg;
            out.forward = st.forward;
            out.stopForward = st.stopForward;
            out.trackAsMove = st.trackAsMove;
            out.pitchRad = st.pitchRad;
            out.takeOff = st.takeOff;
            return out;
        });
        cc->setClickToMoveCancel([this]() {
            if (gameHandler) gameHandler->stopClickToMove();
        });
        // 0x007272c0(.., 1) -> 0x0071aee0, whoever ends it.
        if (gameHandler) {
            gameHandler->setClickToMoveStopMover([this]() {
                if (renderer && renderer->getCameraController()) renderer->getCameraController()->stopClickToMoveMover();
            });
        }
    }
    if (gameHandler) {
        // The surface at a place, for an enemy's place walked to (0x0072ee20
        // -> 0x00783a40): the facets of the ground, the buildings and the
        // doodads (flags 0x100111) a box of no size at the place reaches -
        // those not wholly beyond one side of it - and the first one's plane
        // read at the place's x and y.
        gameHandler->setGroundHeightQuery([this](const glm::vec3& canonical) -> std::optional<float> {
            if (!renderer) return std::nullopt;
            const glm::vec3 at = core::coords::canonicalToRender(canonical);
            std::vector<rendering::collision::Facet> facets;
            if (auto* terrain = renderer->getTerrainManager()) terrain->gatherCollisionFacets(at, at, facets);
            if (auto* wmo = renderer->getWMORenderer()) wmo->gatherCollisionFacets(at, at, facets);
            if (auto* m2 = renderer->getM2Renderer()) m2->gatherCollisionFacets(at, at, facets, false);
            return rendering::collision::firstFacetHeight(facets, at.x, at.y);
        });
    }
}

void Application::connectSpellVisualHooks() {
    // Which units hold a kit's weapon effects, and at what scale
    // (0x0073a6c0, 0x006f8c50), and the rest of the spell visual system's
    // view of the game.
    if (!gameHandler) return;
    if (auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr) {
        svs->setWeaponEffectHolder([this](uint32_t renderInstanceId) -> std::optional<float> {
            if (!entitySpawner_) return 1.0f;
            return entitySpawner_->kitWeaponEffectHolder(renderInstanceId);
        });
        // The aura state kits: the unit's model, its spell's
        // visual, its kits' scale, and whether it shows its
        // Flags 8 kits (0x00720400).
        svs->setCameraShakeSink([this](const rendering::camera_shake::Shake& shake, const glm::vec3& at) {
            if (renderer && renderer->getCameraController())
                renderer->getCameraController()->addCameraShake(shake, at);
        });
        svs->setAttachedEffectScale([this](uint32_t renderInstanceId) {
            return entitySpawner_ ? entitySpawner_->kitAttachedEffectScale(renderInstanceId) : 1.0f;
        });
        svs->setWorldEffectScale([this](uint32_t renderInstanceId) {
            return entitySpawner_ ? entitySpawner_->kitWorldEffectScale(renderInstanceId) : 1.0f;
        });
        // What a kit's CharProc procedures read: its spell's cast
        // time and aim, the unit's creature type flags, and the
        // light the tint goes to.
        svs->setKitSpellResolver([this](uint32_t spellId) {
            rendering::SpellVisualSystem::KitSpellInfo info;
            if (!gameHandler) return info;
            info.castTimeMs = gameHandler->getSpellData(spellId).castTimeMs;
            info.targetKind = gameHandler->getSpellTargetKind(spellId);
            info.mounts = gameHandler->spellAppliesMountedAura(spellId);
            info.requiredVision = gameHandler->spellRequiredVision(spellId);
            return info;
        });
        svs->setPlayerVisionResolver([this]() -> uint32_t {
            return gameHandler ? gameHandler->playerAuraVision() : 0u;
        });
        // A kit's animation on the unit it is played on (0x0073b140), the
        // player's as any unit's.
        svs->setKitAnimationSink([this](const rendering::SpellVisualSystem::KitAnimationPlay& play) {
            auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
            if (!cr) return;
            rendering::kit_animation::KitPlay kit{.type = play.type,
                                                  .startAnim = play.startAnim,
                                                  .anim = play.anim,
                                                  .kitFlags = play.kitFlags,
                                                  .castTime = play.castTime,
                                                  .castKitAnim = play.castKitAnim};
            const uint32_t asked = rendering::kit_animation::askFlags(kit.kitFlags);
            for (int32_t* anim : {&kit.startAnim, &kit.anim}) {
                if (*anim >= 0 && entitySpawner_)
                    *anim = static_cast<int32_t>(
                        entitySpawner_->heldSpecialAttack(play.renderInstanceId, static_cast<uint32_t>(*anim), asked));
            }
            cr->playKitAnimation(play.renderInstanceId, kit);
        });
        svs->setUnitTypeFlags([this](uint32_t renderInstanceId) {
            return entitySpawner_ ? entitySpawner_->creatureTypeFlags(renderInstanceId) : 0u;
        });
        svs->setLightTintSink([this](const glm::vec3& colour, uint32_t amount) {
            if (renderer && renderer->getLightingManager())
                renderer->getLightingManager()->setSpellLightTint(colour, amount);
        });
        svs->setUnitInstanceResolver([this](uint64_t guid) -> uint32_t {
            return gameHandler ? gameHandler->resolveUnitRenderInstance(guid) : 0;
        });
        svs->setUnitMountInstanceResolver([this](uint64_t guid) -> uint32_t {
            if (!entitySpawner_ || !gameHandler) return 0;
            if (guid == gameHandler->getPlayerGuid()) return entitySpawner_->getMountInstanceId();
            return entitySpawner_->remotePlayerMountInstance(guid);
        });
        // CharProc 16's mount transition (0x006f9670): the
        // spell's mount, its model, whether the unit rides, and
        // where it is handed over.
        svs->setActivePlayerResolver([this]() -> uint64_t {
            return gameHandler ? gameHandler->getPlayerGuid() : 0;
        });
        svs->setMountDisplayResolver([this](uint32_t spellId) -> std::optional<uint32_t> {
            if (!gameHandler) return std::nullopt;
            const uint32_t entry = gameHandler->getSpellMountCreature(spellId);
            if (entry == 0) return std::nullopt;
            const auto& cache = gameHandler->getCreatureInfoCache();
            auto it = cache.find(entry);
            if (it == cache.end()) {
                gameHandler->queryCreatureInfo(entry, 0);
                return 0u;
            }
            if (it->second.displayId[0] == 0) return std::nullopt;
            return it->second.displayId[0];
        });
        svs->setMountModelLoader([this](uint32_t displayId) -> uint32_t {
            return entitySpawner_ ? entitySpawner_->loadMountModel(displayId) : 0u;
        });
        svs->setUnitScaleResolver([this](uint64_t guid) -> float {
            return entitySpawner_ ? entitySpawner_->unitModelScale(guid) : 1.0f;
        });
        auto mountField = [this](uint64_t guid) -> uint32_t {
            if (!gameHandler) return 0;
            if (guid == gameHandler->getPlayerGuid()) return gameHandler->isMounted() ? 1u : 0u;
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (!entity || !entity->isUnit()) return 0;
            return static_cast<const game::Unit&>(*entity).getMountDisplayId();
        };
        svs->setUnitMountedQuery([mountField](uint64_t guid) { return mountField(guid) != 0; });
        svs->setUnitMountFieldQuery([this](uint64_t guid) -> uint32_t {
            if (!gameHandler) return 0;
            if (guid == gameHandler->getPlayerGuid()) return gameHandler->currentMountDisplayIdRef();
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (!entity || !entity->isUnit()) return 0;
            return static_cast<const game::Unit&>(*entity).getMountDisplayId();
        });
        svs->setMountSink([this](uint64_t guid, uint32_t displayId) {
            if (!entitySpawner_ || !gameHandler) return;
            // Any unit: a creature's transition hands it over
            // as a player's does (0x007412b0, 0x00740450).
            const bool player = guid == gameHandler->getPlayerGuid();
            if (player || entitySpawner_->isPlayerSpawned(guid) ||
                entitySpawner_->getCreatureInstanceId(guid) != 0)
                entitySpawner_->mountUnitNow(guid, displayId, player);
        });
        svs->setGroundQuery([this](const glm::vec3& top, const glm::vec3& bottom)
                                -> std::optional<rendering::mount_transition::GroundHit> {
            if (!renderer) return std::nullopt;
            std::optional<float> best;
            glm::vec3 normal(0.0f, 0.0f, 1.0f);
            auto consider = [&](std::optional<float> z, const glm::vec3& n) {
                if (z && *z <= top.z && *z >= bottom.z && (!best || *z > *best)) {
                    best = z;
                    normal = n;
                }
            };
            if (auto* terrain = renderer->getTerrainManager()) {
                const auto z = terrain->getHeightAt(top.x, top.y);
                const auto zx = terrain->getHeightAt(top.x + 0.5f, top.y);
                const auto zy = terrain->getHeightAt(top.x, top.y + 0.5f);
                glm::vec3 n(0.0f, 0.0f, 1.0f);
                if (z && zx && zy) n = glm::normalize(glm::vec3((*z - *zx) * 2.0f, (*z - *zy) * 2.0f, 1.0f));
                consider(z, n);
            }
            if (auto* wmo = renderer->getWMORenderer()) consider(wmo->getFloorHeight(top.x, top.y, top.z), glm::vec3(0.0f, 0.0f, 1.0f));
            if (auto* m2 = renderer->getM2Renderer()) consider(m2->getFloorHeight(top.x, top.y, top.z), glm::vec3(0.0f, 0.0f, 1.0f));
            if (!best) return std::nullopt;
            const float span = top.z - bottom.z;
            return rendering::mount_transition::GroundHit{
                .fraction = span > 0.0f ? (top.z - *best) / span : 0.0f, .normal = normal};
        });
        // A transport's matrix, for a dynamic object riding it
        // (0x007132e0 -> 0x00704910).
        svs->setTransportFrameQuery([this](uint64_t transportGuid) -> std::optional<glm::mat4> {
            auto* transports = gameHandler ? gameHandler->getTransportManager() : nullptr;
            return transports ? transports->passengerRenderFrame(transportGuid) : std::nullopt;
        });
        svs->setSegmentQuery([this](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
            return renderer ? renderer->intersectSegment(from, to) : std::nullopt;
        });
        // A siege missile's collision (0x006fcaa0, 0x006fc4a0, 0x006fbff0):
        // each unit's place, scale, CreatureModelData collision columns,
        // kind, the most of its run, swim and flight speeds, and the
        // vehicle it rides or is.
        svs->setCollisionUnitsQuery([this](const std::vector<uint64_t>* only) {
            return missileCollisionUnits(gameHandler.get(), assetManager.get(), entitySpawner_.get(),
                                         renderer.get(), only);
        });
        // CMSG_UPDATE_PROJECTILE_POSITION (0x00703640).
        svs->setProjectilePositionSink(
            [this](uint64_t casterGuid, uint32_t spellId, uint8_t castCount, const glm::vec3& point) {
                if (auto* spells = gameHandler ? gameHandler->getSpellHandler() : nullptr)
                    spells->sendProjectilePosition(casterGuid, spellId, castCount, point);
            });
        svs->setSpellVisualResolver([this](uint32_t spellId) -> uint32_t {
            return gameHandler ? gameHandler->getSpellVisualId(spellId) : 0;
        });
        // Whose spell sounds keep their voice: the active mover's and the
        // listener object's (0x007022d0, 0x006f9840, 0x006f7e40).
        svs->setActiveUnitsQuery([this]() {
            rendering::SpellVisualSystem::ActiveUnits units;
            if (gameHandler) {
                units.mover = gameHandler->activeMover();
                units.listener = gameHandler->listenerObject();
            }
            return units;
        });
        // 0x004fa5f0: the sound listener at the listener object
        // (0x007450b0) while Sound_ListenerAtCharacter is on - a unit
        // facing its own way (0x004f6a90), anything else the camera's.
        renderer->setSoundListenerAnchorQuery([this]() -> std::optional<audio::sound_listener::Anchor> {
            if (!gameHandler || !renderer) return std::nullopt;
            if (addons::storedCVarValue("Sound_ListenerAtCharacter", "1") == "0") return std::nullopt;
            const uint64_t guid = gameHandler->listenerObject();
            const auto entity = guid != 0 ? gameHandler->getEntityManager().getEntity(guid) : nullptr;
            if (!entity) return std::nullopt;
            audio::sound_listener::Anchor anchor;
            if (guid == gameHandler->getPlayerGuid()) {
                // The renderer's own character is where the player is drawn.
                anchor.position = renderer->getCharacterPosition();
                anchor.facing = audio::sound_listener::levelFacing(glm::radians(renderer->getCharacterYaw()));
            } else {
                anchor.position = core::coords::canonicalToRender(
                    glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                if (entity->isUnit()) {
                    // Render yaw is canonical yaw plus a quarter turn.
                    anchor.facing = audio::sound_listener::levelFacing(
                        glm::radians(core::coords::canonicalToCharacterYawDeg(entity->getOrientation())));
                }
            }
            return anchor;
        });
        // 0x006f7b00: a spell effect's $SND on the active mover's unit.
        renderer->getM2EventSounds().setMoverInstanceQuery([this]() -> uint32_t {
            return gameHandler ? gameHandler->resolveUnitRenderInstance(gameHandler->activeMover()) : 0;
        });
        // 0x00804cc0: a dynamic object waits on its cast's missile.
        svs->setMissileInFlightQuery([this](uint64_t caster, uint32_t spellId, uint32_t castTime) {
            return gameHandler && gameHandler->isSpellMissileInFlight(caster, spellId, castTime);
        });
        // A chain's ends (0x007fae90): the unit an instance
        // draws, its height, and a game object's place.
        svs->setInstanceUnitResolver([this](uint32_t renderInstanceId) -> uint64_t {
            return entitySpawner_ ? entitySpawner_->unitGuidForInstance(renderInstanceId) : 0;
        });
        svs->setUnitHeight([this](uint32_t renderInstanceId) {
            return entitySpawner_ ? entitySpawner_->unitGeoBoxHeight(renderInstanceId) : 0.0f;
        });
        svs->setMeleeDrawnQuery([this](uint64_t guid) {
            if (gameHandler && guid == gameHandler->getPlayerGuid())
                return appearanceComposer_ && appearanceComposer_->sheathState() == core::SheathState::Melee;
            return entitySpawner_ && entitySpawner_->unitMeleeDrawn(guid);
        });
        // A kit's worn item (CharProc 17): the player's own
        // drawing, or another player's equipment.
        svs->setWornItemSink([this](uint64_t guid, int equipSlot, uint32_t displayId,
                                    uint8_t inventoryType) {
            if (!gameHandler) return;
            if (guid == gameHandler->getPlayerGuid()) {
                if (appearanceComposer_)
                    appearanceComposer_->setItemOverride(equipSlot, displayId, inventoryType);
                gameHandler->markOnlineEquipmentDirty();
                return;
            }
            if (!entitySpawner_) return;
            if (entitySpawner_->isPlayerSpawned(guid)) {
                entitySpawner_->setPlayerItemOverride(guid, equipSlot, displayId, inventoryType);
            } else if (equipSlot == rendering::spell_kit::equipSlotForComponentSlot(0)) {
                // Any other unit's head only (0x006f82d0).
                entitySpawner_->setNpcHeadItem(guid, displayId);
            }
        });
        svs->setObjectFrameResolver([this](uint64_t guid, glm::mat4& frame) {
            auto entity = gameHandler ? gameHandler->getEntityManager().getEntity(guid) : nullptr;
            if (!entity || entity->getType() != game::ObjectType::GAMEOBJECT) return false;
            const glm::vec3 at = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            frame = glm::rotate(glm::translate(glm::mat4(1.0f), at),
                                entity->getOrientation() + glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
            return true;
        });
        svs->setUnarmedKitsQuery([this](uint64_t guid) {
            if (gameHandler && guid == gameHandler->getPlayerGuid())
                return appearanceComposer_ && appearanceComposer_->unarmedKitsShown();
            return entitySpawner_ && entitySpawner_->unarmedKitsShown(guid);
        });
        // 0x00702db0: a unit whose health is below 1 takes no wound
        // from a missile.
        // A bouncing missile's next target (0x007022d0): the unit the
        // client knows by that guid (0x004d4db0, mask 8), where it is.
        svs->setUnitEndResolver([this](uint64_t guid, rendering::SpellVisualSystem::MissileEnd& end) {
            if (!gameHandler) return false;
            if (guid == gameHandler->getPlayerGuid() && renderer) {
                end.position = renderer->getCharacterPosition();
            } else {
                auto entity = gameHandler->getEntityManager().getEntity(guid);
                if (!entity || !entity->isUnit()) return false;
                end.position = core::coords::canonicalToRender(
                    glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            }
            end.renderInstanceId = gameHandler->resolveUnitRenderInstance(guid);
            return true;
        });
        // UNIT_COMBAT for a missed unit as the missile lands
        // (0x00702fc0, 0x00700e20).
        svs->setMissileMissSink([this](uint32_t spellId, uint64_t caster, uint64_t guid, uint32_t reason) {
            if (gameHandler) gameHandler->fireSpellMissileMiss(spellId, caster, guid, reason);
        });
        svs->setUnitDeadQuery([this](uint64_t guid) {
            auto entity = gameHandler ? gameHandler->getEntityManager().getEntity(guid) : nullptr;
            const auto* unit = dynamic_cast<const game::Unit*>(entity.get());
            return unit && unit->getHealth() < 1;
        });
    }
}

void Application::connectWorldHooks() {
    // Wire up movement opcodes from camera controller
    if (renderer && renderer->getCameraController()) {
        auto* cc = renderer->getCameraController();
        cc->setMovementCallback([this, cc](uint32_t opcode) {
            if (!gameHandler) return;
            // A landing goes out from where the feet landed. The client moves
            // the unit by the fall step's collided move and lands it in that
            // same step (0x007618b0 -> 0x007612b0: the position at +0x10 is
            // advanced, then 0x00988490 takes it as the fall's end), and
            // MSG_MOVE_FALL_LAND carries that position. The camera
            // controller asks for the landing in the frame it lands, before
            // the frame's position reaches the game handler (update, after
            // the renderer's): the packet carried the frame before's height,
            // 1.3 to 1.8 yards over the floor at a long fall's speed.
            //
            // On a transport the client keeps the passenger's place on it
            // and lands it there in the same step (0x007618b0 steps it on
            // the transport, 0x00988490 ends the fall), so the landing goes
            // out with the place on the deck where the feet came down - and
            // the world position made from it (sendMovement). The place kept
            // on it here is the frame before's, taken from the world after
            // the renderer's update: a jump on the Undercity lift landed half
            // a yard over the car's floor.
            if (opcode == static_cast<uint32_t>(game::Opcode::MSG_MOVE_FALL_LAND) && renderer &&
                gameHandler->isOnTransport()) {
                const glm::vec3 landed = core::coords::renderToCanonical(renderer->getCharacterPosition());
                auto* transports = gameHandler->getTransportManager();
                const auto local = transports ? transports->worldToTransportLocal(
                                                    gameHandler->getPlayerTransportGuid(), landed)
                                              : std::nullopt;
                static const bool kFloorDebugTr = [] {
                    const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
                    return v && v[0] && v[0] != '0';
                }();
                if (local) {
                    if (kFloorDebugTr) {
                        const glm::vec3 was = gameHandler->getPlayerTransportOffset();
                        LOG_WARNING("FALL LAND: on the transport where the feet landed tz=", local->z,
                                    " (the frame before tz=", was.z, ")");
                    }
                    gameHandler->setPlayerTransportOffset(*local);
                }
            }
            if (opcode == static_cast<uint32_t>(game::Opcode::MSG_MOVE_FALL_LAND) && renderer &&
                !gameHandler->isOnTransport()) {
                const glm::vec3 landed = core::coords::renderToCanonical(renderer->getCharacterPosition());
                static const bool kFloorDebug = [] {
                    const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
                    return v && v[0] && v[0] != '0';
                }();
                if (kFloorDebug) {
                    LOG_WARNING("FALL LAND: from where the feet landed z=", landed.z, " (the frame before z=",
                                gameHandler->getMovementInfo().z, ")");
                }
                gameHandler->setPosition(landed.x, landed.y, landed.z);
            }
            if (opcode == static_cast<uint32_t>(game::Opcode::MSG_MOVE_FALL_LAND)) {
                if (auto* mh = gameHandler->getMovementHandler()) mh->landFall(cc->lastFallDrop());
                return;
            }
            gameHandler->sendMovement(static_cast<game::Opcode>(opcode));
        });
        // The movement state of whatever the keys move (MoverStep).
        rendering::CameraController::MoverStateHooks state;
        state.falling = [this]() {
            return gameHandler && gameHandler->getMovementInfo().hasFlag(game::MovementFlags::FALLING);
        };
        state.swimming = [this]() {
            return gameHandler && gameHandler->getMovementInfo().hasFlag(game::MovementFlags::SWIMMING);
        };
        // Off a ledge without a jump: the client falls from the step its
        // ground went missing in (0x007620f0 -> 0x00988370(0)), FALLING set
        // and the fall time counting from there, and no packet of its own.
        // Not on a transport, where the client does not mark a passenger
        // standing on it as falling and the server measured a lift's whole
        // descent as one, nor while one carries the player to another map:
        // it is off the transport only until the transport is known there.
        state.fallBegan = [this](uint32_t sinceMs) {
            if (!gameHandler || gameHandler->isOnTransport() || gameHandler->isOnTaxiFlight() || hasM2RideLock_ ||
                gameHandler->hasPendingPlayerTransportWorldTransfer())
                return;
            if (auto* mh = gameHandler->getMovementHandler()) mh->startFall(sinceMs);
        };
        state.fallsFar = [this]() {
            if (auto* mh = gameHandler ? gameHandler->getMovementHandler() : nullptr) mh->setFallingFar();
        };
        state.fallOver = [this]() {
            if (auto* mh = gameHandler ? gameHandler->getMovementHandler() : nullptr) mh->endFallUnsaid();
        };
        state.jumpPressed = [this, cc](float liquidOverFeet) {
            if (!gameHandler) return true;
            const uint64_t self = gameHandler->getPlayerGuid();
            if (gameHandler->activeMover() != self) return true;
            // 0x0072eb80: the loot let go of, and what the key does.
            gameHandler->closeLootByMoving();
            const auto me = std::dynamic_pointer_cast<game::Unit>(gameHandler->getEntityManager().getEntity(self));
            if (!me) return true;
            const auto& move = gameHandler->getMovementInfo();
            game::JumpKeyInput in;
            in.mounted = me->getMountDisplayId() != 0;
            if (in.mounted && entitySpawner_)
                in.mountFlags = entitySpawner_->creatureModelFlags(me->getMountDisplayId()).value_or(0);
            in.moveFlags = move.flags;
            in.floating = move.hasFlag(game::MovementFlags::LEVITATING) || (move.flags2 & 0x4u) != 0;
            in.deep = game::deepInLiquid(liquidOverFeet, cc->collisionHeight());
            const game::JumpKey does = game::jumpKey(in);
            if (does == game::JumpKey::MountSpecial) gameHandler->mountSpecial();
            return does == game::JumpKey::Jump;
        };
        state.turnShare = [this]() {
            if (!gameHandler) return 1.0f;
            const auto& move = gameHandler->getMovementInfo();
            return game::turnShare(move.flags, move.flags2);
        };
        state.pitchShare = [this]() {
            if (!gameHandler) return 1.0f;
            const auto& move = gameHandler->getMovementInfo();
            return game::pitchShare(move.flags, move.flags2);
        };
        cc->setMoverStateHooks(std::move(state));
        // The player sitting down or getting up: the request to the server,
        // and the animation at once (0x006d SetStandState -> 0x0073f060).
        const auto standTo = [this](uint8_t state) {
            if (!gameHandler) return;
            gameHandler->setStandState(state);
            auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
            if (cr && renderer->getCharacterInstanceId() != 0)
                cr->standStateChanged(renderer->getCharacterInstanceId(), state);
        };
        cc->setStandUpCallback([standTo]() { standTo(rendering::AnimationController::STAND_STATE_STAND); });
        cc->setSitDownCallback([standTo]() { standTo(rendering::AnimationController::STAND_STATE_SIT); });
        cc->setStickyCameraProbe([this]() {
            return gameHandler && gameHandler->askInterface("IsModifiedClick('STICKYCAMERA')");
        });
        // The stand state is carried by AnimationCallbackHandler, which
        // owns the one callback slot for it. A second registration here
        // replaced or was replaced by that one depending on which was
        // built last, which is how the seated flag stopped arriving.
        cc->setUseWoWSpeed(true);
    }
    connectClickToMoveHooks();
    if (gameHandler) {
        gameHandler->setFaceCameraProvider([this]() -> float {
            // Turn the character to the camera's look direction and report it in
            // canonical space (same camera-yaw→canonical mapping as the per-frame
            // orientation sync). Lets a fishing cast drop the bobber in front of
            // where the player is aiming even while standing still.
            if (!renderer || !renderer->getCameraController())
                return gameHandler ? gameHandler->getMovementInfo().orientation : 0.0f;
            // Use the CAMERA's live look yaw (getYaw), not getFacingYaw: the latter is
            // the character's facing, which is NOT updated by left-mouse orbit, so while
            // aiming at the water standing still it is stale. Turn the character to the
            // camera aim and use the same yaw→canonical mapping the (working) melee
            // facing uses, so the bobber lands where the player is looking.
            float camYawDeg = renderer->getCameraController()->getYaw();
            renderer->setCharacterYaw(camYawDeg);
            renderer->getCameraController()->setFacingYaw(camYawDeg);
            // "Face land to fish" ⇒ the bobber landed 180° opposite the aim, so the
            // canonical is a half-turn off here (getYaw's zero is opposite the look
            // vector). Add the half-turn so the bobber lands where the camera looks.
            float canon = core::coords::normalizeAngleRad(glm::radians(360.0f - camYawDeg));
            LOG_WARNING("[FISH-AIM] getYaw=", camYawDeg,
                        " getFacingYaw=", renderer->getCameraController()->getFacingYaw(),
                        " charYaw=", renderer->getCharacterYaw(),
                        " canonicalDeg=", canon * 57.29578f,
                        " serverYawDeg=", core::coords::canonicalToServerYaw(canon) * 57.29578f);
            return canon;
        });

        // The player's own movement events, as the client animates the unit
        // it moves (0x00741xxx -> 0x0073ed10).
        gameHandler->setMoverEventCallback([this](game::Opcode op, uint32_t flagsBefore, bool launchedBefore) {
            animatePlayerMovement(op, flagsBefore, launchedBefore);
        });
        // The player's casts set its sheath state (0x007fa2e0): a
        // ranged spell such as Auto Shot draws the ranged weapon.
        gameHandler->setSpellCastBeginCallback([this](uint64_t caster, uint32_t spellId) {
            if (!gameHandler) return;
            if (caster == gameHandler->getPlayerGuid()) {
                if (appearanceComposer_) appearanceComposer_->onSpellCastBegin(spellId);
            } else if (entitySpawner_) {
                entitySpawner_->onUnitSpellCastBegin(caster, spellId);
            }
        });
        // A unit's ammunition (0x007206a0): from a ranged spell's start, or
        // the player's PLAYER_AMMO_ID.
        // In the view: the sphere round its model against the frustum.
        gameHandler->setUnitDrawnCallback([this](uint64_t guid) {
            glm::vec3 centre;
            float radius = 0.0f;
            if (!getRenderBoundsForGuid(guid, centre, radius)) return false;
            const auto* camera = renderer ? renderer->getCamera() : nullptr;
            if (!camera) return true;
            rendering::Frustum frustum;
            frustum.extractFromMatrix(camera->getViewProjectionMatrix());
            return frustum.intersectsSphere(centre, radius);
        });
        gameHandler->setUnitAmmoCallback([this](uint64_t guid, uint32_t value, uint32_t ask, bool isItemEntry) {
            if (!gameHandler || !entitySpawner_) return;
            uint32_t inst = 0;
            if (guid == gameHandler->getPlayerGuid()) inst = renderer ? renderer->getCharacterInstanceId() : 0;
            else inst = entitySpawner_->getCreatureInstanceId(guid);
            if (inst == 0) inst = entitySpawner_->getPlayerInstanceId(guid);
            if (inst == 0) return;
            entitySpawner_->showUnitAmmo(guid, inst, isItemEntry ? entitySpawner_->itemDisplayId(value) : value, ask);
        });
        // Any unit's swing draws its melee weapons (0x00756800).
        gameHandler->setUnitAttackSwingCallback([this](uint64_t attacker) {
            if (!gameHandler) return;
            if (attacker == gameHandler->getPlayerGuid()) {
                if (appearanceComposer_ && appearanceComposer_->sheathState() != core::SheathState::Melee)
                    appearanceComposer_->requestSheathState(core::SheathState::Melee);
            } else if (entitySpawner_) {
                entitySpawner_->onUnitAttackSwing(attacker);
            }
        });
        // A melee hit kept for its attacker's model, and an attack stopping
        // that settles it (rendering/melee_sound.hpp).
        gameHandler->setMeleeHitCallback([this](const game::AttackerStateUpdateData& hit) {
            if (entitySpawner_) entitySpawner_->onMeleeHit(hit);
        });
        gameHandler->setAttackStopCallback([this](uint64_t attacker) {
            if (entitySpawner_) entitySpawner_->onMeleeAttackStop(attacker);
        });
        gameHandler->setSpellMissCallback([this](uint64_t caster, uint64_t target, uint8_t missType, uint32_t spellId) {
            if (entitySpawner_) entitySpawner_->onSpellMiss(caster, target, missType, spellId);
        });
        connectSpellVisualHooks();
        // Whether a text emote is sent, and the player's weapons put
        // away when it is (0x006dd9e0).
        gameHandler->setTextEmoteGateCallback([this](uint32_t textEmoteId) {
            using Gate = game::GameHandler::TextEmoteGate;
            if (!appearanceComposer_) return Gate::Send;
            switch (appearanceComposer_->onTextEmote(textEmoteId)) {
                case core::TextEmoteVerdict::Refused: return Gate::Refuse;
                case core::TextEmoteVerdict::RefusedWhileMoving: return Gate::RefuseWhileMoving;
                default: playTextEmoteLocally(textEmoteId); return Gate::Send;
            }
        });
        // A player difficulty change shows the current map's loading screen
        // until it is done (0x0052c460: 6 -> 0x0040ab70, 9 -> 0x00409550).
        gameHandler->setDifficultyLoadingScreenCallback([this](bool show, uint32_t mapId) {
            difficultyLoadingScreen_.reset();
            if (!show || !window || !assetManager) return;
            auto screen = std::make_unique<rendering::LoadingScreen>();
            screen->setVkContext(window->getVkContext());
            screen->setSDLWindow(window->getSDLWindow());
            if (screen->initialize(assetManager.get(), mapId)) difficultyLoadingScreen_ = std::move(screen);
        });
        // The logout countdown finishing is not the end of it: the server
        // confirms with SMSG_LOGOUT_COMPLETE, and only then does the client
        // leave. Without this the countdown ran out and nothing happened.
        gameHandler->setLogoutCompleteCallback([this](bool exiting) {
            if (exiting) {
                // 0x00409550: the world's music and ambience stop.
                if (auto* ac = getAudioCoordinator()) ac->leaveWorld();
                LOG_INFO("Logout complete - quitting");
                if (window) window->setShouldClose(true);
            } else {
                LOG_INFO("Logout complete - returning to character select");
                logoutToCharacterSelection();
            }
        });
        // The extra action bars as the realm keeps them (PLAYER_FIELD_BYTES
        // byte 2): this client's own bars follow them, so a bar switched on
        // from another computer, or before a reinstall, comes back on. Mapped
        // as SetActionBarToggles maps them - bottom left onto showbar2, the
        // right bar onto showrightbar, right bar 2 onto showleftbar (page 4,
        // drawn at the left edge). Bottom right has no bar here to drive.
        gameHandler->setActionBarTogglesCallback([this](uint8_t bars) {
            if (!uiManager) return;
            auto& gs = uiManager->getGameScreen();
            auto& panel = gs.getSettingsPanel();
            bool changed = false;
            changed |= panel.setSettingValue("showbar2", (bars & 0x1u) ? "1" : "0");
            changed |= panel.setSettingValue("showrightbar", (bars & 0x4u) ? "1" : "0");
            changed |= panel.setSettingValue("showleftbar", (bars & 0x8u) ? "1" : "0");
            if (changed) gs.saveSettings();
        });
        gameHandler->setMoverStopCallback([this]() {
            if (renderer && renderer->getCameraController()) renderer->getCameraController()->stopForwardBack();
        });
        gameHandler->setKnockBackCallback([this](float vcos, float vsin, float hspeed, float vspeed) {
            if (renderer && renderer->getCameraController()) {
                renderer->getCameraController()->applyKnockBack(vcos, vsin, hspeed, vspeed);
            }
        });
        gameHandler->setCameraShakeCallback([this](uint32_t shakesId, uint32_t soundId) {
            auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr;
            if (!svs) return;
            const glm::vec3 at = renderer->getCharacterPosition();
            svs->playCameraShakes(shakesId, at);
            svs->playSoundAt(soundId, at);
        });
        // 0x006da130's DruidMorph_Impact_Base: the active player's own
        // sound - at the listener with it at the character, kept over a
        // crowd's.
        gameHandler->setPlayerSoundCallback([this](uint32_t soundId) {
            auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr;
            if (svs) svs->playOwnSoundAt(soundId, renderer->getCharacterPosition());
        });
        // Barber shop and other live appearance changes rebuild the model.
        gameHandler->setPlayerModelRebuildCallback([this]() {
            refreshPlayerCharacterModel();
        });
    }
    // The auras that came in while the world loaded found the spell visual
    // system without its resolvers, and showed nothing (0x00724820 plays the
    // kit as the slot fills); now that it has them, they are shown.
    if (gameHandler) {
        if (auto* spells = gameHandler->getSpellHandler()) spells->replayAuraStateKits();
    }
}

bool Application::setAssetExpansionOverride(const std::string& id) {
    if (id.empty() || id == "legacy") {
        assetExpansionOverrideId_ = id;
        return true;
    }
    if (!expansionRegistry_) return false;
    const auto* profile = expansionRegistry_->getProfile(id);
    if (!profile || !std::filesystem::exists(profile->dataPath + "/manifest.json")) {
        LOG_WARNING("Cannot select asset profile '", id,
                    "': no extracted manifest is available");
        return false;
    }
    assetExpansionOverrideId_ = id;
    return true;
}

/// Loads the four things that make a protocol profile: its opcode table, its
/// update field table, the packet parsers built for it, and its DBC layouts.
///
/// Read at startup and again whenever the expansion changes, and each of those
/// had its own copy. A table added to one and not the other is invisible until
/// somebody switches expansion, or until somebody does not: the fault appears
/// on exactly one of the two paths, and which one depends on which copy was
/// edited.
///
/// Each table is set active as it loads, because a failed load leaves the
/// previous expansion's table in place rather than none at all, and a wrong
/// table reads better than a null one.
void Application::loadExpansionTables(const game::ExpansionProfile& profile) {
    if (!gameHandler) return;

    const std::string opcodesPath = profile.dataPath + "/opcodes.json";
    if (!gameHandler->getOpcodeTable().loadFromJson(opcodesPath)) {
        LOG_ERROR("Failed to load opcodes from ", opcodesPath);
    }
    game::setActiveOpcodeTable(&gameHandler->getOpcodeTable());

    const std::string updateFieldsPath = profile.dataPath + "/update_fields.json";
    if (!gameHandler->getUpdateFieldTable().loadFromJson(updateFieldsPath)) {
        LOG_ERROR("Failed to load update fields from ", updateFieldsPath);
    }
    game::setActiveUpdateFieldTable(&gameHandler->getUpdateFieldTable());

    // An id with no parser set is a profile this build cannot read the wire
    // for. It still runs, on WotLK's parsers, because refusing to start would
    // break a realm running a hand-written profile that happens to be close
    // enough - but it says so, which is the half that was missing.
    auto packetParsers = game::createPacketParsers(profile.id);
    if (!packetParsers) {
        LOG_ERROR("No packet parsers for expansion '", profile.id,
                  "'. Falling back to WotLK, which will misparse movement and "
                  "update-object against any realm that is not 3.3.5a");
        packetParsers = std::make_unique<game::WotlkPacketParsers>();
    }
    gameHandler->setPacketParsers(std::move(packetParsers));

    if (dbcLayout_) {
        const std::string dbcLayoutsPath = profile.dataPath + "/dbc_layouts.json";
        if (!dbcLayout_->loadFromJson(dbcLayoutsPath)) {
            LOG_ERROR("Failed to load DBC layouts from ", dbcLayoutsPath);
        }
        pipeline::setActiveDBCLayout(dbcLayout_.get());
    }
}

void Application::reloadExpansionData() {
    if (!expansionRegistry_ || !gameHandler) return;
    auto* profile = expansionRegistry_->getActive();
    if (!profile) return;

    LOG_INFO("Reloading expansion data for: ", profile->name);

    loadExpansionTables(*profile);

    // Update expansion data path for CSV DBC lookups and clear DBC cache
    if (assetManager && !profile->dataPath.empty()) {
        const char* dataPathEnv = std::getenv("WOW_DATA_PATH");
        const std::string baseDataPath = dataPathEnv ? dataPathEnv : "./Data";
        const game::ExpansionProfile* assetProfile = profile;
        if (!assetExpansionOverrideId_.empty() &&
            assetExpansionOverrideId_ != "legacy") {
            if (const auto* selected = expansionRegistry_->getProfile(assetExpansionOverrideId_)) {
                assetProfile = selected;
            }
        }
        const std::string assetManifest = assetProfile->dataPath + "/manifest.json";
        const bool useLegacyAssets = assetExpansionOverrideId_ == "legacy";
        const std::string desiredAssetPath = !useLegacyAssets &&
                                                  std::filesystem::exists(assetManifest)
            ? assetProfile->dataPath
            : baseDataPath;
        if (desiredAssetPath != assetManager->getDataPath() &&
            assetManager->switchDataPath(desiredAssetPath) &&
            desiredAssetPath != baseDataPath) {
            assetManager->setBaseFallbackPath(baseDataPath, assetProfile->id);
        }
        LOG_INFO("Protocol profile '", profile->id, "' using asset source '",
                 useLegacyAssets ? std::string("legacy") : assetProfile->id, "'");
        assetManager->setExpansionDataPath(profile->dataPath);
        assetManager->clearDBCCache();
    }

    // Reset map name cache so it reloads from new expansion's Map.dbc
    if (worldLoader_) worldLoader_->resetMapNameCache();

    // Reset game handler DBC caches so they reload from new expansion data
    if (gameHandler) {
        gameHandler->resetDbcCaches();
    }

    // Rebuild creature display lookups with the new expansion's DBC layout
    if (entitySpawner_) entitySpawner_->rebuildLookups();
}

/// The world connection dropped under us.
///
/// Nothing watched for it. WorldState::DISCONNECTED was reached and both of the
/// AppState::DISCONNECTED arms were empty comments, so the client stayed in a
/// world it was no longer connected to - standing in a frozen scene, with the
/// only clue a warning in a log that is not shown.
///
/// The same path a logout takes, so the world is torn down the way it is meant
/// to be, and the reason is put on the login screen where WoW puts it.
void Application::handleWorldDisconnect() {
    if (state != AppState::IN_GAME) return;
    LOG_WARNING("Disconnected from the world server; returning to login");

    disconnectNotice_ = "You have been disconnected from the server.";
    logoutToLogin();
}

void Application::logoutToLogin() { leaveWorld(LeaveTo::Login); }

void Application::logoutToCharacterSelection() { leaveWorld(LeaveTo::CharacterSelection); }

void Application::leaveWorld(LeaveTo to) {
    if (renderingFrame_) {
        if (!leaveWorldPending_) {
            LOG_INFO("Logout requested during render; deferring until frame completes");
        }
        // The login screen, once asked for, stays what it is: the connection
        // is gone, or must go.
        if (!leaveWorldPending_ || to == LeaveTo::Login) leaveWorldPending_ = to;
        return;
    }

    performLeaveWorld(to);
}

bool Application::leaveWorldOutOfMemory(const char* where, const char* what, bool midFrame) {
    if (state != AppState::IN_GAME) return false;
    LOG_ERROR("Out of memory in the world (", where, ": ", what, "); back to the login screen");
    disconnectNotice_ = "Not enough memory to load the world.";
    if (midFrame) {
        // The frame stops where the exception left it: what it recorded is
        // thrown away and an empty frame goes in its place, so the next one
        // begins as after any other. The interface's frame is closed too.
        renderingFrame_ = false;
        leaveWorldPending_.reset();
        if (renderer) renderer->abandonFrame();
        if (ImGuiContext* g = ImGui::GetCurrentContext(); g && g->WithinFrameScope) ImGui::EndFrame();
    }
    logoutToLogin();
    return true;
}

void Application::processDeferredLeaveWorld() {
    if (!leaveWorldPending_) return;
    const LeaveTo to = *leaveWorldPending_;
    leaveWorldPending_.reset();
    performLeaveWorld(to);
}

std::vector<rendering::MemoryEntry> Application::memoryExtras(bool collectLuaFirst) {
    std::vector<rendering::MemoryEntry> extra;
    char name[192];
    if (addonManager_) {
        if (auto* engine = addonManager_->getLuaEngine(); engine && engine->getState()) {
            lua_State* L = engine->getState();
            if (collectLuaFirst) lua_gc(L, LUA_GCCOLLECT, 0);
            const uint64_t kb = static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNT, 0));
            const uint64_t rest = static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNTB, 0));
            extra.push_back({"Lua state", 0, kb * 1024 + rest});
        }
    }
    const auto wav = audio::AudioEngine::decodedCacheStats();
    std::snprintf(name, sizeof(name), "Decoded sounds (%zu)", wav.sounds);
    extra.push_back({name, 0, wav.bytes});
    if (audioCoordinator_) {
        if (auto* music = audioCoordinator_->getMusicManager()) {
            std::snprintf(name, sizeof(name), "Music files kept (%zu)", music->cachedMusicCount());
            extra.push_back({name, 0, music->cachedMusicBytes()});
        }
    }
    if (entitySpawner_) entitySpawner_->memoryReport(extra);
    return extra;
}

void Application::logMemory(const char* stage, const std::string& what) {
    if (!renderer) return;
    const auto lines = memoryGrowth_.note(stage, renderer->memoryOwners(memoryExtras(false)));
    LOG_WARNING("Memory, ", what, " (entities ",
                gameHandler ? gameHandler->getEntityManager().getEntities().size() : size_t{0}, "):");
    for (const std::string& line : lines) LOG_WARNING("Memory [", stage, "]: ", line);
}

void Application::fireInterfaceLeaving() {
    // The camera's distance and pitch, kept for the next time the world is
    // entered (0x005ff320 writes cameraSavedDistance and cameraSavedPitch).
    if (renderer && renderer->getCameraController()) {
        const auto* cam = renderer->getCameraController();
        addons::setStoredCVar("cameraSavedDistance", std::to_string(cam->savedDistance()));
        addons::setStoredCVar("cameraSavedPitch", std::to_string(-cam->savedPitch()));
        addons::setStoredCVar("cameraSavedVehicleDistance", std::to_string(cam->savedVehicleDistance()));
    }
    if (!addonManager_) return;
    {
        // A player still in combat is first told the fight is over
        // (0x00528f00: the player there is, UNIT_FLAG_IN_COMBAT in
        // UNIT_FIELD_FLAGS, event 0x99, then 0x00528c30). Nothing else ends
        // the fight for an interface on its way out, so an addon that locked
        // itself on PLAYER_REGEN_DISABLED saved its state still locked.
        if (gameHandler && gameHandler->playerFights()) addonManager_->fireEvent("PLAYER_REGEN_ENABLED");
        // 0x00528c30 ends with INSTANCE_LOCK_STOP (0x279), which closes a
        // pending instance-bind prompt, then PLAYER_LEAVING_WORLD (0x100).
        addonManager_->fireEvent("INSTANCE_LOCK_STOP");
        addonManager_->fireEvent("PLAYER_LEAVING_WORLD");
        // Then PLAYER_LOGOUT, and the saved variables after it (0x00528f00:
        // 0x00528c30, event 0xfe, 0x00527130). It fired when the server
        // accepted the logout, twenty seconds early and also for a logout
        // then cancelled.
        addonManager_->fireEvent("PLAYER_LOGOUT");
    }
}

void Application::unloadInterface() {
    if (addonManager_ && addonsLoaded_) {
        fireInterfaceLeaving();
        // Saves the saved variables on its way out, so this has to run while
        // the outgoing character is still the one named - the per-character
        // files are keyed by that name, and writing them after it changed
        // would put one character's variables in another's file.
        addonManager_->unloadAll();
        // And now it goes, so nothing can be written under it by accident.
        // The next world entry sets the name of whoever logged in before it
        // loads anything; an unset name skips the per-character files rather
        // than guessing at one.
        addonManager_->setCharacterName("");
    }
    addonsLoaded_ = false;
}

void Application::performLeaveWorld(LeaveTo to) {
    LOG_INFO("Logout requested, to the ", to == LeaveTo::Login ? "login screen" : "character selection");
    if (cinematicPlayer_) cinematicPlayer_->abandon();

    // The interface leaves the world with the character that was wearing it.
    //
    // Here rather than at character select, which is where this used to
    // happen: leaving it standing until then meant the interface outlived the
    // session it belonged to, dormant but intact, and another account or
    // character logged in to would have found it.
    //
    // Before the disconnect below, so PLAYER_LEAVING_WORLD reaches handlers
    // while there is still a world for them to ask about, which is what the
    // event is for.
    unloadInterface();

    // Disconnect TransportManager from WMORenderer before tearing down
    if (gameHandler && gameHandler->getTransportManager()) {
        gameHandler->getTransportManager()->setWMORenderer(nullptr);
    }

    // The connection stays where the server has only let the character out
    // of the world (0x006b2180); one that is gone meanwhile is the login
    // screen's (0x004da9d0).
    if (to == LeaveTo::CharacterSelection && !(gameHandler && gameHandler->leaveWorldForCharacterList()))
        to = LeaveTo::Login;
    if (gameHandler && to == LeaveTo::Login) {
        gameHandler->disconnect();
    }

    // The world's sound goes with it (0x00409550): the music and the
    // ambience. Nothing updates them past this point, so a loop left running
    // here played on over the login screen. The next entry from the
    // character screen lets every intro play again (0x00405540 ->
    // 0x004c9330).
    if (audioCoordinator_) {
        audioCoordinator_->leaveWorld();
        audioCoordinator_->forgetIntroMusic();
    }

    // --- Per-session flags ---
    npcsSpawned = false;
    playerCharacterSpawned = false;
    wasAutoAttacking_ = false;
    if (worldLoader_) worldLoader_->resetLoadedMap();
    if (worldEntryCallbacks_) worldEntryCallbacks_->resetState();
    facingSendCooldown_ = 0.0f;
    lastSentCanonicalYaw_ = kFacingFromServer;
    idleYawned_ = false;

    splineCarriedLastFrame_ = 0;

    // --- Player identity ---
    spawnedPlayerGuid_ = 0;
    spawnedAppearanceBytes_ = 0;
    spawnedFacialFeatures_ = 0;

    if (renderer && renderer->getVkContext() && !renderer->getVkContext()->isDeviceLost()) {
        LOG_DEBUG("Waiting for GPU idle before logout scene cleanup...");
        ::wowee::rendering::deviceWaitIdle(renderer->getVkContext()->getDevice());
    }

    // The session's transports go with it.
    if (gameHandler && gameHandler->getTransportManager())
        gameHandler->getTransportManager()->clearTransports("logout");

    // --- Reset all EntitySpawner state (mount, creatures, players, GOs, queues, caches) ---
    if (entitySpawner_) entitySpawner_->resetAllState();

    world.reset();

    if (renderer) {
        renderer->resetCombatVisualState();
        // Remove old player model so it doesn't persist into next session
        if (auto* charRenderer = renderer->getCharacterRenderer()) {
            charRenderer->removeInstance(1);
        }
        // Clear all world geometry renderers
        if (auto* wmo = renderer->getWMORenderer()) {
            wmo->clearInstances();
        }
        if (auto* m2 = renderer->getM2Renderer()) {
            m2->clear();
        }
        // The loops the doodads' and game objects' events started: no update
        // runs at the character screen to see their owners gone, so they
        // would play on over it.
        renderer->stopModelEventSounds();
        // Clear terrain tile tracking + water surfaces so next world entry starts fresh.
        // softReset() rather than a full teardown: it clears the tile data
        // without blocking on worker thread joins.
        if (auto* terrain = renderer->getTerrainManager()) {
            terrain->softReset();
        }
        if (auto* questMarkers = renderer->getQuestGiverMarkers()) {
            questMarkers->clear();
        }
        if (auto* footprints = renderer->getFootprintRenderer()) {
            footprints->clear();
        }
        if (auto* ac = renderer->getAnimationController()) ac->clearMount();
        renderer->setCharacterFollow(0);
    }

    // Clear stale realm/character selection so switching servers starts fresh
    if (uiManager) {
        if (to == LeaveTo::Login) uiManager->getRealmScreen().reset();
        uiManager->getCharacterScreen().reset();
    }
    setState(to == LeaveTo::Login ? AppState::AUTHENTICATION : AppState::CHARACTER_SELECTION);

    // Said on the screen the player lands on, because there is nowhere to say
    // it on the way out: the world is being torn down as this runs.
    if (!disconnectNotice_.empty() && uiManager) {
        uiManager->getAuthScreen().setStatus(disconnectNotice_, true, /*prominent=*/true);
        disconnectNotice_.clear();
    }
}

// One frame of being in the world.
//
// The largest arm of update()'s state switch by a wide margin: input, the
// camera, the entity syncs that keep render instances on top of what the server
// says, the quest markers, the sheathe toggle, the spawn retries.
//
// updateCheckpoint travels by reference because the caller's catch reports it:
// an exception here has to say where here was.
// Everything the server says about how the player moves, handed to the camera.
//
// Run, walk, swim, flight and turn speeds, rooting, gravity, feather fall,
// water walking, and which of them the current mount, taxi or transport
// overrides. The camera owns the movement, so this is where the server's word
// about it lands.
void Application::applyServerMovementState(float deltaTime) {
    if (renderer && gameHandler && renderer->getCameraController()) {
        // The active mover's speeds: the player's, or a driven vehicle's own.
        const game::MovementSpeeds speeds = gameHandler->moverSpeeds();
        renderer->getCameraController()->setRunSpeedOverride(speeds.run);
        renderer->getCameraController()->setWalkSpeedOverride(speeds.walk);
        renderer->getCameraController()->setWalkMode(gameHandler->isWalkMode());
        renderer->getCameraController()->setSwimSpeedOverride(speeds.swim);
        renderer->getCameraController()->setSwimBackSpeedOverride(speeds.swimBack);
        renderer->getCameraController()->setFlightSpeedOverride(speeds.flight);
        renderer->getCameraController()->setFlightBackSpeedOverride(speeds.flightBack);
        renderer->getCameraController()->setRunBackSpeedOverride(speeds.runBack);
        renderer->getCameraController()->setTurnRateOverride(speeds.turnRate);
        // Nor does a dead one move by its keys: the server's root goes at a
        // teleport to another map, and the dead character walked.
        const auto moverEntity = gameHandler->getEntityManager().getEntity(gameHandler->activeMover());
        const auto* moverUnit =
            moverEntity && moverEntity->isUnit() ? static_cast<const game::Unit*>(moverEntity.get()) : nullptr;
        bool keysMove = true, keysTurn = true, mouseTurns = true;
        if (moverUnit) {
            uint32_t animation = 0;
            float timeMs = 0.0f, durationMs = 0.0f;
            const uint32_t instance = entitySpawner_ ? entitySpawner_->unitAnimationInstance(moverUnit->getGuid()) : 0;
            if (auto* cr = renderer->getCharacterRenderer(); !cr || instance == 0 ||
                                                             !cr->getAnimationState(instance, animation, timeMs, durationMs))
                animation = 0;
            const auto health = static_cast<int32_t>(moverUnit->getHealth());
            // The player's own as its keys have it already (0x006dcb40), the dead's
            // as the server says.
            constexpr uint8_t kSits = 1, kStandDead = 7;
            uint8_t stand = moverUnit->getStandState();
            if (moverUnit->getGuid() == gameHandler->getPlayerGuid() && gameHandler->getStandState() != kStandDead)
                stand = renderer->getCameraController()->isSitting() ? kSits : 0;
            keysMove = game::keysMayMove(health, stand, animation);
            // Stunned it does not turn either, nor does the mouse turn it sitting.
            keysTurn = game::keysMayTurn(health, moverUnit->getUnitFlags(), animation);
            mouseTurns = game::mouseMayTurn(health, moverUnit->getUnitFlags(), stand, animation);
        }
        renderer->getCameraController()->setMovementRooted(gameHandler->isPlayerRooted());
        renderer->getCameraController()->setMoverKeysMove(keysMove);
        renderer->getCameraController()->setMoverTurns(keysTurn, mouseTurns);
        // The mover's flags2 hold its strafe and jump keys.
        const auto held = game::vehicle::moverKeyRestrictions(gameHandler->getMovementInfo().flags2);
        renderer->getCameraController()->setMoverKeyRestrictions(held.noStrafe, held.noJump);
        // A mover held for its seat moves by no key (0x0074ba40).
        renderer->getCameraController()->setMoverHeld(gameHandler->moverHeldBySeat());
        renderer->getCameraController()->setGravityDisabled(gameHandler->isGravityDisabled());
        renderer->getCameraController()->setFeatherFallActive(gameHandler->isFeatherFalling());
        renderer->getCameraController()->setWaterWalkActive(gameHandler->isWaterWalking());
        renderer->getCameraController()->setMoverGhost(gameHandler->isPlayerGhost() &&
                                                       gameHandler->activeMover() == gameHandler->getPlayerGuid());
        // Flight physics engage on the CAN_FLY ability (flying mount
        // or .gm fly), not isPlayerFlying() which also needs the
        // FLYING flag the client only sets once already airborne.
        renderer->getCameraController()->setFlyingActive(gameHandler->canFly());
        // How deep the water must be to swim in, and how deep it treads.
        if (entitySpawner_)
            renderer->getCameraController()->setCollisionHeight(playerCollisionHeight());
        if (entitySpawner_)
            renderer->getCameraController()->setPortHalfWidth(entitySpawner_->playerCollisionHalfWidth());
        if (entitySpawner_)
            renderer->getCameraController()->setPortStepHeight(entitySpawner_->playerStepHeight());
        // And whether the player is up in it, which the camera worked out
        // last frame: take-off and landing are decided where the floor is.
        gameHandler->setFlightAirborne(renderer->getCameraController()->isFlightAirborne());
        renderer->getCameraController()->setHoverActive(gameHandler->isHovering());
        // The follow smoothing's styles (0x00602760 reads both CVars).
        renderer->getCameraController()->setSmoothStyles(
            std::atoi(addons::storedCVarValue("cameraSmoothStyle", "4").c_str()),
            std::atoi(addons::storedCVarValue("cameraSmoothTrackingStyle", "4").c_str()));
        renderer->getCameraController()->setWaterCollision(
            std::atoi(addons::storedCVarValue("cameraWaterCollision", "1").c_str()) != 0);
        renderer->getCameraController()->setSmartPivot(
            std::atoi(addons::storedCVarValue("cameraPivot", "1").c_str()) != 0,
            static_cast<float>(std::atof(addons::storedCVarValue("cameraPivotDXMax", "0.05").c_str())),
            static_cast<float>(std::atof(addons::storedCVarValue("cameraPivotDYMin", "0.00").c_str())));

        // Sync camera forward pitch to movement packets during flight / swimming.
        // The server writes the pitch field when FLYING or SWIMMING flags are set;
        // without this sync it would always be 0 (horizontal), causing other
        // players to see the character flying flat even when pitching up/down.
        if (gameHandler->isPlayerFlying()) {
            // The mount's pitch, which the camera sets only while it steers -
            // orbiting it to look around does not tilt the mount or change
            // what the server is told.
            const float pitchRad = renderer->getCameraController()->getFlightPitchRad();
            gameHandler->setMovementPitch(pitchRad);
            // Tilt the mount/character model to match flight direction
            // (taxi flight uses setTaxiOrientationCallback for this instead)
            if (gameHandler->isMounted()) {
                if (auto* ac = renderer->getAnimationController()) ac->setMountPitchRoll(pitchRad, 0.0f);
            }
        } else if (gameHandler->isSwimming()) {
            // A swimmer's likewise, or as click-to-move pitches it
            // (0x0072f1f0): what it swims at.
            gameHandler->setMovementPitch(renderer->getCameraController()->getSwimPitchRad());
        } else if (gameHandler->isMounted()) {
            // Level on the ground; swimming, the renderer tilts it with the
            // body (swim_pitch).
            if (auto* ac = renderer->getAnimationController()) ac->setMountPitchRoll(0.0f, 0.0f);
        }
    }

    bool onTaxi = gameHandler &&
                  (gameHandler->isOnTaxiFlight() ||
                   gameHandler->isTaxiMountActive() ||
                   gameHandler->isTaxiActivationPending());
    // Deliberately narrower than onTaxi: only true once the flight is
    // actually happening (mounted/flying), not merely pending a reply.
    // A rejected CMSG_ACTIVATETAXI sets isTaxiActivationPending() true
    // for one or two frames and then clears it - if that alone counted
    // as "was taxiing", the landing clamp below would arm on every
    // rejected activation and snap the player onto whatever floor
    // candidate is closest to their current (never-actually-flown-from)
    // position, exactly as if a real flight had just landed. Live-hit
    // this at Booty Bay (multi-level WMO): a rejected activation while
    // standing on the upper platform snapped the character down to the
    // level below, with a "Cannot take that flight path" chat message
    // and zero actual movement in between.
    bool actuallyFlying = gameHandler &&
                          (gameHandler->isOnTaxiFlight() ||
                           gameHandler->isTaxiMountActive());
    bool onTransportNow = gameHandler && gameHandler->isOnTransport();
    // Clear stale client-side transport state when the tracked transport no longer exists.
    if (onTransportNow && gameHandler->getTransportManager()) {
        auto* currentTracked = gameHandler->getTransportManager()->getTransport(
            gameHandler->getPlayerTransportGuid());
        if (!currentTracked && !gameHandler->hasPendingPlayerTransportWorldTransfer()) {
            gameHandler->clearPlayerTransport();
            onTransportNow = false;
        }
    }
    // M2 transports (trams) use position-delta approach: player keeps normal
    // movement and the transport's frame-to-frame delta is applied on top.
    // Only WMO transports (ships) use full external-driven mode.
    bool isM2Transport = false;
    if (onTransportNow && gameHandler->getTransportManager()) {
        auto* tr = gameHandler->getTransportManager()->getTransport(gameHandler->getPlayerTransportGuid());
        isM2Transport = (tr && tr->isM2);
    }
    bool onWMOTransport = onTransportNow && !isM2Transport;
    if (worldEntryCallbacks_ && worldEntryCallbacks_->getWorldEntryMovementGraceTimer() > 0.0f) {
        worldEntryCallbacks_->setWorldEntryMovementGraceTimer(
            worldEntryCallbacks_->getWorldEntryMovementGraceTimer() - deltaTime);
        // Clear stale movement from before teleport each frame
        // until grace period expires (keys may still be held)
        if (renderer && renderer->getCameraController())
            renderer->getCameraController()->clearMovementInputs();
    }
    // Hearth teleport: delegated to WorldEntryCallbackHandler
    if (worldEntryCallbacks_) {
        worldEntryCallbacks_->update(deltaTime);
    }
    // A server move spline carries the player as it does any unit
    // (SMSG_MONSTER_MOVE, 0x0073c8e0) - Charge's run among them - and the
    // keys do not move it meanwhile. Held one frame past its end, for the
    // player to be put where it ends.
    //
    // The player as the unit the camera looks at, as 0x00606f90 asks it, not
    // as the active mover. Fear and charm take the player's control first
    // (SMSG_CLIENT_CONTROL_UPDATE 0), and that leaves no mover at all
    // (0x0072cca0) - so a feared player's runs were taken for nobody's: the
    // character stood where the fear found it while the server ran it about,
    // and the camera never took its Fear state. A seat moves its passenger
    // (the vehicle's transport), not a spline of the player's.
    const uint64_t playerGuid = gameHandler ? gameHandler->getPlayerGuid() : 0;
    const bool seatedPassenger = entitySpawner_ && entitySpawner_->localPassengerState().has_value();
    const bool carriedBySpline =
        gameHandler && rendering::camera_smooth::playerOnServerSpline(gameHandler->onMoveSpline(playerGuid), onTaxi,
                                                                      seatedPassenger, gameHandler->cameraTarget(),
                                                                      playerGuid);
    // The unit the keys drive in the player's stead (the active mover,
    // DAT_00ca1238): its movement is the camera's, and the player rides it.
    // None moves while there is no mover.
    const uint64_t activeMover = gameHandler ? gameHandler->activeMover() : 0;
    uint64_t drivenUnit = 0;
    std::shared_ptr<game::Entity> drivenEntity;
    if (activeMover != 0 && activeMover != playerGuid) {
        drivenEntity = gameHandler->getEntityManager().getEntity(activeMover);
        if (drivenEntity) drivenUnit = activeMover;
    }
    // Its own move spline carries it as the player's does the player.
    const uint64_t splineUnit =
        gameHandler ? game::vehicle::splineCarriedUnit(playerGuid, carriedBySpline, drivenUnit,
                                                       drivenUnit != 0 && gameHandler->onMoveSpline(drivenUnit),
                                                       gameHandler->vehicleRoster().passenger(drivenUnit).has_value())
                    : 0;
    const uint64_t characterUnit = drivenUnit != 0 ? drivenUnit : playerGuid;
    const bool splineCarried = splineUnit != 0 || (splineCarriedLastFrame_ != 0 && splineCarriedLastFrame_ == characterUnit);
    splineCarriedLastFrame_ = splineUnit;
    if (renderer && renderer->getCameraController()) {
    // A ship carries the player's reference frame, but it must not
    // freeze local controls like a taxi flight. On-deck walking is
    // folded into the transport-local offset in the sync block below.
    const bool externallyDrivenMotion = onTaxi || splineCarried;
    // Keep physics frozen (externalFollow) during landing clamp when terrain
    // hasn't loaded yet - prevents gravity from pulling player through void.
    bool hearthFreeze = worldEntryCallbacks_ && worldEntryCallbacks_->isHearthTeleportPending();
    const bool transportTransferFreeze = gameHandler &&
        gameHandler->hasPendingPlayerTransportWorldTransfer();
    bool landingClampActive = !onTaxi && worldEntryCallbacks_ && worldEntryCallbacks_->getTaxiLandingClampTimer() > 0.0f &&
                              worldEntryCallbacks_->getWorldEntryMovementGraceTimer() <= 0.0f &&
                              !gameHandler->isMounted();
    // A passenger moves with its seat and not by itself: its own movement
    // stops (0x007489c0 -> 0x005fbbc0 as it sits and as it is let go), and
    // the camera looks at it where it is drawn on the vehicle.
    const auto seatState = entitySpawner_ ? entitySpawner_->localPassengerState() : std::nullopt;
    const bool vehiclePassenger = seatState.has_value();
    const bool noMover = activeMover == 0;
    {
        auto* cc = renderer->getCameraController();
        const bool possessing = drivenUnit != 0 && gameHandler->moverPossessed();
        if (drivenUnit != drivenUnitLastFrame_) {
            // A new mover's keys start let go (0x00729010 -> 0x005fbbc0); the
            // camera's character is the driven unit where it stands and
            // faces, turned with it.
            cc->clearMovementInputs();
            cc->newMover();
            if (drivenEntity) {
                renderer->getCharacterPosition() = core::coords::canonicalToRender(
                    glm::vec3(drivenEntity->getX(), drivenEntity->getY(), drivenEntity->getZ()));
                const float facing = core::coords::canonicalToCharacterYawDeg(drivenEntity->getOrientation());
                cc->turnWithVehicle(facing - cc->getFacingYaw());
                cc->setFacingYaw(facing);
                renderer->setCharacterYaw(facing);
            } else if (activeMover == gameHandler->getPlayerGuid() && !drivenPossessedLastFrame_) {
                // Out of what it drove, at its seat's place.
                const auto& own = gameHandler->getMovementInfo();
                renderer->getCharacterPosition() = core::coords::canonicalToRender(glm::vec3(own.x, own.y, own.z));
            }
            drivenUnitLastFrame_ = drivenUnit;
        }
        if (drivenPossessedLastFrame_ && activeMover == gameHandler->getPlayerGuid()) {
            // Back from a unit it possessed: the camera's character is the
            // player again, where it stood and as it faced - the camera
            // target goes back to the player (0x006e2880 -> 0x005186a0) and
            // the keys move it from there.
            cc->clearMovementInputs();
            cc->newMover();
            const auto& own = gameHandler->getMovementInfo();
            renderer->getCharacterPosition() = core::coords::canonicalToRender(glm::vec3(own.x, own.y, own.z));
            const float facing = core::coords::canonicalToCharacterYawDeg(own.orientation);
            cc->turnWithVehicle(facing - cc->getFacingYaw());
            cc->setFacingYaw(facing);
            renderer->setCharacterYaw(facing);
        }
        // Held while the possessed unit is the mover, and while the mover is
        // between it and the player (none, or its unit gone from view).
        const bool holdPlayer = gameHandler->moverPossessed() ||
                                (drivenPossessedLastFrame_ && activeMover != gameHandler->getPlayerGuid());
        if (drivenUnit != 0 || activeMover == gameHandler->getPlayerGuid()) drivenPossessedLastFrame_ = possessing;
        // The player's own model stays where it stood while the camera looks
        // through the unit it possesses (the camera's +0x88 is the farsight
        // object, 0x006e2880 -> 0x004f6f50), and the camera measures that
        // unit's model for its pivot.
        if (holdPlayer) {
            const auto& own = gameHandler->playerMovementInfo();
            renderer->setHeldCharacter(rendering::Renderer::HeldCharacter{
                .position = core::coords::canonicalToRender(glm::vec3(own.x, own.y, own.z)),
                .yawDeg = core::coords::canonicalToCharacterYawDeg(own.orientation)});
        } else {
            renderer->setHeldCharacter(std::nullopt);
        }
        cc->setFollowedUnitInstance(possessing ? gameHandler->resolveUnitRenderInstance(drivenUnit) : 0);
        const int stateNow = seatState ? static_cast<int>(*seatState) : 0;
        if (stateNow != localSeatStateLastFrame_ &&
            (stateNow == static_cast<int>(game::vehicle::SeatState::Seated) || stateNow == 0)) {
            cc->clearMovementInputs();
        }
        localSeatStateLastFrame_ = stateNow;
        // What its vehicle camera looks at, and the camera turned with its
        // facing (VehicleCamera_C, 0x00604490) - on past the passenger
        // record, to the end of its way out (0x0075af40).
        const auto view = entitySpawner_ ? entitySpawner_->localVehicleCameraView(
                                               cc->isTurningLeft() || cc->isTurningRight(), cc->isFreeLooking())
                                         : std::nullopt;
        cc->setVehiclePivot(view ? std::optional<glm::vec3>(view->pivot) : std::nullopt);
        cc->setVehicleCollisionOrigin(view ? view->collisionOrigin : std::nullopt);
        // The vehicle the camera's closeness fades in the player's stead
        // (0x004f8660, 0x00606f90).
        if (const auto fade = entitySpawner_ ? entitySpawner_->localCameraFadeVehicle() : std::nullopt) {
            cc->setCameraFadeTarget(rendering::CameraController::CameraFadeTarget{
                .instanceId = fade->instanceId, .scalars = fade->scalars, .riders = fade->riders});
        } else {
            cc->setCameraFadeTarget(std::nullopt);
        }
        // The camera turns with that facing, the driver's too: its keys turn
        // the vehicle, and the camera follows it as it is chased (0x00604490).
        cc->setYawFollowsVehicleCamera(view.has_value() && drivenUnit != 0);
        if (view && view->turned != 0.0f) cc->turnWithVehicle(glm::degrees(view->turned));
        // The seat's zoom when its camera move is due (0x0074c0e0).
        if (auto cue = entitySpawner_ ? entitySpawner_->takeSeatCameraCue() : std::nullopt) {
            // The distance mode first (0x0074c0e0 -> 0x00600590), then the
            // vehicle's yaw and pitch offsets over the same time, none out
            // of a seat (0x005fe890, 0x005fe8d0).
            const auto mode = game::vehicle::seatCameraMode(cue->seated, cue->seat, seatZoomRemembered_.has_value());
            cc->setCameraDistanceMode(mode.mode, cue->seconds, mode.animate, seatZoomRemembered_);
            const bool offsets = cue->seated && cue->vehicle;
            cc->setVehicleCameraOffsets(offsets ? cue->vehicle->cameraYawOffset : 0.0f,
                                        offsets ? cue->vehicle->cameraPitchOffset : 0.0f, cue->seconds);
            const auto zoom = game::vehicle::seatCameraZoom(cue->seated, cue->seat, cc->getUserTargetDistance(),
                                                            seatZoomRemembered_.has_value());
            cc->setSeatZoomLimits(zoom.minDistance, zoom.maxDistance);
            if (zoom.zoomTo) {
                if (zoom.remember) seatZoomRemembered_ = cc->getUserTargetDistance();
                cc->zoomOverTime(*zoom.zoomTo, cue->seconds);
            }
            if (zoom.restore && seatZoomRemembered_) {
                cc->zoomOverTime(*seatZoomRemembered_, cue->seconds);
                seatZoomRemembered_.reset();
            }
        }
        // No vehicle, no seat change to come: no vehicle offsets turning
        // the camera on foot.
        if (!vehiclePassenger && drivenUnit == 0 && entitySpawner_ && !entitySpawner_->seatCameraCuePending()) {
            cc->releaseVehicleCameraOffsets();
        }
    }
    renderer->getCameraController()->setExternalFollow(
        externallyDrivenMotion || landingClampActive || hearthFreeze ||
        transportTransferFreeze || deckFloorPending_ || (vehiclePassenger && drivenUnit == 0) || noMover);
    renderer->getCameraController()->setExternalMoving(externallyDrivenMotion);
    // The camera's Fear state (camera_smooth::stateFor): a server spline
    // carrying the player, not a flight path's.
    renderer->getCameraController()->setServerMoved(carriedBySpline);
    if (externallyDrivenMotion) {
        // Drop any stale local movement toggles while server drives taxi motion.
        renderer->getCameraController()->clearMovementInputs();
        if (worldEntryCallbacks_) worldEntryCallbacks_->setTaxiLandingClampTimer(0.0f);
    }
    if (worldEntryCallbacks_ && worldEntryCallbacks_->getLastTaxiFlight() && !onTaxi) {
        renderer->getCameraController()->clearMovementInputs();
        // Keep clamping until terrain loads at landing position.
        // Timer only counts down once a valid floor is found.
        //
        // Before WotLK, where the flight is this client's own and ends on the
        // node. The 3.3.5 client has nothing of the kind: the unit falls from
        // its spline's last point (0x006eae70 -> 0x0098b710,
        // MovementHandler::updateMoverSplineDone), and the step that does it
        // lands it.
        if (worldEntryCallbacks_ && game::isPreWotlk()) {
            worldEntryCallbacks_->setTaxiLandingClampTimer(2.0f);
            // Capture where the flight itself left the player, before terrain/WMO
            // streaming has a chance to move things around - this is the ground
            // truth the floor-selection below picks the closest candidate to.
            if (renderer) {
                worldEntryCallbacks_->setTaxiLandingReferenceZ(renderer->getCharacterPosition().z);
            }
        }
    }
    if (landingClampActive) {
        if (renderer && gameHandler) {
            glm::vec3 p = renderer->getCharacterPosition();
            std::optional<float> terrainFloor;
            std::optional<float> wmoFloor;
            std::optional<float> m2Floor;
            if (renderer->getTerrainManager()) {
                terrainFloor = renderer->getTerrainManager()->getHeightAt(p.x, p.y);
            }
            // Ground truth for this landing: where the flight itself left
            // the player, captured once when the clamp armed. Both the probe
            // height and the candidate selection below key off it.
            const float referenceZ = worldEntryCallbacks_
                ? worldEntryCallbacks_->getTaxiLandingReferenceZ() : p.z;

            // Probe from a height that does not move, because this clamp
            // rewrites p.z every frame and the structure query only looks
            // for candidates within roughly ten yards of where it is asked.
            //
            // Probing p.z + 40 therefore only found a floor while the
            // player was still far below it. Snapping them onto it lifted
            // the probe out of range, the floor stopped being reported, the
            // clamp fell back to terrain and dropped them again - landing
            // at Booty Bay flip-flopped between the WMO deck at 36.5 and
            // the terrain at 4.5 twelve times over, and whichever the last
            // frame chose is where the player was abandoned as the timer
            // ran out, which is how they ended up thrown up and then under
            // the structure.
            //
            // referenceZ is captured once when the clamp arms, from where
            // the flight itself left the player, and is already the value
            // the selection below measures candidates against. Anchoring
            // the probe to it keeps the candidate set the same every frame,
            // so the choice is stable and the streaming re-query it exists
            // for still works.
            constexpr float kLandingProbeAbove = 2.0f;
            const float probeZ = referenceZ + kLandingProbeAbove;
            if (renderer->getWMORenderer()) {
                wmoFloor = renderer->getWMORenderer()->getFloorHeight(p.x, p.y, probeZ);
            }
            if (renderer->getM2Renderer()) {
                // Include M2 floors (bridges/platforms) in landing recovery.
                m2Floor = renderer->getM2Renderer()->getFloorHeight(p.x, p.y, probeZ);
            }

            // Pick whichever floor candidate is closest to where the taxi flight
            // itself left the player, rather than unconditionally preferring
            // WMO/M2 over terrain. Unconditionally preferring WMO/M2 fixed
            // underground landings (terrain has no notion of being underground -
            // e.g. a WMO tunnel beneath a mountain, like Ironforge's flight point,
            // where terrainFloor=769 the mountain surface beat the correct
            // wmoFloor=502 the tunnel floor with "highest wins"), but could just as
            // easily snap an *outdoor* landing down onto an unrelated structure
            // sitting underneath it. referenceZ - captured once when the clamp
            // armed, from wherever the flight simulation actually left the player -
            // is ground truth for which candidate is plausible.
            std::optional<float> targetFloor;
            const char* pickedFrom = "none";
            float bestDist = std::numeric_limits<float>::max();
            const std::pair<std::optional<float>, const char*> floorCandidates[] = {
                {wmoFloor, "wmo"}, {m2Floor, "m2"}, {terrainFloor, "terrain"}};
            for (const auto& [candidate, name] : floorCandidates) {
                if (!candidate) continue;
                float dist = std::abs(*candidate - referenceZ);
                if (dist < bestDist) {
                    bestDist = dist;
                    targetFloor = candidate;
                    pickedFrom = name;
                }
            }

            LOG_INFO("Taxi landing clamp: pos=(", p.x, ", ", p.y, ", ", p.z, ") ",
                     "referenceZ=", referenceZ, " probeZ=", probeZ, " ",
                     "terrainFloor=", terrainFloor ? std::to_string(*terrainFloor) : "none", " ",
                     "wmoFloor=", wmoFloor ? std::to_string(*wmoFloor) : "none", " ",
                     "m2Floor=", m2Floor ? std::to_string(*m2Floor) : "none", " ",
                     "picked=", pickedFrom, " ",
                     "timer=", worldEntryCallbacks_ ? worldEntryCallbacks_->getTaxiLandingClampTimer() : 0.0f);

            if (targetFloor) {
                // Floor found - snap player to it and start countdown to release
                float targetZ = *targetFloor + 0.10f;
                if (std::abs(p.z - targetZ) > 0.05f) {
                    LOG_INFO("Taxi landing clamp: snapping z ", p.z, " -> ", targetZ,
                             " (source=", pickedFrom, ")");
                    p.z = targetZ;
                    renderer->getCharacterPosition() = p;
                    glm::vec3 canonical = core::coords::renderToCanonical(p);
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
                    gameHandler->sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
                }
                float clampTimer = worldEntryCallbacks_ ? worldEntryCallbacks_->getTaxiLandingClampTimer() : 0.0f;
                clampTimer -= deltaTime;
                if (worldEntryCallbacks_) worldEntryCallbacks_->setTaxiLandingClampTimer(clampTimer);
            }
            // No floor found: don't decrement timer, keep player frozen until terrain loads
        }
    }
    bool idleOrbit = renderer->getCameraController()->isIdleOrbit();
    if (idleOrbit && !idleYawned_ && renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->playEmote("yawn");
        idleYawned_ = true;
    } else if (!idleOrbit) {
        idleYawned_ = false;
    }
    }
    if (renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->setTaxiFlight(onTaxi);
        if (gameHandler && gameHandler->takeBarberLeft() && uiManager)
            uiManager->getGameScreen().windowManager().barberResetSelections(*gameHandler);
        // The barber's cape on as its window opens, and off again (0x0052f9b0,
        // 0x0052e9f0).
        if (appearanceComposer_ && gameHandler &&
            appearanceComposer_->setBarberDress(gameHandler->isBarberShopOpen())) {
            gameHandler->markOnlineEquipmentDirty();
        }
        // Round to the character's face while the barber has them in the chair,
        // and back to the player's own view when either ends.
        //
        // Reconciled here rather than on the packet that seats them, because
        // the seat and the shop arrive in either order - they are two packets
        // sent together - and a view set from whichever came first would have
        // been set on half the story.
        if (auto* cam = renderer->getCameraController()) {
            cam->setBarberShopView(gameHandler && gameHandler->isBarberShopOpen() &&
                                   cam->isSeatedInChair());
            if (gameHandler && gameHandler->isInWorld()) {
                const auto idle = game::idleSteps({.idleMs = Input::getInstance().idleMs(),
                                                   .standing = gameHandler->getStandState() == 0,
                                                   .fighting = gameHandler->playerFights(),
                                                   .mounted = gameHandler->isMounted(),
                                                   .walking = gameHandler->clickToMoveUnderWay(),
                                                   .onTaxi = onTaxi,
                                                   .away = gameHandler->isAfk(),
                                                   .loggingOut = gameHandler->isLoggingOut()});
                if (idle.sit) cam->sitDown();
                if (idle.away) gameHandler->toggleAfk();
                if (idle.logout) gameHandler->logoutIdle();
            }
        }
    }
    if (renderer && renderer->getTerrainManager()) {
    renderer->getTerrainManager()->setStreamingEnabled(true);
    renderer->getTerrainManager()->setUpdateInterval(0.033f);
    // The same square about the camera in flight as on foot (0x00780860 asks
    // the far clip and nothing else). It was eight tiles each way and twelve
    // to keep on a flight path, and the route besides: 450 tiles in by
    // Orgrimmar, 100000 terrain meshes for the rays, and 16 GB in all.
    renderer->getTerrainManager()->setLoadReach(renderer->getTerrainLoadReach());
    renderer->getTerrainManager()->setUnloadReach(renderer->getTerrainUnloadReach());
    }
    if (worldEntryCallbacks_) worldEntryCallbacks_->setLastTaxiFlight(actuallyFlying);

    // Sync character render position ↔ canonical WoW coords each frame. Not
    // between the server's word of a new world and the end of its load: there
    // is no player in a world until then (0x00403b70 takes the object away
    // with every other), and the place drawn is the old world's. Told of, it
    // put the character there on the new map - whatever lay at those
    // coordinates - for as long as the load took.
    const bool enteringWorld = worldLoader_ && worldLoader_->isWorldEntryUnderway();
    if (enteringWorld) lastSentCanonicalYaw_ = kFacingFromServer;
    // Whether what the camera looks at is on a flight path (0x00606f90 asks
    // the unit it looks at; with none it is not run).
    if (renderer && renderer->getCameraController() && gameHandler && !enteringWorld) {
        if (const auto unit = std::dynamic_pointer_cast<game::Unit>(
                gameHandler->getEntityManager().getEntity(gameHandler->cameraTarget())))
            renderer->getCameraController()->setOnFlight((unit->getUnitFlags() & game::UNIT_FLAG_TAXI_FLIGHT) != 0);
    }
    if (renderer && gameHandler && !enteringWorld) {
    // For position sync branching, only WMO transports use the dedicated
    // onTransport branch. M2 transports use the normal movement else branch
    // with a position-delta correction applied on top.
    bool onTransport = onWMOTransport;

    static bool wasOnTransport = false;
    bool onTransportNowDbg = gameHandler->isOnTransport();
    if (onTransportNowDbg != wasOnTransport) {
        // At warning, not debug. Whether the player is attached to a transport
        // decides which movement branch runs, and "the zeppelin leaves without
        // me" has no other symptom - the transport moves, the player does not,
        // and nothing anywhere says whether the attachment was ever made.
        LOG_WARNING("Transport state changed: onTransport=", onTransportNowDbg,
                 " isM2=", isM2Transport,
                 " guid=0x", std::hex, gameHandler->getPlayerTransportGuid(), std::dec);
        wasOnTransport = onTransportNowDbg;
    }

    if (onTaxi) {
        auto playerEntity = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
        glm::vec3 canonical(0.0f);
        bool haveCanonical = false;
        if (playerEntity) {
            canonical = glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ());
            haveCanonical = true;
        } else {
            // Fallback for brief entity gaps during taxi start/updates:
            // movementInfo is still updated by client taxi simulation.
            const auto& move = gameHandler->getMovementInfo();
            canonical = glm::vec3(move.x, move.y, move.z);
            haveCanonical = true;
        }
        if (haveCanonical) {
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            renderer->getCharacterPosition() = renderPos;
            if (renderer->getCameraController()) {
                glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable();
                if (followTarget) {
                    *followTarget = renderPos;
                }
            }
        }
    } else if (onTransport) {
        // WMO transport mode (ships): keep the transport-local
        // attachment, while folding real WASD/jump movement since
        // last frame into that offset. Treating ships as fully
        // externally driven cleared movement input and reapplied the
        // boarding-time offset forever, freezing the player in a
        // running pose at the point where they stepped aboard.
        auto* tm = gameHandler->getTransportManager();
        auto* tr = tm ? tm->getTransport(gameHandler->getPlayerTransportGuid()) : nullptr;
        if (tr) {
            const uint64_t transportGuid = gameHandler->getPlayerTransportGuid();
            const uint32_t mapId = gameHandler->getCurrentMapId();
            glm::vec3 localOffset = gameHandler->getPlayerTransportOffset();
            const glm::vec3 tentativeRender = renderer->getCharacterPosition();
            const glm::vec3 expectedRender(
                tr->transform * glm::vec4(localOffset, 1.0f));
            glm::vec3 intendedRender = expectedRender;

            // A cross-map ship transfer can reuse the same synthetic GO
            // GUID on the destination map. Never interpret the continent-
            // sized difference from the previous map's ride lock as deck
            // walking: doing so rewrites a valid server transport offset
            // into a huge negative Z and drops the rider underwater.
            const bool sameRideFrame = hasWMORideLock_ &&
                lastWMORideTransportGuid_ == transportGuid &&
                lastWMORideMapId_ == mapId;
            if (!sameRideFrame && !tr->isM2) {
                deckFloorPending_ = true;
            }
            // Carried to another map on it: the hull flew on through the
            // load, and the ground the camera remembers is where the transfer
            // set the rider down, at the edge of the map. Its floor probe
            // reached up from there and stood the rider on the top of the
            // zeppelin's envelope, inside it. The rider is where the offset on
            // the deck puts them now (0x004f4460), and the ground with them.
            if (!tr->isM2 && gameHandler->takeTransportTransferArrival()) {
                if (auto* cc = renderer->getCameraController()) cc->teleportTo(expectedRender, true);
            }
            // The rider's facing is kept on the deck, not in the world: the
            // world one is the ship's plus it (0x004f42a0), so as the ship
            // turns, so does whoever is standing on it, and the camera behind.
            if (sameRideFrame) {
                const float turnDeg = glm::degrees(
                    core::coords::normalizeAngleRad(tr->facing - lastWMORideFacing_));
                auto* cc = renderer->getCameraController();
                if (cc && std::abs(turnDeg) > 1e-4f) {
                    cc->turnWithVehicle(turnDeg);
                    cc->setFacingYaw(cc->getFacingYaw() + turnDeg);
                    renderer->setCharacterYaw(cc->getFacingYaw());
                    renderer->turnCharacterBody(turnDeg);
                }
                static const bool kRideTurnDebug = [] {
                    const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
                    return v && v[0] && v[0] != '0';
                }();
                static float rideTurnLogTimer = 0.0f;
                rideTurnLogTimer += deltaTime;
                if (kRideTurnDebug && cc && rideTurnLogTimer >= 1.0f) {
                    rideTurnLogTimer = 0.0f;
                    const auto* ac = renderer->getAnimationController();
                    const int shuffle = ac ? static_cast<int>(ac->turnShuffle()) : -1;
                    LOG_WARNING("RIDE TURN: facing=", tr->facing, " turnDeg=", turnDeg, " camYaw=", cc->getYaw(),
                                " facingYaw=", cc->getFacingYaw(), " body=", renderer->getCharacterBodyYaw(),
                                " shuffle=", shuffle, " clientAnim=", tr->useClientAnimation,
                                " serverYaw=", tr->hasServerYaw);
                }
            }
            lastWMORideFacing_ = tr->facing;
            if (sameRideFrame && renderer->getCameraController()) {
                const glm::vec3 localMotion =
                    tentativeRender - lastWMORideLockedRender_;
                if (renderer->getCameraController()->isMoving()) {
                    intendedRender.x += localMotion.x;
                    intendedRender.y += localMotion.y;
                }
                if (!renderer->getCameraController()->isGrounded()) {
                    intendedRender.z += localMotion.z;
                }
            }

            // The camera controller's ordinary static-world floor query accepts a
            // moving WMO deck only when it is right under the feet, so every WMO
            // ship needs its exact transport-instance floor held under the rider -
            // otherwise gravity folds into the attachment and pulls them through
            // the hull, and multi-deck ships (stairs, ramps) can't be climbed
            // because nothing raises the rider onto the upper geometry. This is
            // the walkable-deck query for ANY ship, keyed on the transport being
            // a WMO (not M2), not on a specific ship entry. getInstanceFloorHeight
            // returns the height of whatever deck/stair is under the player, so
            // walking up onto a higher deck just follows the collision. An upward
            // jump remains fully controlled by vertical physics (skipped below).
            auto* cameraController = renderer->getCameraController();
            // Standing still on it: the deck alone places the rider.
            if (cameraController) {
                cameraController->setRideHeld(!tr->isM2 && !cameraController->isMoving() &&
                                              cameraController->isGrounded());
            }
            if (!tr->isM2 && cameraController &&
                !cameraController->isJumping()) {
                const glm::vec3 intendedCanonical =
                    core::coords::renderToCanonical(intendedRender);
                const auto deckFloor = tm->getTransportDeckFloorHeight(
                    transportGuid, intendedCanonical);
                if (deckFloor && intendedRender.z >= *deckFloor - 3.0f &&
                    intendedRender.z <= *deckFloor + 0.35f) {
                    intendedRender.z = *deckFloor + 0.10f;
                    cameraController->suppressVerticalPhysics();
                    deckFloorPending_ = false;
                } else if (deckFloorPending_ &&
                           !tm->isTransportCollisionReady(transportGuid)) {
                    // A continent transfer registers the transport GO
                    // before its WMO collision necessarily finishes loading.
                    // Preserve the local offset until this exact instance's
                    // deck exists instead of releasing gravity after a timer.
                    intendedRender = expectedRender;
                    cameraController->suppressVerticalPhysics();
                } else if (deckFloorPending_) {
                    // Collision is loaded and still found no deck underfoot,
                    // which is a real answer, not a not-ready one. The hold
                    // above waits for geometry to arrive; it must not
                    // outlive its own premise.
                    //
                    // It did, and there was no way out of it: the flag is set
                    // on boarding and cleared only by a successful deck query,
                    // so boarding somewhere the query never succeeds - a
                    // gangway that belongs to the pier rather than the hull -
                    // reapplied the boarding offset every frame forever. That
                    // is a rider running on the spot, unable to walk far
                    // enough to trigger disembark and so unable to get off.
                    deckFloorPending_ = false;
                }
            }

            localOffset = glm::vec3(
                tr->invTransform * glm::vec4(intendedRender, 1.0f));
            gameHandler->setPlayerTransportOffset(localOffset);
        }

        glm::vec3 canonical = gameHandler->getComposedWorldPosition();
        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
        renderer->getCharacterPosition() = renderPos;
        lastWMORideLockedRender_ = renderPos;
        hasWMORideLock_ = true;
        lastWMORideTransportGuid_ = gameHandler->getPlayerTransportGuid();
        lastWMORideMapId_ = gameHandler->getCurrentMapId();
        gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
        // The facing in the world, which turns with the ship (the turn is
        // added to the character's yaw above): the client's unit facing is
        // the transport's plus the one kept on it (vtable +0x34, 0x006e6f40
        // -> 0x004f42a0), and GetPlayerFacing (0x0060a490) reads that. Left
        // as it was at boarding, it stood still while the hull turned, and
        // the facing on the deck worked out from it for the wire
        // (transportFacingToWire) drifted the other way.
        gameHandler->setOrientation(core::coords::characterYawDegToCanonical(renderer->getCharacterYaw()));
        if (renderer->getCameraController()) {
            glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable();
            if (followTarget) {
                *followTarget = renderPos;
            }
        }
        {
            // WOWEE_TRANSPORT_POSE: every tenth of a second on a transport,
            // the hull, the rider (world and on the deck) and the camera.
            static const bool kRideTrace = [] {
                const char* v = std::getenv("WOWEE_TRANSPORT_POSE");
                return v && v[0] && v[0] != '0';
            }();
            static auto lastRideLog = std::chrono::steady_clock::time_point{};
            const auto nowRide = std::chrono::steady_clock::now();
            if (kRideTrace && nowRide - lastRideLog >= std::chrono::milliseconds(100)) {
                lastRideLog = nowRide;
                const auto* trLog = gameHandler->getTransportManager()
                    ? gameHandler->getTransportManager()->getTransport(gameHandler->getPlayerTransportGuid()) : nullptr;
                const auto* camLog = renderer->getCamera();
                const glm::vec3 off = gameHandler->getPlayerTransportOffset();
                const glm::vec3 cp = camLog ? camLog->getPosition() : glm::vec3(0.0f);
                const glm::vec3 cf = camLog ? camLog->getForward() : glm::vec3(0.0f);
                LOG_WARNING("RIDE POSE: hull=(", trLog ? trLog->position.x : 0.0f, ", ", trLog ? trLog->position.y : 0.0f,
                            ", ", trLog ? trLog->position.z : 0.0f, ") yaw=", trLog ? trLog->facing : 0.0f,
                            " player=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ") off=(", off.x, ", ",
                            off.y, ", ", off.z, ") cam=(", cp.x, ", ", cp.y, ", ", cp.z, ") fwd=(", cf.x, ", ", cf.y,
                            ", ", cf.z, ")");
            }
        }
        gameHandler->updateM2TransportBoarding(canonical);
    } else if (splineCarried) {
        // Where the spline has the player's own entity, or that of the unit
        // moved in its stead (0x0073c8e0), to its end once it has run out, and
        // facing as it does.
        if (auto carried = gameHandler->getEntityManager().getEntity(characterUnit)) {
            const glm::vec3 canonical = splineUnit != 0
                ? glm::vec3(carried->getX(), carried->getY(), carried->getZ())
                : glm::vec3(carried->getLatestX(), carried->getLatestY(), carried->getLatestZ());
            const glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            renderer->getCharacterPosition() = renderPos;
            const float yawDeg = core::coords::canonicalToCharacterYawDeg(carried->getOrientation());
            renderer->setCharacterYaw(yawDeg);
            renderer->getCameraController()->setFacingYaw(yawDeg);
            // The movement the client keeps goes with it; the entity is not
            // set, which would end its spline.
            auto& move = gameHandler->movementInfoRef();
            move.x = canonical.x;
            move.y = canonical.y;
            move.z = canonical.z;
            gameHandler->setOrientation(carried->getOrientation());
            if (glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable())
                *followTarget = renderPos;
        }
    } else {
        if (auto* cc = renderer ? renderer->getCameraController() : nullptr) cc->setRideHeld(false);
        hasWMORideLock_ = false;
        lastWMORideTransportGuid_ = 0;
        lastWMORideMapId_ = 0xFFFFFFFFu;
        deckFloorPending_ = false;
        // In a vehicle's seat the player is its place there (0x004f4460), and
        // stands there when it is let go: what the camera's character and the
        // keys had was the place it got on at.
        if (gameHandler->activeMover() == 0 || gameHandler->activeMover() == gameHandler->getPlayerGuid()) {
            if (auto* movement = gameHandler->getMovementHandler()) {
                if (const auto seat = movement->seatPlace(gameHandler->getPlayerGuid()))
                    renderer->getCharacterPosition() = core::coords::canonicalToRender(*seat);
            }
        }
        glm::vec3 renderPos = renderer->getCharacterPosition();

        // M2 transport riding: resolve in canonical space and lock once per frame.
        // This avoids visible jitter from mixed render/canonical delta application.
        if (isM2Transport && gameHandler->getTransportManager()) {
            auto* tr = gameHandler->getTransportManager()->getTransport(
                gameHandler->getPlayerTransportGuid());
            if (tr) {
                // Ride along at a fixed offset from the transport's current position
                // (set once at boarding - see GameHandler::updateM2TransportBoarding),
                // plus whatever the player has walked on the deck since then. WASD
                // input runs earlier in the frame and moves renderer's character
                // position directly; comparing that (tentativeCanonical) against
                // where *we* locked it last frame isolates just that walked delta,
                // so standing still holds a fixed deck position while active input
                // still moves the player, instead of either (a) fully locking
                // movement or (b) recomputing offset from the absolute position,
                // which is a no-op identity once fed back into
                // lockedCanonical = transport matrix * offset: the character's render
                // position could never actually change due to the tram moving, so
                // riding appeared to "float" in place no matter how far the tram
                // traveled underneath.
                auto* transports = gameHandler->getTransportManager();
                const uint64_t rideGuid = gameHandler->getPlayerTransportGuid();
                // The offset is in the car's own frame; the walking below is
                // in the world's, so it is done to the place the offset puts
                // the rider and taken back into the car's frame after.
                const glm::vec3 heldOffset = gameHandler->getPlayerTransportOffset();
                // Where the offset alone puts the rider: if nothing below moves
                // them from there, the offset is kept as it is.
                const glm::vec3 carriedCanonical = transports->getPlayerWorldPosition(rideGuid, heldOffset);
                // A rider moves in the car's frame, as the client moves a
                // passenger (its place on the transport is what it steps and
                // falls by): the car carries it, and only its own movement
                // this frame - the camera's, from where it was put last frame -
                // is added. Taking the camera's world height instead read the
                // car's own travel as a fall or a climb: a lift running down at
                // 27 yd/s left its rider floating above the floor, one running
                // up left it below, where the floor underfoot is the world's
                // and the ride ended.
                //
                // The camera's own locomotion this frame counts as well as
                // the keys: for two seconds after a teleport the keys are
                // let go every frame (the world-entry grace) while the
                // camera still walks the character on them, and a walk on
                // a lift's car then went nowhere - the car held its rider
                // at the middle while the character ran on the spot.
                auto* rider = renderer->getCameraController();
                game::car_ride::Step step;
                step.carried = carriedCanonical;
                step.boarding = !hasM2RideLock_;
                if (rider) {
                    step.grounded = rider->isGrounded();
                    step.rising = rider->isJumping();
                    if (hasM2RideLock_) {
                        step.own = core::coords::renderToCanonical(renderPos) - lastM2RideLockedCanonical_;
                        step.moves = rider->isMoving() || rider->getLocomotionSpeed() > 0.0f || !step.grounded;
                    }
                }
                const game::car_ride::Place place = game::car_ride::place(
                    step, [&](const glm::vec3& at) { return transports->m2DeckFloorHeight(rideGuid, at); });
                const glm::vec3 riderCanonical = place.at;
                if (rider && place.onFloor) rider->suppressVerticalPhysics();
                // The offset is the passenger's place on the transport and the
                // client changes it only by moving the passenger (0x004f47b0
                // is boarding's). Taken back out of the world each frame
                // instead, it drifted: through the inverse of a car pitched on
                // the tram's slopes, two thousand yards from the origin, the
                // round trip loses a little the same way every frame - a rider
                // standing still slid half a yard along the car in a trip.
                glm::vec3 localOffset = riderCanonical == carriedCanonical
                    ? heldOffset
                    : transports->worldToTransportLocal(rideGuid, riderCanonical).value_or(glm::vec3(0.0f));
                gameHandler->setPlayerTransportOffset(localOffset);

                glm::vec3 lockedCanonical = transports->getPlayerWorldPosition(rideGuid, localOffset);
                renderPos = core::coords::canonicalToRender(lockedCanonical);
                renderer->getCharacterPosition() = renderPos;
                lastM2RideLockedCanonical_ = lockedCanonical;
                hasM2RideLock_ = true;
            }
        } else {
            hasM2RideLock_ = false;
        }

        // Model-Z reversal probe: the floor-selection log stays
        // quiet through the reported Undercity bob, so the bob is in
        // the Z that actually reaches the character, not the floor
        // chosen for it - gravity overshooting a stable floor, or a
        // server correction blended in. This watches the committed
        // render Z for a direction flip (up-then-down or the
        // reverse) where both legs clear 0.15, the unmistakable
        // signature of a yo-yo, and names the two steps so the next
        // Undercity walk says how far and how fast it bobs and
        // whether it moves while the feet are otherwise still.
        {
            static float lastZ = renderPos.z;
            static float lastDz = 0.0f;
            static std::chrono::steady_clock::time_point lastRevLog{};
            const float dz = renderPos.z - lastZ;
            if (std::abs(dz) > 0.15f && std::abs(lastDz) > 0.15f &&
                ((dz > 0.0f) != (lastDz > 0.0f))) {
                static core::LogBudget reversalBudget(12, "Player Z reversal");
                const auto now = std::chrono::steady_clock::now();
                if (now - lastRevLog > std::chrono::milliseconds(250) &&
                    reversalBudget.take()) {
                    lastRevLog = now;
                    LOG_WARNING("Player Z reversal: z=", renderPos.z,
                                " step=", dz, " prevStep=", lastDz,
                                " grounded=",
                                renderer->getCameraController() &&
                                renderer->getCameraController()->isGrounded() ? 1 : 0,
                                " moving=",
                                renderer->getCameraController() &&
                                renderer->getCameraController()->isMoving() ? 1 : 0);
                }
            }
            if (std::abs(dz) > 0.02f) lastDz = dz;
            lastZ = renderPos.z;
        }

        glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
        gameHandler->setPosition(canonical.x, canonical.y, canonical.z);

        // Sync orientation: camera yaw (degrees) → WoW orientation (radians)
        float yawDeg = renderer->getCharacterYaw();
        // Keep all game-side orientation in canonical space.
        // We historically sent serverYaw = radians(yawDeg - 90). With the new
        // canonical<->server mapping (serverYaw = PI/2 - canonicalYaw), the
        // equivalent canonical yaw is radians(180 - yawDeg).
        float canonicalYaw = core::coords::characterYawDegToCanonical(yawDeg);
        gameHandler->setOrientation(canonicalYaw);

        // Send MSG_MOVE_SET_FACING when the player changes facing direction
        // (e.g. via mouse-look). Without this, the server predicts movement in
        // the old facing and position-corrects on the next heartbeat - the
        // micro-teleporting the GM observed.
        // Skip while keyboard-turning: the server tracks that via TURN_LEFT/RIGHT flags.
        facingSendCooldown_ -= deltaTime;
        const auto& mi = gameHandler->getMovementInfo();
        constexpr uint32_t kTurnFlags =
            static_cast<uint32_t>(game::MovementFlags::TURN_LEFT) |
            static_cast<uint32_t>(game::MovementFlags::TURN_RIGHT);
        bool keyboardTurning = (mi.flags & kTurnFlags) != 0;
        // The facing a world is entered with is the server's own.
        if (lastSentCanonicalYaw_ == kFacingFromServer) lastSentCanonicalYaw_ = canonicalYaw;
        if (!keyboardTurning && facingSendCooldown_ <= 0.0f) {
            float yawDiff = core::coords::normalizeAngleRad(canonicalYaw - lastSentCanonicalYaw_);
            if (std::abs(yawDiff) > glm::radians(3.0f)) {
                gameHandler->sendMovement(game::Opcode::MSG_MOVE_SET_FACING);
                lastSentCanonicalYaw_ = canonicalYaw;
                facingSendCooldown_ = 0.1f;  // max 10 Hz
            }
        }

        // Client-side transport board/disembark check - shared
        // with any other driver that knows the player's canonical position (see
        // GameHandler::updateM2TransportBoarding).
        if (gameHandler->getTransportManager()) {
            glm::vec3 playerCanonical = core::coords::renderToCanonical(renderPos);
            gameHandler->updateM2TransportBoarding(playerCanonical);
        }
    }
    }
}

// Keep the render instances on top of what the server says.
//
// A creature's model is placed once when it spawns and would stay there, while
// the target circle follows the entity - a drift between the two reads as a
// ring sliding off an NPC that never moved. Player instances need the same for
// a different reason: without it they never leave the run animation when they
// stop.
rendering::upper_body::Unit Application::playerAnimationState(const game::Unit& self) const {
    namespace ub = rendering::upper_body;
    ub::Unit u;
    // Its own movement flags, the ones it sends (+0x7cc). In the air as
    // 0x00723350 has it: falling with a jump's vertical speed, or falling
    // far - which a drop off a ledge is here once it has lasted as long as a
    // fall the landing is told of. Its own while a unit it possesses moves
    // in its stead: the keys move that one, not the player.
    const auto& movement =
        gameHandler->moverPossessed() ? gameHandler->playerMovementInfo() : gameHandler->getMovementInfo();
    u.moveFlags = movement.flags;
    const bool launched = (u.moveFlags & ub::kMoveFalling) != 0 && movement.jumpVelocity != 0.0f;
    if (playerFallsFar()) u.moveFlags |= ub::kMoveFalling | kMoveFallingFar;
    u.leaping = launched || (u.moveFlags & kMoveFallingFar) != 0;
    u.standState = gameHandler->getStandState();
    u.mounted = self.getMountDisplayId() != 0;
    u.alive = self.getHealth() > 0;
    u.channeling = gameHandler->isChanneling();
    u.castTimed = gameHandler->isCasting() && !u.channeling && gameHandler->getCastTimeTotal() > 0.0f;
    u.autoAttacking = gameHandler->isAutoAttacking();
    u.rangedDrawn = appearanceComposer_ && appearanceComposer_->sheathState() == SheathState::Ranged;
    u.wholeBodyOnly = entitySpawner_ &&
        (entitySpawner_->creatureModelFlags(self.getDisplayId()).value_or(0) & 0x8u) != 0;
    if (auto* ac = renderer ? renderer->getAnimationController() : nullptr) {
        // Turning on the spot shuffles it round (+0xa38 0x1800, 0x800 to
        // the left).
        u.turningInPlace = ac->turnShuffle() != rendering::body_yaw::TurnShuffle::None;
        u.turningLeft = ac->turnShuffle() == rendering::body_yaw::TurnShuffle::Left;
        u.riding = ac->isMounted();
    }
    // What its base animation reads besides (0x00724500): its health,
    // feigning death, stealth, its walk speed, hovering or gravity off, and
    // the ready for the weapons in its hands (0x00714dd0).
    u.alive = self.getHealth() > 0;
    if (const uint16_t f2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2); f2 != 0xFFFF)
        u.feignDeath = (self.getField(f2) & 0x1u) != 0;
    if (const uint16_t b1 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_1); b1 != 0xFFFF)
        u.stealthed = ((self.getField(b1) >> 16) & 0x2u) != 0;
    u.walkSpeed = gameHandler->getServerWalkSpeed();
    unitSplineState(u, *gameHandler, gameHandler->getPlayerGuid(), u.moveFlags);
    // On a flight path: along a flying spline (0x004f5290), forward as one
    // moves a unit.
    if (auto* ac = renderer ? renderer->getAnimationController() : nullptr; ac && ac->isMounted() && ac->isTaxiFlight()) {
        u.splineFlying = true;
        u.leaping = false;
        u.moveFlags = (u.moveFlags & ~(ub::kMoveSwimOrFly | ub::kMoveFalling | kMoveFallingFar)) | 0x1u;
    }
    // Looting (0x007222a0, 0x0071b6b0 read the loot target for the active
    // player), in combat (0x0071afb0), an auto-repeating shot (+0xa38 0x200).
    u.looting = u.canLoot = gameHandler->isLootWindowOpen();
    if (const uint16_t f = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS); f != 0xFFFF)
        u.inCombat = (self.getField(f) & 0x800u) != 0;
    u.inCombat = u.inCombat || u.autoAttacking;
    u.autoRepeat = gameHandler->isAutoRepeating();
    // Its emote state (0x007171c0).
    if (const uint32_t emote = self.getNpcEmoteState(); emote != 0) {
        if (const uint32_t anim = rendering::AnimationController::getEmoteAnimByEmotesId(emote); anim != 0)
            u.emoteStateAnim = anim;
    }
    {
        namespace ba = rendering::base_animation;
        const auto& inventory = gameHandler->getInventory();
        const auto hand = [this, &inventory](game::EquipSlot slot) -> std::optional<ba::HandItem> {
            const auto& equipped = inventory.getEquipSlot(slot);
            if (equipped.empty()) return std::nullopt;
            const auto* info = gameHandler->getItemInfo(equipped.item.itemId);
            if (!info || !info->valid) return ba::HandItem{};
            return ba::HandItem{info->itemClass, info->subClass};
        };
        const auto mainHand = hand(game::EquipSlot::MAIN_HAND);
        const auto offHand = hand(game::EquipSlot::OFF_HAND);
        u.meleeReady = ba::meleeReady(mainHand ? &*mainHand : nullptr, offHand ? &*offHand : nullptr);
        // The ranged weapon's subclass, for its ready (0x00714f90).
        if (const auto ranged = hand(game::EquipSlot::RANGED); ranged && ranged->itemClass == 2)
            u.rangedSubClass = ranged->subClass;
    }
    unitKitAnimationState(u, renderer ? renderer->getSpellVisualSystem() : nullptr, gameHandler->getPlayerGuid(),
                          gameHandler->isCasting() && !u.channeling ? gameHandler->getCurrentCastSpellId() : 0u,
                          unitChannelSpell(self));
    // The pose it rides in, as the animation controller puts it in it.
    if (u.riding && renderer && renderer->getCharacterRenderer()) {
        if (auto* svs = renderer->getSpellVisualSystem()) {
            const uint32_t pose = svs->activePlayerRiderPose();
            if (renderer->getCharacterRenderer()->hasAnimation(renderer->getCharacterInstanceId(), pose))
                u.riderPose = pose;
        }
    }
    return u;
}

// 0x006dd9e0, a text emote the player sends: a stand-state emote asks for
// its stand state (0x006dcb40); any other animation plays at once only where
// 0x00724410 lets it - on the move - and otherwise waits for the server's
// SMSG_EMOTE (0x006cfe70).
void Application::playTextEmoteLocally(uint32_t textEmoteId) {
    if (!appearanceComposer_ || !gameHandler) return;
    const TextEmoteSheathInput row = appearanceComposer_->textEmoteRow(textEmoteId);
    if (!row.emoteKnown) return;
    if (textEmoteSetsStandState(row.specProc, row.specProcParam)) {
        gameHandler->setStandState(static_cast<uint8_t>(row.specProcParam));
        return;
    }
    if (row.animId == 0) return;
    auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
    const uint32_t instanceId = renderer ? renderer->getCharacterInstanceId() : 0;
    auto entity = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
    const auto* self = dynamic_cast<const game::Unit*>(entity.get());
    if (!cr || instanceId == 0 || !self) return;
    rendering::upper_body::Unit u = playerAnimationState(*self);
    if (const auto seen = cr->unitStandStateSeen(instanceId)) u.standStateSeen = *seen;
    // 0x006dac10: a ghost (PLAYER_FLAGS 0x10) is dead to it.
    bool ghost = false;
    if (const uint16_t pf = game::fieldIndex(game::UF::PLAYER_FLAGS); pf != 0xFFFF)
        ghost = (self->getField(pf) & 0x10u) != 0;
    if (!rendering::upper_body::localEmotePlays(u, ghost)) return;
    cr->playAnimation(instanceId, row.animId, row.specProc == 2);
}

// The player's collision height (+0x854): what the server gave last, else
// its model's, or its mount's under it (0x0073fcc0 -> 0x0073d5d0(1, 1) ->
// 0x006d7720 for one made mounted).
float Application::playerCollisionHeight() {
    if (const auto* mh = gameHandler->getMovementHandler(); mh && mh->collisionHeightGiven())
        return *mh->collisionHeightGiven();
    if (auto self = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid()); self && self->isUnit()) {
        const uint32_t mount = static_cast<const game::Unit&>(*self).getMountDisplayId();
        if (mount != 0) {
            if (const float h = entitySpawner_->playerMountedCollisionHeight(mount); h > 0.0f) return h;
        }
    }
    return entitySpawner_->playerCollisionHeight();
}

// MOVEMENTFLAG_FALLING_FAR as the fall step sets it (CameraController::
// isFallingFar), which the fall's animation and its landing go by.
bool Application::playerFallsFar() const {
    const auto* cc = renderer ? renderer->getCameraController() : nullptr;
    return cc && cc->isFallingFar() && !hasM2RideLock_;
}

// The player's unit, onto its instance each frame, and its base animation
// asked again where what it reads has changed: a turn on the spot started or
// stopped (0x0073dab0), alive or dead, looting, stealth, a melee target taken
// or dropped, into or out of water or the air. Its movement events ask it as
// they come (animatePlayerMovement), and falling far Fall (0x0073ad00).
void Application::syncPlayerDisplayModel(const game::Unit& self) {
    if (!playerCharacterSpawned || !renderer || !entitySpawner_ || !gameHandler) return;
    auto* cr = renderer->getCharacterRenderer();
    if (!cr) return;
    const auto field = [&](game::UF f) {
        const uint16_t i = game::fieldIndex(f);
        return i == 0xFFFF ? 0u : self.getField(i);
    };
    const uint32_t display = field(game::UF::UNIT_FIELD_DISPLAYID);
    const uint32_t native = field(game::UF::UNIT_FIELD_NATIVEDISPLAYID);
    const uint32_t want = display != 0 && native != 0 && display != native ? display : 0;
    if (want == shownPlayerDisplay_) return;
    const glm::vec3 pos = renderer->getCharacterPosition();
    // The character, or one of another race's body that wears the player's own
    // things (0x0071a430): built again from the look, with its gear.
    if (want == 0 || gameHandler->ownEquipmentDisplay(want)) {
        if (const uint32_t old = renderer->getCharacterInstanceId(); old != 0) {
            renderer->setCharacterFollow(0);
            cr->removeInstance(old);
        }
        renderer->setCharacterShownAsDisplay(false);
        playerCharacterSpawned = false;
        spawnedPlayerGuid_ = 0;
        spawnSnapToGround = false;
        spawnPlayerCharacter();
        renderer->getCharacterPosition() = pos;
        gameHandler->resetEquipmentDirtyTracking();
        return;
    }
    float scale = 1.0f;
    if (const uint32_t raw = field(game::UF::OBJECT_FIELD_SCALE_X); raw != 0) {
        float f = 1.0f;
        std::memcpy(&f, &raw, sizeof(f));
        if (f > 0.01f && f < 100.0f) scale = f;
    }
    const uint32_t instanceId = entitySpawner_->createDisplayInstance(want, pos, 0.0f, scale);
    shownPlayerDisplay_ = want;
    if (instanceId == 0) {
        LOG_WARNING("Player display ", want, " has no model to draw; the character stays");
        return;
    }
    if (const uint32_t old = renderer->getCharacterInstanceId(); old != 0) {
        renderer->setCharacterFollow(0);
        if (auto* ac = renderer->getAnimationController()) ac->clearMount();
        cr->removeInstance(old);
    }
    renderer->setCharacterShownAsDisplay(true);
    renderer->getCharacterPosition() = pos;
    renderer->setCharacterFollow(instanceId);
    if (renderer->getCameraController()) renderer->getCameraController()->setCharacterRenderer(cr, instanceId);
    cr->playAnimation(instanceId, rendering::anim::STAND, true);
    playerAnimationSeen_ = {};
    LOG_INFO("Player shown as display ", want, " (native ", native, ")");
}

void Application::syncPlayerAnimation(const game::Unit& self) {
    auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
    const uint32_t instanceId = renderer ? renderer->getCharacterInstanceId() : 0;
    if (!cr || instanceId == 0) return;
    cr->setAnimationTier(instanceId, unitAnimationTier(self));
    const rendering::upper_body::Unit u = playerAnimationState(self);
    // The mount under it, which plays what the player is asked (0x007385c0).
    cr->setRiderMount(instanceId, u.riding && entitySpawner_ ? entitySpawner_->getMountInstanceId() : 0u);
    cr->setUnitAnimationState(instanceId, u);
    const rendering::PlayerAnimationSeen now{.instanceId = instanceId,
                                  .fellFar = (u.moveFlags & kMoveFallingFar) != 0,
                                  .turning = u.turningInPlace,
                                  .turningLeft = u.turningLeft,
                                  .alive = u.alive,
                                  .looting = u.looting,
                                  .stealthed = u.stealthed,
                                  .autoAttacking = u.autoAttacking,
                                  .swimming = (u.moveFlags & rendering::upper_body::kMoveSwimOrFly) != 0,
                                  .onFlightPath = u.splineFlying,
                                  .splineMoves = playerSplineMoves_};
    const rendering::PlayerAnimationSeen before = std::exchange(playerAnimationSeen_, now);
    if (rendering::asksBaseAgain(before, now)) cr->playBaseAnimation(instanceId);
    if (before.instanceId == instanceId && now.fellFar && !before.fellFar && now.alive) cr->playAnimation(instanceId, rendering::anim::FALL, true);
}

// What an event of the player's own movement does to its animation: the
// client's switch for it (0x0073ed10, mover_event), and for a landing
// 0x0073d3d0 -> 0x0073d2b0.
void Application::animatePlayerMovement(game::Opcode op, uint32_t flagsBefore, bool launchedBefore) {
    namespace me = rendering::mover_event;
    auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr;
    const uint32_t instanceId = renderer ? renderer->getCharacterInstanceId() : 0;
    if (!cr || instanceId == 0 || !gameHandler) return;
    // The events of a unit the player possesses are that unit's, not the
    // player's own (the mover is the unit the event is for).
    if (gameHandler->moverPossessed()) return;
    auto self = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
    if (!self || !self->isUnit()) return;
    // Carried along a flight path it moves by no event of its own.
    auto* ac = renderer->getAnimationController();
    const bool mounted = ac && ac->isMounted();
    if (mounted && ac->isTaxiFlight()) return;
    // The state the event leaves it in, and the speed its flags move it at
    // (CMovement::GetCurrentSpeed).
    const uint32_t flags = (gameHandler->moverPossessed() ? gameHandler->playerMovementInfo()
                                                          : gameHandler->getMovementInfo()).flags;
    game::MovementSpeeds speeds;
    speeds.walk = gameHandler->getServerWalkSpeed();
    speeds.run = gameHandler->getServerRunSpeed();
    speeds.runBack = gameHandler->getServerRunBackSpeed();
    speeds.swim = gameHandler->getServerSwimSpeed();
    speeds.swimBack = gameHandler->getServerSwimBackSpeed();
    speeds.flight = gameHandler->getServerFlightSpeed();
    speeds.flightBack = gameHandler->getServerFlightBackSpeed();
    const float speed = game::Entity::speedForFlags(speeds, flags);
    // Mounted, it is the mount's stride that goes at it.
    const uint32_t mountInstance = mounted && entitySpawner_ ? entitySpawner_->getMountInstanceId() : 0u;
    cr->setLocomotionSpeed(mountInstance != 0 ? mountInstance : instanceId, speed);
    const bool fellFar = playerAnimationSeen_.fellFar;
    syncPlayerAnimation(static_cast<const game::Unit&>(*self));
    // The jump's own sound, its mount's where it rides one (0x00747310).
    const auto jumpSound = [&](uint32_t slot) {
        if (entitySpawner_) entitySpawner_->playUnitJumpSound(static_cast<const game::Unit&>(*self), slot);
    };
    if (op == game::Opcode::MSG_MOVE_FALL_LAND) {
        const auto landed = rendering::landed(flagsBefore | (fellFar ? kMoveFallingFar : 0u), flags,
                                              launchedBefore, speed > speeds.walk + speeds.walk);
        switch (landed) {
            case rendering::Landed::Nothing: break;
            case rendering::Landed::Base: cr->playBaseAnimation(instanceId); break;
            case rendering::Landed::End:
                jumpSound(rendering::unit_event::kSlotJumpEnd);
                cr->playAnimation(instanceId, rendering::anim::JUMP_END, false);
                break;
            case rendering::Landed::LandRun:
                jumpSound(rendering::unit_event::kSlotJumpEnd);
                cr->playAnimation(instanceId, rendering::anim::JUMP_LAND_RUN, false);
                break;
        }
        return;
    }
    uint32_t current = 0;
    float time = 0.0f, duration = 0.0f;
    bool fallPlaying = cr->getAnimationState(instanceId, current, time, duration) &&
                       current == rendering::anim::FALL;
    if (mountInstance != 0 && cr->getAnimationState(mountInstance, current, time, duration))
        fallPlaying = fallPlaying || current == rendering::anim::FALL;  // 0x007172e0
    // In a vehicle's seat that keeps the jump's animations from it
    // (VehicleFlags 0x4000000, 0x007571c0).
    const auto& movement = gameHandler->getMovementInfo();
    const bool seatHolds = movement.transportSeat >= 0 && entitySpawner_ &&
                           (entitySpawner_->vehicleFlags(movement.transportGuid) & 0x4000000u) != 0;
    const me::Reaction r = me::react(op, flags, fallPlaying, seatHolds);
    if (r.sound != me::Sound::None)
        jumpSound(r.sound == me::Sound::JumpStart ? rendering::unit_event::kSlotJumpStart
                                                  : rendering::unit_event::kSlotJumpEnd);
    switch (r.play) {
        case me::Play::Nothing: break;
        case me::Play::Base: cr->playBaseAnimation(instanceId); break;
        case me::Play::JumpStart: cr->playAnimation(instanceId, rendering::anim::JUMP_START, false); break;
        case me::Play::Fall: cr->playAnimation(instanceId, rendering::anim::FALL, true); break;
        case me::Play::JumpLandRun: cr->playAnimation(instanceId, rendering::anim::JUMP_LAND_RUN, false); break;
    }
}

void Application::syncRenderInstancesToEntities(float deltaTime) {
    auto creatureSyncStart = std::chrono::steady_clock::now();
    if (renderer && gameHandler && renderer->getCharacterRenderer()) {
        auto* charRenderer = renderer->getCharacterRenderer();
        // Weapons drawn or put away as each unit's items and sheath state say.
        entitySpawner_->updateUnitWeapons();
        entitySpawner_->refreshGuildTabards();
        glm::vec3 playerPos(0.0f);
        glm::vec3 playerRenderPos(0.0f);
        bool havePlayerPos = false;
        if (gameHandler->getPlayerGuid() != 0) {
            // The server does not continuously echo our own movement into
            // the cached player Entity. MovementInfo is the live canonical
            // position that we render and send to the server; using the
            // Entity here eventually distance-culls nearby enemies against
            // the player's old spawn position and freezes their visuals.
            const auto& movement = gameHandler->getMovementInfo();
            playerPos = glm::vec3(movement.x, movement.y, movement.z);
            playerRenderPos = core::coords::canonicalToRender(playerPos);
            havePlayerPos = true;
            if (auto self = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
                self && self->isUnit()) {
                syncPlayerDisplayModel(static_cast<const game::Unit&>(*self));
                syncPlayerAnimation(static_cast<const game::Unit&>(*self));
                // 0x006e8280: a new model of the player's shows its
                // ammunition (PLAYER_AMMO_ID) again (0x007206a0, 0x1e).
                if (const uint32_t inst = renderer ? renderer->getCharacterInstanceId() : 0;
                    inst != 0 && inst != ammoInstanceSeen_) {
                    ammoInstanceSeen_ = inst;
                    entitySpawner_->showUnitAmmo(gameHandler->getPlayerGuid(), inst,
                                                 entitySpawner_->itemDisplayId(gameHandler->getAmmoId()),
                                                 kAmmoAskForRanged);
                }
            }
        }
        const float syncRadiusSq = 320.0f * 320.0f;
        auto& _creatureInstances = entitySpawner_->getCreatureInstances();
        auto& _creatureRenderPosCache = entitySpawner_->getCreatureRenderPosCache();
        auto& _creatureSwimmingState = entitySpawner_->getCreatureSwimmingState();
        auto& _creatureFlyingState = entitySpawner_->getCreatureFlyingState();
        auto& _creatureWasMoving = entitySpawner_->getCreatureWasMoving();
        auto& _creatureWasSwimming = entitySpawner_->getCreatureWasSwimming();
        auto& _creatureWasFlying = entitySpawner_->getCreatureWasFlying();
        auto& _creatureWasWalking = entitySpawner_->getCreatureWasWalking();
        const uint64_t currentTargetGuid = gameHandler->hasTarget()
            ? gameHandler->getTargetGuid() : 0;
        const uint64_t autoAttackGuid = gameHandler->getAutoAttackTargetGuid();
        static uint32_t creatureFloorFrame = 0;
        const uint32_t floorFrameNo = ++creatureFloorFrame;
        for (const auto& [guid, instanceId] : _creatureInstances) {
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            // A unit, or a player drawn as its display (a form or a morph).
            if (!entity || !entity->isUnit()) continue;

            // By where it is, not by where its path ends: the end of one that
            // goes round a city is out of the radius while the unit walks by.
            const glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            float canonDistSq = 0.0f;
            if (havePlayerPos) {
                glm::vec3 d = canonical - playerPos;
                canonDistSq = glm::dot(d, d);
                const bool activeCombatTarget =
                    guid == currentTargetGuid || guid == autoAttackGuid;
                if (canonDistSq > syncRadiusSq && !activeCombatTarget) continue;
            }

            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            auto posIt = _creatureRenderPosCache.find(guid);
            const std::optional<glm::vec3> previousRenderPos =
                posIt != _creatureRenderPosCache.end()
                    ? std::optional<glm::vec3>(posIt->second)
                    : std::nullopt;

            // On the ground only a unit moved on by its flags is put on the
            // floor (unitOnFloorWhileMoving); one on a spline is drawn at the
            // spline's height. A creature riding (UNIT_FIELD_MOUNTDISPLAYID)
            // is drawn on its mount as a player is: the mount where the unit
            // stands, the rider up on its seat (0x0073d5d0).
            const auto* creatureMount = entitySpawner_->getRemotePlayerMount(guid);
            std::optional<glm::vec3> previousGroundPos = previousRenderPos;
            if (creatureMount && previousGroundPos) previousGroundPos->z -= creatureMount->riderHeight;
            const bool groundCreature = !_creatureFlyingState.count(guid) &&
                                        !_creatureSwimmingState.count(guid);
            if (unitOnFloorWhileMoving(*entity) && groundCreature) {
                if (auto floorZ = unitMovingFloor(guid, renderer.get(), renderPos,
                                                  previousGroundPos, floorFrameNo)) {
                    renderPos.z = heightOverFloor(*entity, renderPos.z, *floorZ);
                }
            }
            const glm::vec3 creatureMountPos = renderPos;
            if (creatureMount) renderPos.z += creatureMount->riderHeight;

            // No visual collision guard here any more.
            //
            // A creature closer than about two yards used to have its *render*
            // position pushed away from the player, so that a hostile one could
            // not appear to stand inside them while attacking. The cost was
            // that walking up to any hostile NPC made it slide backwards, which
            // is not what WoW does: units have no collision against each other
            // there at all, and a mob overlapping the player is ordinary. It
            // also drew every affected creature somewhere other than where it
            // actually was.

            // The model turns towards the movement facing (FUN_00735f60).
            float bodyYaw = entity->getModelFacing() + glm::radians(90.0f);
            if (posIt == _creatureRenderPosCache.end()) {
                charRenderer->setInstancePosition(instanceId, renderPos);
                if (creatureMount) charRenderer->setInstancePosition(creatureMount->instanceId, creatureMountPos);
                _creatureRenderPosCache[guid] = renderPos;
            } else {
                const glm::vec3 prevPos = posIt->second;
                float ddx2 = renderPos.x - prevPos.x;
                float ddy2 = renderPos.y - prevPos.y;
                float planarDistSq = ddx2 * ddx2 + ddy2 * ddy2;
                float dz = std::abs(renderPos.z - prevPos.z);

                auto unitPtr = std::static_pointer_cast<game::Unit>(entity);
                const bool deadOrCorpse = unitPtr->getHealth() == 0;
                const bool largeCorrection = (planarDistSq > 36.0f) || (dz > 3.0f);
                // Use isActivelyMoving() so Run/Walk animation stops when the
                // creature reaches its destination, and not the change of
                // position (planarDistSq) while it has a move of its own. Only
                // fall back to position-
                // change detection for entities with no active movement tracking
                // (e.g. teleports or position-only updates from the server).
                const bool entityIsMoving = entity->isActivelyMoving();
                constexpr float kMoveThreshSq = 0.03f * 0.03f;
                const bool posChanging = planarDistSq > kMoveThreshSq || dz > 0.08f;
                const bool transportAttached =
                    gameHandler->transportAttachmentsRef().count(guid) != 0;
                // A stationary deck passenger changes world position every
                // frame because the parent ship moves. That parent motion is
                // not creature locomotion and must not trigger Run. Real
                // transport-local spline movement still reports actively moving.
                const bool positionOnlyLocomotion =
                    posChanging && !entity->isEntityMoving() && !transportAttached;
                // In a seat its own movement flags carry no direction: its
                // vehicle moves it (0x00717050 reads the unit's).
                const bool isMovingNow = !deadOrCorpse && !charRenderer->isInstanceSeated(instanceId) &&
                                         !entity->fallsInPlace() && (entityIsMoving || positionOnlyLocomotion);
                if (creatureMount) charRenderer->setInstancePosition(creatureMount->instanceId, creatureMountPos);
                if (deadOrCorpse || largeCorrection) {
                    charRenderer->setInstancePosition(instanceId, renderPos);
                } else if (renderPos != prevPos) {
                    // Every step, however small: a threshold here compares one
                    // frame's step, and a cockroach at under a yard a second
                    // stays below it frame after frame. It stood still and then
                    // jumped whenever a long frame made one step big enough.
                    // Entity::updateMovement already evaluates the server spline for
                    // this frame. Starting another renderer interpolation here resets
                    // that interpolation every frame and leaves the model trailing its
                    // authoritative entity position. Copy the evaluated position
                    // directly; animation transitions remain driven below.
                    charRenderer->setInstancePosition(instanceId, renderPos);
                }
                // When entity is moving but getX/Y/Z is stale (distance-culled),
                // don't call moveInstanceTo - creatureMoveCallback_ already drove
                // the renderer to the correct destination via the spline packet.
                posIt->second = renderPos;

                feedUnitRipple(*renderer, *entitySpawner_, guid, instanceId, *unitPtr, creatureMountPos,
                               isMovingNow && !deadOrCorpse, deadOrCorpse, deltaTime);

                // The speed its movement animation plays to: the spline's,
                // as the client's GetCurrentSpeed gives it for a unit on one.
                charRenderer->setLocomotionSpeed(
                    instanceId, isMovingNow ? entity->getMoveSpeed() : 0.0f);
                if (creatureMount)
                    charRenderer->setLocomotionSpeed(creatureMount->instanceId,
                                                     isMovingNow ? entity->getMoveSpeed() : 0.0f);
                syncUnitAnimation(*charRenderer, *gameHandler, *entitySpawner_, renderer->getSpellVisualSystem(), guid,
                                  instanceId, *unitPtr,
                                  isMovingNow, _creatureSwimmingState.count(guid) > 0, _creatureFlyingState.count(guid) > 0,
                                  creatureMount ? creatureMount->instanceId : 0u);

                // Drive movement animation: Walk/Run/Swim (4/5/42) when moving,
                // Stand/SwimIdle (0/41) when idle. Walk(4) selected when WALKING flag is set.
                // WoW M2 animation IDs: 4=Walk, 5=Run, 41=SwimIdle, 42=Swim.
                // Only switch on transitions to avoid resetting animation time.
                // Don't override Death (1) animation.
                const bool isSwimmingNow = _creatureSwimmingState.count(guid) > 0;
                // Walk or Run by the pace, as FUN_00717050 picks it: Run when
                // the current speed is more than twice the unit's walk speed,
                // Walk otherwise. The speed is the spline's own (length over
                // duration, FUN_00987570), and no flag enters into it - the
                // client never copies a spline's walk mode into the unit's
                // flags. Patrolling guards are moved at walk pace with no
                // WALKING flag at all, and played Run slowed to that pace.
                const bool isWalkingNow  = (isMovingNow && entity->movesAtWalkPace());
                const bool isFlyingNow   = _creatureFlyingState.count(guid) > 0;
                bool prevMoving   = _creatureWasMoving[guid];
                bool prevSwimming = _creatureWasSwimming[guid];
                bool prevFlying   = _creatureWasFlying[guid];
                bool prevWalking  = _creatureWasWalking[guid];
                // Trigger animation update on any locomotion-state transition, not just
                // moving/idle - e.g. creature lands while still moving → FlyForward→Run,
                // or server changes WALKING flag while creature is already running → Walk.
                // A jump keeps its animations until it is down and landed.
                bool jumpRefresh = false;
                const bool locomotionAsked = entity->takeLocomotionRequest();
                const bool baseAsked = entity->takeBaseAnimationRequest();
                const bool jumpHolds = !deadOrCorpse &&
                    jumpHoldsAnimation(*charRenderer, creatureMount ? creatureMount->instanceId : instanceId,
                                       *entity, isFlyingNow, locomotionAsked, jumpRefresh);
                // A rider's mount is asked again on each movement packet that
                // asks it (0x0073ed10), so one swimming on to the side turns
                // to it.
                const bool stateChanged = !jumpHolds &&
                                          (jumpRefresh || baseAsked ||
                                           (isMovingNow  != prevMoving)   ||
                                           (isSwimmingNow != prevSwimming) ||
                                           (isFlyingNow   != prevFlying)   ||
                                           (isWalkingNow  != prevWalking && isMovingNow) ||
                                           (creatureMount && locomotionAsked));
                if (stateChanged) {
                    _creatureWasMoving[guid]   = isMovingNow;
                    _creatureWasSwimming[guid] = isSwimmingNow;
                    _creatureWasFlying[guid]   = isFlyingNow;
                    _creatureWasWalking[guid]  = isWalkingNow;
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                    if (!gotState || curAnimId != rendering::anim::DEATH) {
                        // What it is in now, as the client asks it on
                        // each such change (0x0073ac30 -> 0x00724500):
                        // the walk, run, swim or fall; its ready in a
                        // fight; its stand state's loop; its emote
                        // state's; the stand. A rider's mount plays it
                        // (0x007385c0), the rider holds its pose (+0xb7c).
                        charRenderer->playBaseAnimation(instanceId);
                    }
                }
                // A rider's seat pose outlasts emotes and state updates.
                if (creatureMount) {
                    const uint32_t pose = entitySpawner_->riderPose(guid, instanceId);
                    uint32_t riderAnim = 0;
                    float riderTime = 0.0f, riderDuration = 0.0f;
                    const bool haveRiderState =
                        charRenderer->getAnimationState(instanceId, riderAnim, riderTime, riderDuration);
                    if ((!haveRiderState || riderAnim != pose) &&
                        riderAnim != rendering::anim::DEATH) {
                        charRenderer->playAnimation(instanceId, pose, /*loop=*/true);
                    }
                }
                // Its body, behind or aside of that facing (FUN_0073dab0).
                bodyYaw = turnUnitBody(*charRenderer, instanceId, entitySpawner_->getUnitBodies()[guid],
                                       bodyYaw,
                                       unitBodyFlags(*entity, isMovingNow, isSwimmingNow, isFlyingNow),
                                       deltaTime, deadOrCorpse,
                                       !jumpHolds && !isMovingNow && !deadOrCorpse &&
                                           unitPtr->getStandState() == 0,
                                       creatureMount ? creatureMount->instanceId : 0u,
                                       unitRideOf(*gameHandler, guid), unitRideGuidOf(*gameHandler, guid));
            }
            // Tilted by its pitch while it swims or flies forward or back
            // (swim_pitch::otherUnit, 0x00719a90): the model's own Y axis,
            // nose up positive, as the player's (Renderer).
            const float unitTilt = unitSwimTilt(*gameHandler, *entity, guid,
                                                _creatureSwimmingState.count(guid) > 0 ||
                                                    _creatureFlyingState.count(guid) > 0);
            charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, -unitTilt, bodyYaw));
            if (creatureMount)
                charRenderer->setInstanceRotation(creatureMount->instanceId, glm::vec3(0.0f, -unitTilt, bodyYaw));
        }
    }
    {
        float csMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - creatureSyncStart).count();
        if (csMs > 5.0f) {
            LOG_WARNING("SLOW update stage 'creature render sync': ", csMs, "ms (",
                        entitySpawner_->getCreatureInstances().size(), " creatures)");
        }
    }

    // --- Online player render sync (position, orientation, animation) ---
    // Mirrors the creature sync loop above but without collision guard or
    // weapon-attach logic.  Without this, online players never transition
    // back to Stand after movement stops ("run in place" bug).
    auto playerSyncStart = std::chrono::steady_clock::now();
    if (renderer && gameHandler && renderer->getCharacterRenderer()) {
        auto* charRenderer = renderer->getCharacterRenderer();
        glm::vec3 pPos(0.0f);
        bool havePPos = false;
        if (gameHandler->getPlayerGuid() != 0) {
            const auto& movement = gameHandler->getMovementInfo();
            pPos = glm::vec3(movement.x, movement.y, movement.z);
            havePPos = true;
        }
        const float pSyncRadiusSq = 320.0f * 320.0f;

        auto& _playerInstances = entitySpawner_->getPlayerInstances();
        auto& _pCreatureWasMoving = entitySpawner_->getCreatureWasMoving();
        auto& _pCreatureWasSwimming = entitySpawner_->getCreatureWasSwimming();
        auto& _pCreatureWasFlying = entitySpawner_->getCreatureWasFlying();
        auto& _pCreatureWasWalking = entitySpawner_->getCreatureWasWalking();
        auto& _pCreatureSwimmingState = entitySpawner_->getCreatureSwimmingState();
        auto& _pCreatureFlyingState = entitySpawner_->getCreatureFlyingState();
        auto& _pCreatureRenderPosCache = entitySpawner_->getCreatureRenderPosCache();
        const uint64_t playerTargetGuid = gameHandler->hasTarget()
            ? gameHandler->getTargetGuid() : 0;
        const uint64_t playerAutoAttackGuid = gameHandler->getAutoAttackTargetGuid();
        static uint32_t playerFloorFrame = 0;
        const uint32_t floorFrameNo = ++playerFloorFrame;
        for (const auto& [guid, instanceId] : _playerInstances) {
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;

            // Distance cull, by where it is (as above).
            const glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            if (havePPos) {
                glm::vec3 d = canonical - pPos;
                const bool activeCombatTarget =
                    guid == playerTargetGuid || guid == playerAutoAttackGuid;
                if (glm::dot(d, d) > pSyncRadiusSq && !activeCombatTarget) continue;
            }

            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            auto posIt = _pCreatureRenderPosCache.find(guid);
            const std::optional<glm::vec3> previousRenderPos =
                posIt != _pCreatureRenderPosCache.end()
                    ? std::optional<glm::vec3>(posIt->second)
                    : std::nullopt;
            const auto* remoteMount = entitySpawner_->getRemotePlayerMount(guid);
            std::optional<glm::vec3> previousMountPos = previousRenderPos;
            if (remoteMount && previousMountPos) {
                previousMountPos->z -= remoteMount->riderHeight;
            }

            // As the creatures: on the floor only while moved on by its flags.
            const bool groundPlayer = !_pCreatureFlyingState.count(guid) &&
                                      !_pCreatureSwimmingState.count(guid);
            if (unitOnFloorWhileMoving(*entity) && groundPlayer) {
                if (auto floorZ = unitMovingFloor(guid, renderer.get(), renderPos,
                                                  previousMountPos, floorFrameNo)) {
                    renderPos.z = heightOverFloor(*entity, renderPos.z, *floorZ);
                }
            }

            const glm::vec3 mountRenderPos = renderPos;
            if (remoteMount) renderPos.z += remoteMount->riderHeight;

            // Orientation sync: the model turns towards the movement facing
            // rather than snapping to it, as the client draws other players
            // (FUN_00735f60).
            float bodyYaw = entity->getModelFacing() + glm::radians(90.0f);
            if (posIt == _pCreatureRenderPosCache.end()) {
                charRenderer->setInstancePosition(instanceId, renderPos);
                if (remoteMount) {
                    charRenderer->setInstancePosition(remoteMount->instanceId, mountRenderPos);
                }
                _pCreatureRenderPosCache[guid] = renderPos;
            } else {
                const glm::vec3 prevPos = posIt->second;
                float ddx2 = renderPos.x - prevPos.x;
                float ddy2 = renderPos.y - prevPos.y;
                float planarDistSq = ddx2 * ddx2 + ddy2 * ddy2;
                float dz = std::abs(renderPos.z - prevPos.z);

                auto unitPtr = std::static_pointer_cast<game::Unit>(entity);
                const bool deadOrCorpse = unitPtr->getHealth() == 0;
                const bool largeCorrection = (planarDistSq > 36.0f) || (dz > 3.0f);
                const bool entityIsMoving = entity->isActivelyMoving();
                constexpr float kMoveThreshSq2 = 0.03f * 0.03f;
                const bool posChanging2 = planarDistSq > kMoveThreshSq2 || dz > 0.08f;
                const bool transportAttached =
                    gameHandler->transportAttachmentsRef().count(guid) != 0;
                const bool positionOnlyLocomotion =
                    posChanging2 && !entity->isEntityMoving() && !transportAttached;
                // In a seat its own movement flags carry no direction: its
                // vehicle moves it (0x00717050 reads the unit's).
                const bool isMovingNow = !deadOrCorpse && !charRenderer->isInstanceSeated(instanceId) &&
                                         !entity->fallsInPlace() && (entityIsMoving || positionOnlyLocomotion);

                if (deadOrCorpse || largeCorrection) {
                    charRenderer->setInstancePosition(instanceId, renderPos);
                    if (remoteMount) {
                        charRenderer->setInstancePosition(remoteMount->instanceId, mountRenderPos);
                    }
                } else if (renderPos != prevPos) {
                    float planarDist = std::sqrt(planarDistSq);
                    float duration = std::clamp(planarDist / 5.5f, 0.05f, 0.22f);
                    charRenderer->moveInstanceTo(instanceId, renderPos, duration);
                    if (remoteMount) {
                        charRenderer->moveInstanceTo(remoteMount->instanceId, mountRenderPos, duration);
                    }
                }
                posIt->second = renderPos;
                feedUnitRipple(*renderer, *entitySpawner_, guid, instanceId, *unitPtr, mountRenderPos,
                               isMovingNow && !deadOrCorpse, deadOrCorpse, deltaTime);

                // The speed the player's (or its mount's) movement animation
                // plays to - see the creature loop above.
                const float locomotionSpeed = isMovingNow ? entity->getMoveSpeed() : 0.0f;
                charRenderer->setLocomotionSpeed(instanceId, locomotionSpeed);
                if (remoteMount) {
                    charRenderer->setLocomotionSpeed(remoteMount->instanceId, locomotionSpeed);
                }
                syncUnitAnimation(*charRenderer, *gameHandler, *entitySpawner_, renderer->getSpellVisualSystem(), guid,
                                  instanceId, *unitPtr,
                                  isMovingNow, _pCreatureSwimmingState.count(guid) > 0, _pCreatureFlyingState.count(guid) > 0,
                                  remoteMount ? remoteMount->instanceId : 0u);

                // Drive movement animation (same logic as creatures)
                const bool isSwimmingNow = _pCreatureSwimmingState.count(guid) > 0;
                const bool isWalkingNow  = (isMovingNow && entity->movesAtWalkPace());  // FUN_00717050
                const bool isFlyingNow   = _pCreatureFlyingState.count(guid) > 0;
                // In its rider pose (+0xb7c, Mount unless a mount aura's
                // kit gave another), in the air too: 3.3.5 has no rider
                // flight poses.
                const uint32_t mountedRiderAnim = entitySpawner_->riderPose(guid, instanceId);
                bool prevMoving   = _pCreatureWasMoving[guid];
                bool prevSwimming = _pCreatureWasSwimming[guid];
                bool prevFlying   = _pCreatureWasFlying[guid];
                bool prevWalking  = _pCreatureWasWalking[guid];
                // A jump keeps its animations until it is down and landed -
                // a mounted player's on the mount, the rider keeping the
                // seat pose.
                bool jumpRefresh = false;
                const bool locomotionAsked = entity->takeLocomotionRequest();
                const bool jumpHolds = !deadOrCorpse &&
                    jumpHoldsAnimation(*charRenderer,
                                       remoteMount ? remoteMount->instanceId : instanceId,
                                       *entity, isFlyingNow, locomotionAsked, jumpRefresh);
                const bool stateChanged = !jumpHolds &&
                                          (jumpRefresh ||
                                           (isMovingNow  != prevMoving)   ||
                                           (isSwimmingNow != prevSwimming) ||
                                           (isFlyingNow   != prevFlying)   ||
                                           (isWalkingNow  != prevWalking && isMovingNow) ||
                                           (remoteMount && locomotionAsked));
                if (stateChanged) {
                    _pCreatureWasMoving[guid]   = isMovingNow;
                    _pCreatureWasSwimming[guid] = isSwimmingNow;
                    _pCreatureWasFlying[guid]   = isFlyingNow;
                    _pCreatureWasWalking[guid]  = isWalkingNow;
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                    if (!gotState || curAnimId != rendering::anim::DEATH) {
                        // What it is in now (0x0073ac30 -> 0x00724500), on
                        // its mount where it rides (0x007385c0).
                        charRenderer->playBaseAnimation(instanceId);
                    }
                }

                // Server emotes and state updates can arrive after the mount
                // field and replace the one-shot mounted pose with Stand. A
                // rider's mount field is authoritative, so repair that pose
                // even when their movement state did not transition this frame.
                if (remoteMount) {
                    uint32_t riderAnim = 0;
                    float riderTime = 0.0f, riderDuration = 0.0f;
                    const bool haveRiderState = charRenderer->getAnimationState(
                        instanceId, riderAnim, riderTime, riderDuration);
                    if ((!haveRiderState || riderAnim != mountedRiderAnim) &&
                        riderAnim != rendering::anim::DEATH) {
                        charRenderer->playAnimation(instanceId, mountedRiderAnim,
                                                    /*loop=*/true);
                    }
                }
                // Its body, behind or aside of that facing (FUN_0073dab0): a
                // strafing player's turned to the side, a turn on the spot
                // shuffled.
                bodyYaw = turnUnitBody(*charRenderer, instanceId, entitySpawner_->getUnitBodies()[guid],
                                       bodyYaw,
                                       unitBodyFlags(*entity, isMovingNow, isSwimmingNow, isFlyingNow),
                                       deltaTime, deadOrCorpse,
                                       !jumpHolds && !isMovingNow && !deadOrCorpse &&
                                           unitPtr->getStandState() == 0,
                                       remoteMount ? remoteMount->instanceId : 0u,
                                       unitRideOf(*gameHandler, guid), unitRideGuidOf(*gameHandler, guid));
            }
            const float unitTilt = unitSwimTilt(*gameHandler, *entity, guid,
                                                _pCreatureSwimmingState.count(guid) > 0 ||
                                                    _pCreatureFlyingState.count(guid) > 0);
            charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, -unitTilt, bodyYaw));
            if (remoteMount) {
                charRenderer->setInstanceRotation(remoteMount->instanceId,
                                                  glm::vec3(0.0f, -unitTilt, bodyYaw));
            }
        }
    }
    {
        float psMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - playerSyncStart).count();
        if (psMs > 5.0f) {
            LOG_WARNING("SLOW update stage 'player render sync': ", psMs, "ms (",
                        entitySpawner_->getPlayerInstances().size(), " players)");
        }
    }

    // Which units have a blob shadow under them this frame, and how big;
    // and what their models' events play.
    if (entitySpawner_ && renderer) {
        entitySpawner_->updateBlobShadows(renderer->getCharacterInstanceId());
        entitySpawner_->playUnitModelEvents(renderer->getCharacterInstanceId());
        entitySpawner_->playUnitSplashes(renderer->getCharacterInstanceId());
        entitySpawner_->updateObjectEffects(deltaTime);
        entitySpawner_->updateUnitLoopSounds();
    }
}

void Application::updateInGame(float deltaTime, const char*& updateCheckpoint) {
    updateCheckpoint = "in_game: enter";
    const char* inGameStep = "begin";
    try {
    auto runInGameStage = [&](const char* stageName, auto&& fn) {
        auto stageStart = std::chrono::steady_clock::now();
        const core::ProfileScope profileScope(stageName, core::ProfileKind::Cpu);
        const stall_report::StageScope stallStage(stageName);
        try {
            fn();
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during IN_GAME update stage '", stageName, "': ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during IN_GAME update stage '", stageName, "': ", e.what());
            throw;
        }
        auto stageEnd = std::chrono::steady_clock::now();
        float stageMs = std::chrono::duration<float, std::milli>(stageEnd - stageStart).count();
        noteStageTime(stageName, stageMs);
        if (stageMs > 50.0f) {
            LOG_WARNING("SLOW update stage '", stageName, "': ", stageMs, "ms");
        }
    };
    inGameStep = "gameHandler update";
    updateCheckpoint = "in_game: gameHandler update";
#ifdef WOWEE_MOBILE
    ui::touchControls().update();
    if (renderer && renderer->getCameraController()) {
        auto* cam = renderer->getCameraController();
        ui::touchControls().setCameraController(cam);
        // The touch controls turn the view themselves, from whichever finger is
        // doing it. SDL's own translation of the first finger must not turn it
        // as well, or a drag counts twice and the stick swings the camera.
        cam->setRotationSuppressed(true);
        cam->setSteering(ui::touchControls().isSteering() ||
                         ui::gamepadControls().isSteering());
    }
#endif
    runInGameStage("gameHandler->update", [&] {
        if (gameHandler) {
            gameHandler->update(deltaTime);
        }
    });
    // Not behind the loading screen. The client loads a world inside one
    // call (0x00403b70 -> 0x00781430) whose progress callbacks (0x0040aef0,
    // 0x0040af40 -> 0x0040a920) pump the sound and draw the loading screen
    // and nothing else: no frame's OnUpdate runs until the screen is gone.
    // The world settling behind it here (stepWorldSettle) ran them, and a
    // script's timer ran out there - a jump it pressed went to a mover held
    // still behind the screen, and neither the jump nor its MSG_MOVE_JUMP
    // happened.
    if (addonManager_ && addonsLoaded_ && !settleScreen_) {
        addonManager_->update(deltaTime);
    }
    inGameStep = "auto-unsheathe";
    updateCheckpoint = "in_game: auto-unsheathe";
    if (gameHandler) {
        const bool attackWeaponNeeded = gameHandler->isAutoAttacking();
        // Starting an attack draws melee (0x006e2610, with no sheath
        // animation).
        if (attackWeaponNeeded && !wasAutoAttacking_ && appearanceComposer_) {
            appearanceComposer_->requestSheathState(core::SheathState::Melee);
        }
        wasAutoAttacking_ = attackWeaponNeeded;
    }

    // The weapons as the fields hold them. Z itself is the TOGGLESHEATH
    // binding, run through the interface's bindings like any other key
    // (Bindings.xml -> ToggleSheath, 0x0051a6b0 -> 0x006e23a0).
    inGameStep = "weapon-toggle input";
    updateCheckpoint = "in_game: weapon-toggle input";
    if (appearanceComposer_) appearanceComposer_->updateWeaponsFromFields();

    inGameStep = "world update";
    updateCheckpoint = "in_game: world update";
    runInGameStage("world->update", [&] {
        if (world) {
            world->update(deltaTime);
        }
    });
    inGameStep = "spawn/equipment queues";
    updateCheckpoint = "in_game: spawn/equipment queues";
    runInGameStage("spawn/equipment queues", [&] {
        if (entitySpawner_) entitySpawner_->update();
        if (auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr) {
            cr->processPendingNormalMaps(4);
        }
        if (auto* wr = renderer ? renderer->getWMORenderer() : nullptr) {
            wr->processNormalMapBackfill();
        }
    });
    // Every unit's channel, whose ChannelKit it holds while the channel
    // lasts (0x0073eb50, 0x0072bc70).
    if (auto* svs = renderer ? renderer->getSpellVisualSystem() : nullptr; svs && gameHandler) {
        std::vector<rendering::SpellVisualSystem::ChannelState> channels;
        const uint16_t channelField = game::fieldIndex(game::UF::UNIT_FIELD_CHANNEL_OBJECT);
        const uint16_t spellField = game::fieldIndex(game::UF::UNIT_CHANNEL_SPELL);
        if (channelField != 0xFFFF && spellField != 0xFFFF) {
            for (const auto& [guid, entity] : gameHandler->getEntityManager().getEntities()) {
                if (!entity || !entity->isUnit()) continue;
                const uint32_t spell = entity->getField(spellField);
                if (spell == 0) continue;
                const uint64_t object = static_cast<uint64_t>(entity->getField(channelField)) |
                    (static_cast<uint64_t>(entity->getField(static_cast<uint16_t>(channelField + 1))) << 32);
                channels.push_back({.unitGuid = guid, .spellId = spell, .channelObject = object});
            }
        }
        svs->setUnitChannels(channels);
    }
    // Fishing lines (0x007221d0): every unit channelling a spell at a
    // bobber - its UNIT_FIELD_CHANNEL_OBJECT a game object of type 17 - with
    // something in its right hand.
    if (auto* lines = renderer ? renderer->getFishingLineRenderer() : nullptr; lines && gameHandler && entitySpawner_) {
        std::vector<rendering::FishingLineRenderer::Line> found;
        const uint16_t channelField = game::fieldIndex(game::UF::UNIT_FIELD_CHANNEL_OBJECT);
        const uint16_t spellField = game::fieldIndex(game::UF::UNIT_CHANNEL_SPELL);
        const uint16_t goBytes1 = game::fieldIndex(game::UF::GAMEOBJECT_BYTES_1);
        auto& entities = gameHandler->getEntityManager();
        if (channelField != 0xFFFF && spellField != 0xFFFF) {
            for (const auto& [guid, entity] : entities.getEntities()) {
                if (!entity || (entity->getType() != game::ObjectType::UNIT &&
                                entity->getType() != game::ObjectType::PLAYER)) continue;
                if (entity->getField(spellField) == 0) continue;
                const uint64_t object = static_cast<uint64_t>(entity->getField(channelField)) |
                    (static_cast<uint64_t>(entity->getField(static_cast<uint16_t>(channelField + 1))) << 32);
                if (object == 0) continue;
                auto bobber = entities.getEntity(object);
                if (!bobber || bobber->getType() != game::ObjectType::GAMEOBJECT) continue;
                const auto* info = gameHandler->getCachedGameObjectInfo(
                    static_cast<const game::GameObject&>(*bobber).getEntry());
                const uint32_t type = info ? info->type
                                           : (goBytes1 != 0xFFFF ? (bobber->getField(goBytes1) >> 8) & 0xFFu : 0);
                if (type != core::kGameObjectTypeFishingNode) continue;
                const auto& objects = entitySpawner_->getGameObjectInstances();
                auto goIt = objects.find(object);
                if (goIt == objects.end() || goIt->second.isWmo) continue;
                const uint32_t unitInstance = gameHandler->resolveUnitRenderInstance(guid);
                if (unitInstance == 0) continue;
                found.push_back({.unitInstance = unitInstance, .bobberInstance = goIt->second.instanceId});
            }
        }
        lines->setLines(std::move(found));
    }
    // Self-heal missing creature visuals: if a nearby UNIT exists in
    // entity state but has no render instance, queue a spawn retry.
    inGameStep = "creature resync scan";
    updateCheckpoint = "in_game: creature resync scan";
    if (gameHandler) {
        static float creatureResyncTimer = 0.0f;
        creatureResyncTimer += deltaTime;
        if (creatureResyncTimer >= 3.0f) {
            creatureResyncTimer = 0.0f;

            glm::vec3 playerPos(0.0f);
            bool havePlayerPos = false;
            uint64_t playerGuid = gameHandler->getPlayerGuid();
            if (auto playerEntity = gameHandler->getEntityManager().getEntity(playerGuid)) {
                playerPos = glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ());
                havePlayerPos = true;
            }

            // Counters for the summary below. "No NPCs at all" has three
            // possible causes that look identical in-world - the server
            // sent no units, the units exist but carry no display id, or
            // they exist and the spawner is refusing them - and the
            // difference decides where to look.
            int unitsSeen = 0, unitsNoDisplay = 0, unitsSpawned = 0, unitsQueued = 0;

            const float kResyncRadiusSq = 260.0f * 260.0f;
            for (const auto& pair : gameHandler->getEntityManager().getEntities()) {
                uint64_t guid = pair.first;
                const auto& entity = pair.second;
                if (!entity || guid == playerGuid) continue;
                if (entity->getType() != game::ObjectType::UNIT) continue;
                unitsSeen++;
                auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
                if (!unit || unit->getDisplayId() == 0) { unitsNoDisplay++; continue; }
                if (entitySpawner_->isCreatureSpawned(guid)) { unitsSpawned++; continue; }
                if (entitySpawner_->isCreaturePending(guid)) { unitsQueued++; continue; }

                if (havePlayerPos) {
                    glm::vec3 pos(unit->getX(), unit->getY(), unit->getZ());
                    glm::vec3 delta = pos - playerPos;
                    float distSq = glm::dot(delta, delta);
                    if (distSq > kResyncRadiusSq) continue;
                }

                float retryScale = 1.0f;
                {
                    using game::fieldIndex; using game::UF;
                    uint16_t si = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
                    if (si != 0xFFFF) {
                        uint32_t raw = unit->getField(si);
                        if (raw != 0) {
                            float s2 = 1.0f;
                            s2 = std::bit_cast<float>(raw);
                            if (s2 > 0.01f && s2 < 100.0f) retryScale = s2;
                        }
                    }
                }
                entitySpawner_->queueCreatureSpawn(guid, unit->getDisplayId(),
                    unit->getX(), unit->getY(), unit->getZ(),
                    unit->getOrientation(), retryScale);
            }

            // Silent during normal play: this only speaks up when the
            // world holds units and none of them have a model.
            if (unitsSeen > 0 && unitsSpawned == 0) {
                LOG_WARNING("No creature models for ", unitsSeen,
                            " units in range (", unitsNoDisplay,
                            " without a display id, ", unitsQueued,
                            " queued, lookups built=",
                            entitySpawner_->areCreatureLookupsBuilt() ? "yes" : "no", ")");
            } else if (unitsSeen == 0) {
                static float noUnitsFor = 0.0f;
                noUnitsFor += 3.0f;
                if (noUnitsFor >= 9.0f) {
                    noUnitsFor = 0.0f;
                    LOG_WARNING("No UNIT entities in range at all - the server has "
                                "sent no creatures, or their update blocks were dropped");
                }
            }
        }
    }

    inGameStep = "gameobject/transport queues";
    updateCheckpoint = "in_game: gameobject/transport queues";
    runInGameStage("gameobject/transport queues", [&] {
        // GO/transport queues handled by entitySpawner_->update() above
    });
    inGameStep = "pending mount";
    updateCheckpoint = "in_game: pending mount";
    runInGameStage("processPendingMount", [&] {
        // Mount processing handled by entitySpawner_->update() above
    });
    // Update 3D quest markers above NPCs
    inGameStep = "quest markers";
    updateCheckpoint = "in_game: quest markers";
    runInGameStage("updateQuestMarkers", [&] {
        updateQuestMarkers();
        updateWorldFloorSound();
    });
    runInGameStage("updateFlightCeiling", [&] {
        updateFlightCeiling();
    });
    runInGameStage("updateLootSparkles", [&] {
        updateLootSparkles();
    });
    // Sync server run speed to camera controller
    inGameStep = "post-update sync";
    updateCheckpoint = "in_game: post-update sync";
    runInGameStage("post-update sync", [&] { applyServerMovementState(deltaTime); });

    // Keep creature render instances aligned with authoritative entity positions.
    // This prevents desync where target circles move with server entities but
    // creature models remain at stale spawn positions.
    inGameStep = "creature render sync";
    updateCheckpoint = "in_game: creature render sync";
    { WOWEE_PROFILE_SCOPE("render sync: units", Cpu); syncRenderInstancesToEntities(deltaTime); }
    // Passengers onto their vehicles' seats, over where the sync put them.
    if (entitySpawner_) entitySpawner_->updateVehiclePassengers();
    // Movement heartbeat is sent from GameHandler::update() to avoid
    // duplicate packets from multiple update loops.

    } catch (const std::bad_alloc& e) {
        LOG_ERROR("OOM inside AppState::IN_GAME at step '", inGameStep, "': ", e.what());
        throw;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception inside AppState::IN_GAME at step '", inGameStep, "': ", e.what());
        throw;
    }
}

void Application::update(float deltaTime) {
    ZoneScopedN("Application::update");
    const char* updateCheckpoint = "enter";
    try {
    // A reload asked for by the interface. Done here, between frames, because
    // the request comes from inside the Lua state that the reload destroys -
    // performing it at the call site would free the machinery mid-call.
    if (reloadUiPending_) {
        reloadUiPending_ = false;
        if (addonManager_) {
            // ReloadUI (0x005222e0) only sets DAT_00bd0791; the frame then
            // takes the interface down exactly as logout does (0x0052af40:
            // 0x00528f00) before loading it again (0x0052a980), so the
            // interface hears it leave the world and log out - and saves
            // what it saves on PLAYER_LOGOUT - before its state is gone.
            if (addonsLoaded_) fireInterfaceLeaving();
            addonManager_->reload();
            // The same three the /reload command sends, so an interface
            // reloaded from a popup comes up in the same state as one reloaded
            // from chat rather than waiting for events that already fired.
            // Silent through the login burst, for the same reason the load
            // itself is: the unit frames initialize their dropdowns on these
            // events, every initializer ends in UnitPopup_ShowMenu, and that
            // ends in PlaySound("igMainMenuOpen"). Eight of them arrive inside
            // thirty milliseconds and stack into one hit a few seconds after
            // entering the world. The real client is behind a loading screen
            // here. See LuaEngine::setUiSoundsSuppressed.
            addons::LuaEngine::setUiSoundsSuppressed(true);
            addonManager_->fireEvent("VARIABLES_LOADED");
            // After VARIABLES_LOADED, which is when the chat window
            // settings are available to be read. Every chat frame
            // answers this by applying its saved font, colour, size
            // and docked state; without it they keep the defaults
            // they were built with however much was saved.
            addonManager_->fireEvent("UPDATE_CHAT_WINDOWS");
            addonManager_->fireEvent("PLAYER_LOGIN");
            addonManager_->fireEvent("PLAYER_ENTERING_WORLD");
            // The new interface has not heard the login's UPDATE_FACTION either
            // (see WorldLoader::loadOnlineWorldTerrain): the experience bar
            // against the max-level bar and the watched faction.
            addonManager_->fireEvent("UPDATE_FACTION");
            // 0x00528010 runs again for the new interface, and with it the
            // queue questions whose answers set up the dungeon finder.
            if (gameHandler && gameHandler->getState() == game::WorldState::IN_WORLD)
                gameHandler->requestEntryQueueStatus();
            // A player still fighting is told so again, which is what puts the
            // new interface into combat lockdown (0x00528010: after
            // PLAYER_ENTERING_WORLD, UNIT_FLAG_IN_COMBAT is event 0x98 and
            // +0x124c cleared) - the other half of what fireInterfaceLeaving
            // says on the way out.
            if (gameHandler) gameHandler->sayFightOnEntry();
            // Then the player's vehicle seat, said again (0x00749ed0).
            if (entitySpawner_) entitySpawner_->refirePlayerSeatEvents();
            addons::LuaEngine::setUiSoundsSuppressed(false);
        }
    }
    // Update based on current state
    // Streaming: what was fetched, every ten seconds while it changes.
    if (pipeline::DataStream::instance().active()) {
        static auto lastReport = std::chrono::steady_clock::now();
        static uint64_t lastFetched = ~0ull;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(10)) {
            lastReport = now;
            // Which chunks were read, for the cache limit; kept now and then
            // so a crash loses little of it.
            pipeline::DataStream::instance().saveUse();
            const auto t = pipeline::DataStream::instance().totals();
            if (t.fetchedBytes != lastFetched) {
                lastFetched = t.fetchedBytes;
                LOG_WARNING("STREAM: fetched ", t.fetchedBytes, " bytes in ", t.requests,
                            " requests this run; cache holds ", t.localBytes, " of ", t.mpqBytes,
                            " MPQ bytes (", t.totalBytes, " listed)");
            }
        }
    }
    updateCheckpoint = "state switch";
    switch (state) {
        case AppState::FIRST_RUN:
            // Drawn, not driven: the extraction runs on its own thread and
            // the screen reads its progress while laying itself out.
            updateCheckpoint = "first_run: enter";
            break;
        case AppState::DATA_FOLDER:
            updateCheckpoint = "data_folder: enter";
            pollStreamStart();
            break;

        case AppState::AUTHENTICATION:
            updateCheckpoint = "auth: enter";
            if (authHandler) {
                authHandler->update(deltaTime);
            }
            break;

        case AppState::REALM_SELECTION:
            updateCheckpoint = "realm_selection: enter";
            if (authHandler) {
                authHandler->update(deltaTime);
            }
            break;

        case AppState::CHARACTER_CREATION:
            updateCheckpoint = "char_creation: enter";
            if (gameHandler) {
                gameHandler->update(deltaTime);
            }
            if (uiManager) {
                uiManager->getCharacterCreateScreen().update(deltaTime);
            }
            break;

        case AppState::CHARACTER_SELECTION:
            updateCheckpoint = "char_selection: enter";
            if (gameHandler) {
                gameHandler->update(deltaTime);
            }
            break;

        case AppState::IN_GAME:
            // Checked before the frame rather than after it: everything below
            // reads a world that is no longer being updated.
            if (gameHandler &&
                gameHandler->getState() == game::WorldState::DISCONNECTED) {
                handleWorldDisconnect();
                break;
            }
            updateInGame(deltaTime, updateCheckpoint);
            break;

        case AppState::DISCONNECTED:
            // Handle disconnection
            break;
    }

    // Process any pending world entry request via WorldLoader
    updateCheckpoint = "world entry";
    if (worldLoader_ && state != AppState::DISCONNECTED) {
        worldLoader_->processPendingEntry();
    }

    // A cinematic's camera, sounds and fades, ahead of the renderer that
    // shows its camera (game/cinematic.hpp).
    if (cinematicPlayer_ && state == AppState::IN_GAME) {
        cinematicPlayer_->update();
        if (uiManager) uiManager->setScreenFade(cinematicPlayer_->fadeAlpha());
    } else if (uiManager) {
        uiManager->setScreenFade(0.0f);
    }

    // Update renderer (camera, etc.) only when in-game
    updateCheckpoint = "renderer update";
    if (renderer && state == AppState::IN_GAME) {
        auto rendererUpdateStart = std::chrono::steady_clock::now();
        WOWEE_PROFILE_SCOPE("renderer->update", Cpu);
        try {
            renderer->update(deltaTime);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::update stage 'renderer->update': ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::update stage 'renderer->update': ", e.what());
            throw;
        }
        float ruMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - rendererUpdateStart).count();
        if (ruMs > 50.0f) {
            LOG_WARNING("SLOW update stage 'renderer->update': ", ruMs, "ms");
        }
    }
    {
        // WOWEE_TRANSPORT_POSE: on a transport, every tenth of a second, the
        // frame as it is drawn - the hull, the character and the camera, all
        // placed for this frame.
        static const bool kViewTrace = [] {
            const char* v = std::getenv("WOWEE_TRANSPORT_POSE");
            return v && v[0] && v[0] != '0';
        }();
        static auto lastViewLog = std::chrono::steady_clock::time_point{};
        const auto nowView = std::chrono::steady_clock::now();
        if (kViewTrace && renderer && gameHandler && gameHandler->isOnTransport() &&
            nowView - lastViewLog >= std::chrono::milliseconds(100)) {
            lastViewLog = nowView;
            auto* tm = gameHandler->getTransportManager();
            const auto* tr = tm ? tm->getTransport(gameHandler->getPlayerTransportGuid()) : nullptr;
            const glm::vec3 hull = tr ? core::coords::canonicalToRender(tr->position) : glm::vec3(0.0f);
            const glm::vec3 ch = renderer->getCharacterPosition();
            const auto* cam = renderer->getCamera();
            const glm::vec3 cp = cam ? cam->getPosition() : glm::vec3(0.0f);
            const glm::vec3 cf = cam ? cam->getForward() : glm::vec3(0.0f);
            LOG_WARNING("RIDE VIEW: hull=(", hull.x, ", ", hull.y, ", ", hull.z, ") yaw=", tr ? tr->facing : 0.0f,
                        " char=(", ch.x, ", ", ch.y, ", ", ch.z, ") cam=(", cp.x, ", ", cp.y, ", ", cp.z,
                        ") fwd=(", cf.x, ", ", cf.y, ", ", cf.z, ")");
        }
    }
    // Update UI
    updateCheckpoint = "ui update";
    if (uiManager) {
        WOWEE_PROFILE_SCOPE("uiManager->update", Cpu);
        try {
            uiManager->update(deltaTime);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::update stage 'uiManager->update': ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::update stage 'uiManager->update': ", e.what());
            throw;
        }
    }
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("OOM in Application::update checkpoint '", updateCheckpoint, "': ", e.what());
        throw;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception in Application::update checkpoint '", updateCheckpoint, "': ", e.what());
        throw;
    }
}

void Application::beatWatchdog() {
    watchdogHeartbeatMs_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_release);
}

void Application::render() {
#ifdef WOWEE_VR
    // However this returns, the headset's frame is ended - a begun frame
    // left open stops the compositor taking the next.
    struct VrFrameEnd {
        ~VrFrameEnd() { vr::Runtime::get().endFrame(); }
    } vrFrameEnd;
#endif
    // Nothing to draw to between the app leaving the foreground and its
    // surface being rebuilt, and nothing is drawn in the background at all.
    // syncSurface is also where a surface the driver reported lost, or one
    // whose native window was not there yet on resume, is rebuilt - so this is
    // asked every frame rather than only between the lifecycle events.
    //
    // update() has already opened an ImGui frame by this point, and ImGui wants
    // every frame closed before the next is opened - returning without doing so
    // aborted on the following NewFrame rather than on anything to do with the
    // surface. EndFrame closes it without drawing it.
    if (rendering::VkContext* vk = window ? window->getVkContext() : nullptr;
        vk && !vk->syncSurface()) {
        if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->WithinFrameScope) {
            ImGui::EndFrame();
        }
        // Said once a second, so the log shows a client that is waiting
        // rather than one that has stopped.
        static auto lastSaid = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSaid > std::chrono::seconds(1)) {
            lastSaid = now;
            LOG_INFO("Not drawing: ", vk->isDeviceLost()   ? "device lost"
                                      : vk->inBackground() ? "in the background"
                                                           : "no surface yet");
        }
        // The loop still runs, to keep reading the socket. It does not need to
        // run at frame rate to do that, and a phone in someone's pocket should
        // not be spending a core on it. A lost device stops here only for the
        // frame it was lost in; the loop then runs runWhileDeviceLost instead
        // until it is rebuilt.
        SDL_Delay(50);
        return;
    }

    if (!renderer) {
        return;
    }

    // A difficulty change's loading screen is all there is to see while it
    // is up (0x0040ab70); it has no world to stay over once the game is left.
    if (difficultyLoadingScreen_ && state != AppState::IN_GAME) difficultyLoadingScreen_.reset();
    if (difficultyLoadingScreen_) {
        difficultyLoadingScreen_->render();
        return;
    }

    // Mirrors the IN_GAME update stages: a frame that blocks long enough to trip the
    // watchdog needs to say which phase did it.
    auto runRenderStage = [this](const char* stageName, auto&& fn) {
        auto stageStart = std::chrono::steady_clock::now();
        const core::ProfileScope profileScope(stageName, core::ProfileKind::Cpu);
        const stall_report::StageScope stallStage(stageName);
        try {
            fn();
        } catch (const std::bad_alloc&) {
            LOG_ERROR("Out of memory in render stage '", stageName, "'");
            throw;
        }
        float stageMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        noteStageTime(stageName, stageMs);
        if (stageMs > 50.0f) {
            LOG_WARNING("SLOW render stage '", stageName, "': ", stageMs, "ms");
        }
    };

#ifdef WOWEE_MOBILE
    // Drawn into the background list, under everything the interface puts on
    // screen, and only while a thumb is holding it.
    ui::touchControls().setInWorld(state == AppState::IN_GAME);
    ui::touchControls().draw();
#endif

#ifdef WOWEE_VR
    // In the world, the frame below is the left eye's and the right eye's
    // follows it; in the menus it is drawn only for the interface.
    vr::Runtime& vrRuntime = vr::Runtime::get();
    rendering::Camera* vrCamera = renderer->getCamera();
    const int vrEyes = vrCamera ? vrRuntime.eyesToRender(state == AppState::IN_GAME) : 0;
    vrRuntime.setRecordingEye(vrEyes > 0 ? 0 : -1);
    if (vrEyes > 0) vrRuntime.applyEyeCamera(0, *vrCamera);
#endif

    renderingFrame_ = true;
    runRenderStage("beginFrame", [&] { renderer->beginFrame(); });

    // Only render 3D world when in-game
    if (state == AppState::IN_GAME) {
        // The names laid out last frame, moved with their units to where
        // they are drawn now (see WorldTextRenderer::followUnits).
        if (auto* worldText = renderer->getWorldTextRenderer()) {
            worldText->followUnits([this](uint64_t guid, glm::vec3& at) { return getRenderPositionForGuid(guid, at); });
        }
        runRenderStage("renderWorld", [&] {
            renderer->renderWorld(world ? world.get() : nullptr, gameHandler.get());
        });
    }

    // Render performance HUD (within ImGui frame, before UI ends the frame)
    if (renderer) {
        runRenderStage("renderHUD", [&] { renderer->renderHUD(); });
    }

    // Addon-built frames, drawn under the client's own interface while the two
    // coexist. This is the whole point of the widget tree: an addon that calls
    // CreateTexture now puts something on the screen instead of talking to a
    // table of no-ops.
    //
    // FrameXML's frames come through the same tree, and they are the interface
    // now rather than a second one drawn over this client's own. This used to
    // be gated on WOWEE_FRAMEXML_UI naming at least one element, so that a run
    // loading FrameXML only to exercise the parser kept the client's own
    // interface; there is no longer another interface for it to keep.
    //
    // Only while there is a world to draw it over. That is the whole of the
    // gate now, and it is the half that was load-bearing: without it every
    // frame FrameXML had built stayed on screen through a logout and sat on top
    // of character select - /logout ran, the character left the world, and the
    // interface it had been using never went away.
    const bool drawWidgets = state == AppState::IN_GAME;
    if (drawWidgets && addonManager_ && addonManager_->getLuaEngine() && renderer) {
        runRenderStage("addonWidgets", [&] {
            const ImGuiIO& io = ImGui::GetIO();
            auto* engine = addonManager_->getLuaEngine();

            // The portrait is the character itself rendered small, so it is
            // produced here rather than read from a file, and handed to the
            // widget by name each frame. Told every frame rather than once,
            // because the render target is rebuilt when the window resizes and
            // a handle kept across that would be stale.
            runRenderStage("addonWidgets/models", [&] {
            if (gameHandler && assetManager) {
                auto& widgets = engine->widgets();
                ui::Widget* portrait = portraitWidgetId_
                    ? widgets.get(portraitWidgetId_) : nullptr;
                if (!portrait || portrait->name != "PlayerPortrait") {
                    portrait = widgets.findByName("PlayerPortrait");
                    portraitWidgetId_ = portrait ? portrait->id : 0;
                }
                // Only while something is going to show it. This is a whole
                // offscreen character pass, and running it for a frame that
                // FrameXML has not been given costs a model render and a
                // composite every frame to produce a picture nothing draws.
                // The minimap is a Vulkan pass of its own drawn inside the
                // main render pass, not an image this renderer could place, so
                // the map is told where FrameXML's frame ended up rather than
                // the frame being handed a picture. One frame behind, because
                // that pass has already run by the time this lays out - which
                // for a frame that does not move is not visible.
                // The world map is an ImGui window that centres itself, so
                // like the minimap it is told where to be rather than handed
                // a picture. The detail frame, not the outer one: that is the
                // map area inside the panel FrameXML drew.
                if (auto* wmap = renderer->getWorldMap()) {
                    // Where the map has come to be on the player's zone as it
                    // was drawn, it says so and the dropdowns are rebuilt.
                    if (wmap->takeViewChanged() && gameHandler) {
                        gameHandler->fireAddonEvent("WORLD_MAP_UPDATE", {});
                    }
                    ui::Widget* wm = worldMapWidgetId_
                        ? widgets.get(worldMapWidgetId_) : nullptr;
                    if (!wm || wm->name != "WorldMapDetailFrame") {
                        wm = widgets.findByName("WorldMapDetailFrame");
                        worldMapWidgetId_ = wm ? wm->id : 0;
                    }
                    if (wm && wm->visible && wm->rectW > 0.0f && wm->rectH > 0.0f) {
                        const float sc = widgets.uiScale();
                        wmap->setFrameRect(wm->left * sc,
                                           io.DisplaySize.y - (wm->bottom + wm->rectH) * sc,
                                           wm->rectW * sc, wm->rectH * sc);
                    } else {
                        wmap->clearFrameRect();
                    }
                }

                if (auto* map = renderer->getMinimap()) {
                    ui::Widget* mm = minimapWidgetId_
                        ? widgets.get(minimapWidgetId_) : nullptr;
                    if (!mm || mm->name != "Minimap") {
                        mm = widgets.findByName("Minimap");
                        minimapWidgetId_ = mm ? mm->id : 0;
                    }
                    // The interface owns the map: with its frame hidden (the
                    // interface toggled off, UIParent:Hide()) nothing of the
                    // map is drawn - not the bare disc in the corner.
                    map->setFrameHidden(mm && !mm->visible);
                    map->setFrameAlpha(mm ? widgets.effectiveAlpha(*mm) : 1.0f);
                    if (mm && mm->visible && mm->rectW > 0.0f && mm->rectH > 0.0f) {
                        const float sc = widgets.uiScale();
                        map->setScreenRect(mm->left * sc,
                                           io.DisplaySize.y - (mm->bottom + mm->rectH) * sc,
                                           mm->rectW * sc, mm->rectH * sc);

                        // The zoom buttons move a number on the frame, and the
                        // map has to be told: Minimap:SetZoom is the interface
                        // saying what it wants, not the map changing. The
                        // client's six levels, zero furthest out (0x007f3b90).
                        //
                        // Except as the player goes in or out: the client keeps
                        // a level for each (minimapZoom, minimapInsideZoom;
                        // 0x007f3b40 answers GetZoom with the one in use) and
                        // fires MINIMAP_UPDATE_ZOOM when the map changes over
                        // (0x007f5ba0), so the frame takes the map's level then.
                        if (map->isIndoors() != minimapWasIndoors_) {
                            minimapWasIndoors_ = map->isIndoors();
                            if (minimapWasIndoors_ && mm->savedInsideZoom >= 0) {
                                map->setZoomLevel(mm->savedInsideZoom);
                                mm->savedInsideZoom = -1;
                            }
                            mm->zoomLevel = map->getZoomLevel();
                            mm->zoomIndoors = minimapWasIndoors_;
                            if (gameHandler) gameHandler->fireAddonEvent("MINIMAP_UPDATE_ZOOM", {});
                        }
                        if (mm->zoomLevel != map->getZoomLevel()) map->setZoomLevel(mm->zoomLevel);

                        // Minimap:PingLocation, which is all Minimap_OnClick
                        // does. The offsets are minimap-local interface units
                        // from the centre with +y up; the projection works in
                        // pixels with +y down, and the ratio of the two radii
                        // is what carries the scale, so both being interface
                        // units is enough.
                        if (mm->pingRequested) {
                            mm->pingRequested = false;
                            ui::MinimapView view;
                            view.viewRadius = map->getViewRadius();
                            view.mapRadius = mm->rectW * 0.5f;
                            if (map->isRotateWithCamera()) {
                                if (auto* cam = renderer->getCamera()) {
                                    const glm::vec3 fwd = cam->getForward();
                                    const float bearing = std::atan2(fwd.y, -fwd.x);
                                    view.cosBearing = std::cos(bearing);
                                    view.sinBearing = std::sin(bearing);
                                }
                            }
                            glm::vec3 playerRender = renderer->getCharacterInstanceId() != 0
                                ? renderer->getCharacterPosition()
                                : (renderer->getCamera() ? renderer->getCamera()->getPosition()
                                                         : glm::vec3(0.0f));
                            const glm::vec2 d = ui::minimapOffsetToRenderDelta(
                                mm->pingX, -mm->pingY, view);
                            const glm::vec3 canon = core::coords::renderToCanonical(
                                playerRender + glm::vec3(d.x, d.y, 0.0f));
                            if (gameHandler) gameHandler->sendMinimapPing(canon.x, canon.y);
                        }
                    } else {
                        map->clearScreenRect();
                    }
                }

                // How big to build a model view, from the frame it will be
                // drawn into.
                //
                // The aspect is the point. Every one of these was built at
                // 640x800 and stretched to fill a frame with a different shape:
                // the paperdoll's is 233x215, so the character was drawn a
                // third too wide, and the companion preview's is 288x160, so a
                // mount was more than twice too wide. Only the inspect frame
                // happened to match.
                //
                // In pixels, so a larger interface scale gets a larger image
                // rather than a blurrier one, and bounded either way: nothing
                // smaller than is worth compositing, nothing larger than the
                // paperdoll ever needed.
                auto sizeFor = [&widgets](ui::UnitPortrait& view, const ui::Widget* frame) {
                    if (!frame || frame->rectW <= 0.0f || frame->rectH <= 0.0f) return;
                    const float scale = widgets.uiScale();
                    const auto clampDim = [](float v) {
                        return static_cast<int>(std::clamp(v, 64.0f, 800.0f));
                    };
                    view.setTargetSize(clampDim(frame->rectW * scale),
                                       clampDim(frame->rectH * scale));
                };

            runRenderStage("addonWidgets/modelFiles", [&] {
                // Model frames given a model file (CSimpleModel, 0x0095fc30):
                // each its own off-screen view of the frame's size, posed and
                // projected as the client does in DDC, the screen with a
                // diagonal of 1 (0x0047bf90) - a frame's rect, SetPosition and
                // model scale all measured in it.
                const float screenW = io.DisplaySize.x, screenH = io.DisplaySize.y;
                const float uiScale = widgets.uiScale();
                std::unordered_set<uint32_t> shown;
                std::vector<uint32_t> modelsUpdated;
                if (screenW > 0.0f && screenH > 0.0f && uiScale > 0.0f) {
                    const float ddcH = rendering::model_frame::ddcHeight(screenW / screenH);
                    const float ddcPerUnit = ddcH / (screenH / uiScale);
                    for (const uint32_t id : widgets.modelFileFrames()) {
                        ui::Widget* frame = widgets.get(id);
                        if (!frame || !frame->visible || frame->modelFile.empty()) continue;
                        // One held in the middle of a frame without a size is
                        // drawn about that point.
                        constexpr float kPointPad = 32.0f;
                        frame->modelPad =
                            frame->modelCentered && (frame->rectW <= 0.0f || frame->rectH <= 0.0f) ? kPointPad : 0.0f;
                        const float viewW = frame->rectW + 2.0f * frame->modelPad;
                        const float viewH = frame->rectH + 2.0f * frame->modelPad;
                        if (viewW <= 0.0f || viewH <= 0.0f) continue;
                        auto& view = modelFrameViews_[id];
                        if (!view) view = std::make_unique<rendering::ModelFrameView>();
                        const int pw = static_cast<int>(viewW * uiScale + 0.5f);
                        const int ph = static_cast<int>(viewH * uiScale + 0.5f);
                        if (!view->ensureSize(assetManager.get(), pw, ph) || !view->setModel(frame->modelFile, frame->modelIconTexture)) {
                            frame->externalTexture = 0;
                            // Kept while its renderer is made beside the frames
                            // (or its model will not load): let go, it was made
                            // anew each frame and never got there (T360).
                            shown.insert(id);
                            continue;
                        }
                        rendering::model_frame::Pose pose;
                        // Held at the frame's middle, in its own units: half its
                        // width and height (0x005446f0).
                        if (frame->modelCentered) {
                            const float eff = frame->effScale > 0.0f ? widgets.trueScale(*frame) : 1.0f;
                            frame->modelPosition[0] = viewW * ddcPerUnit / eff * 0.5f;
                            frame->modelPosition[1] = viewH * ddcPerUnit / eff * 0.5f;
                            frame->modelPosition[2] = 0.0f;
                        }
                        pose.position = glm::vec3(frame->modelPosition[0], frame->modelPosition[1],
                                                  frame->modelPosition[2]);
                        pose.facing = frame->modelFacing;
                        pose.modelScale = frame->modelScale;
                        pose.effectiveScale = widgets.trueScale(*frame);
                        // A PlayerModel's camera stays where the model came.
                        std::optional<rendering::model_frame::Pose> came;
                        if (frame->isPlayerModel() && frame->modelCamera < 0) {
                            came = pose;
                            came->position = glm::vec3(frame->modelCameAt[0], frame->modelCameAt[1], frame->modelCameAt[2]);
                            came->facing = 0.0f;
                            came->modelScale = frame->modelCameScale;
                        }
                        view->setFrame(viewW * ddcPerUnit, viewH * ddcPerUnit, pose, ddcH,
                                       frame->modelCamera, frame->modelSequence, frame->modelSequenceTime,
                                       frame->modelSequenceSerial, came ? &*came : nullptr);
                        view->setLight(rendering::model_frame::lightOf(*frame));
                        view->setFog(rendering::model_frame::fogOf(*frame));
                        view->update(io.DeltaTime);
                        view->requestComposite();
                        modelsUpdated.push_back(id);
                        frame->externalTexture = view->textureId();
                        frame->externalPremultiplied = true;
                        shown.insert(id);
                    }
                }
                // The interface's own player arrow on the world map: the map shows
                // none of its own - where that is hidden, on another floor, neither.
                if (auto* wm = renderer ? renderer->getWorldMap() : nullptr) {
                    const ui::Widget* arrow = widgets.findByName("PlayerArrowFrame");
                    wm->setInterfaceDrawsPlayerArrow(arrow && !(arrow->visible && arrow->externalTexture == 0));
                }
                // OnUpdateModel (0x0095fba0), once all are placed: a script may make frames.
                for (const uint32_t id : modelsUpdated) engine->runFrameScript(id, "OnUpdateModel");
                // A frame hidden, gone or cleared lets its view go.
                for (auto it = modelFrameViews_.begin(); it != modelFrameViews_.end();) {
                    if (shown.count(it->first)) { ++it; continue; }
                    if (ui::Widget* frame = widgets.get(it->first)) {
                        frame->externalTexture = 0;
                        frame->externalPremultiplied = false;
                    }
                    it = modelFrameViews_.erase(it);
                }
            });

            runRenderStage("addonWidgets/paperdoll", [&] {
                drawUnitModels(widgets, io.DisplaySize.x, io.DisplaySize.y, io.DeltaTime);
            });
            runRenderStage("addonWidgets/portrait", [&] {
                // A portrait is a still picture, drawn once (0x00619580),
                // unless the setting asks for live ones.
                const bool animatedPortraits =
                    uiManager && uiManager->getGameScreen().getSettingsPanel().animatedPortraits_;
                unitPortrait_.setAnimated(animatedPortraits);
                if (portrait) {
                    sizeFor(unitPortrait_, portrait);
                    const bool drawn = unitPortrait_.show(unitLook(gameHandler->getPlayerGuid()), assetManager.get(),
                                                          renderer.get(), io.DeltaTime);
                    // Assigned every frame including when it is zero. Keeping
                    // the last good handle instead would leave the widget
                    // pointing at a descriptor set that has been destroyed -
                    // the render target is torn down whenever the model is
                    // rebuilt - and drawing from that is a use-after-free
                    // rather than a stale picture.
                    portrait->externalTexture = drawn ? unitPortrait_.textureId() : 0;
                }

                // Every texture SetPortraitTexture was told to fill with the
                // player. Assigned here rather than when it was asked for,
                // because the handle is rebuilt whenever the portrait's render
                // target is and one kept across that is stale. The list is
                // short - five frames name the player in all of FrameXML - so
                // this costs a handful of lookups rather than a scan.
                const uint64_t playerFace = unitPortrait_.textureId();
                for (uint32_t id : widgets.playerPortraits()) {
                    if (ui::Widget* w = widgets.get(id)) {
                        w->externalTexture = playerFace;
                    }
                }

                // The other faces, on the same terms as the player's: one to
                // each unit a texture was given, and only while one of its
                // textures is on screen. Each is a whole offscreen model pass.
                //
                // A player from their appearance and anything else from its
                // display id. Both are read from what the world already has:
                // a creature is a model path, and a player's skin, face and
                // hair come off the update fields their spawn was built from.
                std::vector<std::string> faces;
                for (auto& claim : widgets.portraitClaims()) {
                    if (!ui::portrait_claims::unitOf(claim)) continue;
                    const auto& ids = widgets.portraitsFor(claim);
                    if (std::any_of(ids.begin(), ids.end(), [&widgets](uint32_t id) {
                            const ui::Widget* w = widgets.get(id);
                            return w && w->visible;
                        })) {
                        faces.push_back(std::move(claim));
                    } else {
                        // Its view may draw another by the time it is back.
                        for (uint32_t id : ids)
                            if (ui::Widget* w = widgets.get(id)) w->externalTexture = 0;
                    }
                }
                facePool_.assign(faces);
                faceViews_.resize(facePool_.views());
                facePictures_.resize(facePool_.views());
                for (const std::string& claim : faces) {
                    const auto guid = ui::portrait_claims::unitOf(claim);
                    const auto view = facePool_.viewOf(claim);
                    if (!guid || !view) continue;
                    if (!faceViews_[*view]) faceViews_[*view] = std::make_unique<ui::UnitPortrait>();
                    struct UnitFace {
                        ui::UnitPortrait* portrait;
                        uint64_t guid;
                    };
                    const UnitFace face{faceViews_[*view].get(), *guid};
                    const auto& claimed = widgets.portraitsFor(claim);
                    const bool there = gameHandler->getEntityManager().getEntity(face.guid) != nullptr;

                    // Draws it, and answers whether the picture is that unit's.
                    const auto draw = [&]() -> bool {
                        face.portrait->setFraming(ui::UnitPortrait::Framing::Face);
                        face.portrait->setAnimated(animatedPortraits);
                        // Sized for the circle it is drawn into rather than for the
                        // paperdoll, and no smaller than another circle the view
                        // may draw later wants: the client's are 64 (0x00619580).
                        face.portrait->setTargetSize(128, 128);
                        sizeFor(*face.portrait, widgets.get(claimed.front()));
                        return face.portrait->show(unitLook(face.guid), assetManager.get(), renderer.get(),
                                                   io.DeltaTime);
                    };
                    // A unit gone out of sight keeps the picture taken of it.
                    if (there) facePictures_[*view] = draw() ? claim : std::string();
                    const uint64_t drawn = facePictures_[*view] == claim ? face.portrait->textureId() : 0;
                    for (uint32_t id : claimed) {
                        if (ui::Widget* w = widgets.get(id)) w->externalTexture = drawn;
                    }
                }
            });
            }

            // Lay out first: hit testing reads the rects this produces, so
            // clicking a frame that moved this frame would otherwise use where
            // it used to be.
            //
            // Only the layout. The drawing waits until after the UI stage,
            // which is what puts the nameplates and the minimap's blips into
            // the same ImGui background list - and in a draw list the last
            // thing added is the thing on top. Drawing here put the panels
            // down first, so every player's name and health bar in the world
            // showed through the bags and the auction house.
            });
            runRenderStage("addonWidgets/layout", [&] {
            // The display's safe area, in the window units DisplaySize is
            // in (UIManager, from SDL). The whole window everywhere but a
            // phone with a notch.
            {
                const ui::safe_area::Insets& safe = ui::safe_area::current();
                engine->widgets().setSafeArea(safe.left, safe.right, safe.top, safe.bottom);
            }
            widgetRenderer_.layout(engine->widgets(), io.DisplaySize.x, io.DisplaySize.y);
            });

            // The client's own interface has first claim, but only over the
            // point the cursor is actually on.
            //
            // WantCaptureMouse is the wrong test: it is also true whenever any
            // ImGui item is active anywhere, and this client keeps a chat input
            // on screen. A focused input would hold it true for as long as it
            // held focus, and no addon frame would ever see the mouse no matter
            // where the cursor was. Asking whether a window is under the cursor
            // is the question that was meant.
            //
            // First claim, though, is claimed on the press. Once a frame has
            // taken one the interface keeps the mouse until it comes up again,
            // wherever the cursor goes in the meantime. A drag crosses the
            // screen by definition - spellbook to action bar passes over
            // whatever else is on the way - and dispatchMouse is the only thing
            // that advances the press state, so cutting it off part way does
            // not pause the drag, it strands it: OnDragStop and OnReceiveDrag
            // live on the release path and the release is never seen. That is
            // the whole of "cannot drag a spell to the action bar"; every link
            // in the chain below this one was already correct.
            const bool overClientUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                                      !engine->holdsMousePress();

            // The keys that open what FrameXML now owns.
            //
            // Each of this client's panels polls its own keybinding from
            // inside its own draw, so a panel that is no longer drawn never
            // sees the key - handing one over made it unopenable. The key has
            // to reach the replacement instead, which is FrameXML's own toggle
            // for that panel.
            runRenderStage("addonWidgets/keys", [&] {
            {
                using ui::UiElement;
                using K = ui::KeybindingManager;
                struct Route { UiElement element; K::Action action; const char* call; };
                // NOLINTBEGIN(modernize-use-designated-initializers) - a table
                // whose columns are its field names, with the struct in view
                // directly above.
                static const Route kRoutes[] = {
                    // Names checked against this FrameXML rather than assumed:
                    // ToggleAllBags, ToggleQuestLog and ToggleWorldMap are all
                    // later additions and do not exist in 3.3.5, so calling
                    // them did nothing at all - the fallback answers an
                    // unknown name and calling it yields nothing.
                    {UiElement::Bags,           K::Action::TOGGLE_BAGS,             "ToggleBackpack()"},
                    {UiElement::Spellbook,      K::Action::TOGGLE_SPELLBOOK,        "ToggleSpellBook(BOOKTYPE_SPELL)"},
                    {UiElement::QuestLog,       K::Action::TOGGLE_QUESTS,           "ToggleFrame(QuestLogFrame)"},
                    {UiElement::CharacterFrame, K::Action::TOGGLE_CHARACTER_SCREEN, "ToggleCharacter(\"PaperDollFrame\")"},
                    {UiElement::WorldMap,       K::Action::TOGGLE_WORLD_MAP,        "ToggleFrame(WorldMapFrame)"},
                    // Three more panels that poll their own key from inside
                    // their own draw, found by asking which of this client's
                    // windows do that and which of those are gated. Every one
                    // that is gated and not listed here is a key that stops
                    // working the moment its element is handed over - the
                    // talent frame on N, the guild roster and the dungeon
                    // finder on theirs.
                    //
                    // Names checked against this FrameXML, as the note above
                    // says to: ToggleTalentFrame is in uiparent.lua,
                    // ToggleFriendsFrame takes a tab number and three is the
                    // guild one, and the dungeon finder is ToggleLFDParentFrame
                    // - the binding is still called TOGGLELFGPARENT, which is
                    // the old name kept so nobody's key was reset.
                    {UiElement::Talents,        K::Action::TOGGLE_TALENTS,          "ToggleTalentFrame()"},
                    // ToggleFriendsFrame with no tab, which is what this key
                    // is: bindings.xml gives TOGGLESOCIAL - O by default, and
                    // the action here is O - the body ToggleFriendsFrame(),
                    // while ToggleFriendsFrame(3) belongs to TOGGLEGUILDTAB on
                    // a key of its own.
                    //
                    // The difference is not cosmetic. The tab form has two
                    // paths that do not close: it returns without doing
                    // anything at all when the player is in no guild, and when
                    // the selected tab is not the one asked for it re-shows on
                    // that tab rather than hiding. The plain form is what its
                    // name says - shown becomes hidden, hidden becomes shown -
                    // so the key stopped closing the window it had opened.
                    {UiElement::Social,         K::Action::TOGGLE_GUILD_ROSTER,     "ToggleFriendsFrame()"},
                    {UiElement::DungeonFinder,  K::Action::TOGGLE_DUNGEON_FINDER,   "ToggleLFDParentFrame()"},
                };
                // NOLINTEND(modernize-use-designated-initializers)
                for (const Route& r : kRoutes) {
                    if (!ui::frameXmlOwns(r.element)) continue;
                    if (!K::getInstance().isActionPressed(r.action)) continue;
                    engine->executeString(r.call);
                }
            }

            // After layout, because the range follows from the rects it just
            // resolved, and before the mouse, so a scroll bar enabled by this
            // frame's range can be clicked in it.
            // Both after layout: what is on screen and how far a frame can
            // scroll are answers layout produces, not things the interface
            // announced. Visibility first, because a panel's OnShow is what
            // fills it in and the size of what it filled is what the range is
            // then measured from.
            });
            runRenderStage("addonWidgets/engine", [&] {
            engine->reportEventListenersOnce();
            // Keep this after the widget render, which is where suppressed
            // windows are forced hidden. A suppressed frame that some handler
            // showed earlier this iteration is already hidden again by the time
            // this looks, so it never counts as having appeared and its
            // OnShow/OnHide never run. Several of those handlers do real work:
            // LootFrame_OnHide calls CloseLoot, which releases the loot on the
            // server - through the client's own loot window, the one actually
            // on screen. Moving this earlier would take the player's loot.
            engine->updateVisibility();
            // After visibility, because a frame shown this frame is measured
            // this frame and its size is new rather than changed.
            engine->updateSizeChanges();
            engine->updateScrollRanges();
            });

            // A click that never reaches the interface and a click whose
            // handler does nothing look the same from the chair. Said once a
            // second while a button is actually held, so it costs nothing and
            // appears exactly when someone is wondering why nothing happened.
            if (overClientUi && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                static double lastSaid = 0.0;
                static core::LogBudget heldBudget(
                    5, "WidgetInput: click held over this client's own window");
                const double now = ImGui::GetTime();
                if (now - lastSaid > 1.0 && heldBudget.take()) {
                    lastSaid = now;
                    LOG_WARNING("WidgetInput: click held over this client's own "
                                "window, so it was not passed to the interface");
                }
            }

            if (overClientUi) {
                // Nothing below is going to run, so say so rather than leaving
                // the tree believing the cursor is still where it last saw it.
                engine->releaseMouseHover();
            } else {
                addons::LuaEngine::MouseButtons buttons;
                buttons.left   = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                buttons.right  = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                buttons.middle = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                // ImGui's SDL3 backend numbers the side buttons 3 and 4.
                buttons.button4 = ImGui::IsMouseDown(3);
                buttons.button5 = ImGui::IsMouseDown(4);
                engine->dispatchMouse(io.MousePos.x, io.MousePos.y,
                                      io.DisplaySize.y, buttons);
            }
        });
    }

    // Render UI on top (ends ImGui frame with ImGui::Render())
    if (uiManager) {
        runRenderStage("uiManager->render", [&] {
            // Not wrapped in an upload batch, though the temptation is
            // strong: nine screens here upload icons one texture at a time,
            // each submitting and waiting on the shared immediate fence, and
            // batching them took this stage from 682ms to under 200.
            //
            // It also broke correctness, which is why it is not done. This
            // stage does not only build draw lists - the unit portrait and the
            // character preview render models to offscreen targets inside it,
            // and those are real draws. With the batch still open they sampled
            // images whose uploads had not been submitted, and validation
            // began reporting VK_IMAGE_LAYOUT_UNDEFINED one second into every
            // run rather than only at the moment the device was lost.
            //
            // The stall is real and worth fixing, but it has to be fixed where
            // the uploads are - around each screen's icon loop, inside which
            // nothing draws - not around a stage that draws.
            uiManager->render(state, authHandler.get(), gameHandler.get());
        });

        // The interface, over the world and everything the world drew on top
        // of itself. Laid out much earlier, because the frame's clicks are
        // resolved against those rects; drawn here, because the stage above is
        // what adds the nameplates and the minimap blips to the background
        // draw list and the last thing added to one is the thing on top.
        //
        // The same guard the layout runs under, so the two never disagree
        // about whether there is an interface to draw.
        if (drawWidgets && addonManager_ && addonManager_->getLuaEngine() && renderer) {
            runRenderStage("frameXml->draw", [&] {
                const ImGuiIO& uiIo = ImGui::GetIO();
                widgetRenderer_.draw(addonManager_->getLuaEngine()->widgets(),
                                     uiIo.DisplaySize.x, uiIo.DisplaySize.y);
            });
        }

        // Only now is the draw data closed.
        uiManager->finishImGuiFrame();

        runRenderStage("mapWindow", [&] { updateMapWindow(); });
    }

    // The world, put into the frame only now. renderWorld left the doodads
    // recording on a worker, and everything above - the HUD, the addon
    // widgets, the game screen and FrameXML - is CPU work on draw lists and
    // offscreen views that neither records into this frame's command buffer
    // nor reads what the world recorded, so it runs while that worker does.
    // What in it would change the model or character renderers reaches them
    // through Renderer's getters, which wait for the worker first.
    runRenderStage("finishWorld", [&] { renderer->finishRenderWorld(); });
    runRenderStage("endFrame", [&] { renderer->endFrame(); });

#ifdef WOWEE_VR
    // Where the pointer is, for the next frame's clicks: on one of the
    // client's own windows or a FrameXML frame, or on nothing of the
    // interface - which makes a click a click into the world.
    {
        const bool overFrameXml = drawWidgets && addonManager_ && addonManager_->getLuaEngine() &&
                                  addonManager_->getLuaEngine()->mouseOverFrameXml();
        // FrameXML's own lines, drawn wider for a controller: a press has to
        // travel 16 interface units to be a drag, and a second click may come
        // 0.55 s after the first.
        if (addonManager_ && addonManager_->getLuaEngine()) {
            addonManager_->getLuaEngine()->setPointerTolerance(16.0f, 0.55);
        }
        vrRuntime.setPointerOverInterface(ImGui::GetIO().WantCaptureMouse || overFrameXml);
    }
    // A click into the world: what the desktop's left click (target) and
    // right click (target and interact) do, along the controller's ray.
    if (auto click = vrRuntime.takeWorldClick(); click && gameHandler && uiManager && state == AppState::IN_GAME) {
        const rendering::Ray ray{.origin = click->origin, .direction = click->direction};
        const ui::ScenePick pick = ui::pickScene(*gameHandler, ray, ui::ScenePickParams{});
        const uint64_t guid = pick.resolve();
        auto* engine = addonManager_ ? addonManager_->getLuaEngine() : nullptr;
        // What is held goes first, as on the desktop.
        const bool taken = engine && (click->interact ? engine->rightPressPutsCursorDown()
                                                      : engine->worldLeftClickTakenByCursor(guid != 0));
        if (!taken && guid != 0) {
            gameHandler->setTarget(guid);
            if (click->interact) uiManager->getGameScreen().interactWithTarget(*gameHandler);
        }
    }
    if (vrEyes > 0) {
        vrRuntime.restoreCamera(*vrCamera);
        // The right eye: the same world from its place, without the
        // interface, which went onto its own quad with the left.
        vrRuntime.setRecordingEye(1);
        vrRuntime.applyEyeCamera(1, *vrCamera);
        runRenderStage("vr: right eye", [&] {
            renderer->beginFrame();
            renderer->renderWorld(world ? world.get() : nullptr, gameHandler.get());
            renderer->finishRenderWorld();
            renderer->endFrame();
        });
        vrRuntime.restoreCamera(*vrCamera);
    }
#endif

    // A picture of the client, written once and then done with.
    //
    // What a screen looks like cannot be checked by reading the code that draws
    // it - the login card in particular is measured before it is drawn, and a
    // row counted in one place and not the other comes out as a sheet taller
    // than what is on it, which nothing but looking will catch. The in-game
    // screenshot key cannot reach the login screen, which is where that card is.
    if (!testFrameShotPath.empty()) {
        renderer->captureScreenshot(testFrameShotPath);
        testFrameShotPath.clear();
    }
#ifndef WOWEE_MOBILE
    if (mcp_) mcp_->afterFrame(*renderer);
#endif
    if (const char* shot = std::getenv("WOWEE_SCREENSHOT"); shot != nullptr) {
        // Let the interface settle first: ImGui sizes much of its layout from
        // what it measured last frame, and hides a window outright for its
        // first frame or two while it fits itself. WOWEE_SCREENSHOT_FRAME
        // takes it later, for a screen reached after a while (the login after
        // a streaming start from the data screen).
        int frame = kScreenshotFrame;
        if (const char* at = std::getenv("WOWEE_SCREENSHOT_FRAME"); at != nullptr && std::atoi(at) > 0) {
            frame = std::atoi(at);
        }
        if (++screenshotFrames_ == frame) {
            renderer->captureScreenshot(shot);
            running = false;
        }
    }

    // A line of Lua run in the world without anyone at the keys, for checking
    // what only a cast or a click shows: WOWEE_AUTO_LUA=<code>, run
    // WOWEE_AUTO_LUA_DELAY seconds (ten by default) after entering the world
    // and again every WOWEE_AUTO_LUA_REPEAT seconds if that is set. With
    // WOWEE_AUTO_LOGIN=2 a run needs no one there at all. __WoweeWarn(text)
    // puts a line in the log, where print() puts it in the chat.
    if (const char* autoLua = std::getenv("WOWEE_AUTO_LUA"); autoLua != nullptr && *autoLua != '\0') {
        static double inWorldSince = -1.0;
        static double nextRun = -1.0;
        // Which world entry of this process the run is in, for a run that
        // logs out and back in: __WoweeTestSession, set before each run.
        static int session = 0;
        const bool inWorld = gameHandler && gameHandler->getState() == game::WorldState::IN_WORLD;
        const double now = core::appTimeSeconds();
        if (!inWorld) {
            inWorldSince = -1.0;
        } else if (inWorldSince < 0.0) {
            inWorldSince = now;
            ++session;
            const char* delay = std::getenv("WOWEE_AUTO_LUA_DELAY");
            nextRun = now + ((delay && *delay) ? std::atof(delay) : 10.0);
        } else if (nextRun >= 0.0 && now >= nextRun && addonManager_ && addonManager_->getLuaEngine()) {
            LOG_WARNING("WOWEE_AUTO_LUA: running ", autoLua);
            addonManager_->getLuaEngine()->executeString("__WoweeTestSession=" + std::to_string(session));
            // Under /run's taint (0x00510b30), not none. The line is typed by
            // someone, not shipped by Blizzard: run with no taint it was the
            // interface's own code, so whatever it reached through an addon's
            // function - CloseAllBags is WoweeAllBags's - took that addon's
            // taint into the script's own state, and a protected call it made
            // seconds later was ADDON_ACTION_FORBIDDEN against WoweeAllBags. A
            // closure /run makes holds its taint while it runs (0x00857ca0), so
            // the refusal now names the script: MACRO_ACTION_FORBIDDEN, as a
            // typed /run InteractUnit("target") is. Protected actions are for
            // the keys (__WoweePressKey), whose bindings run secure.
            addonManager_->getLuaEngine()->runScriptAs(autoLua, addons::taint::macro());
            const char* repeat = std::getenv("WOWEE_AUTO_LUA_REPEAT");
            const double every = (repeat && *repeat) ? std::atof(repeat) : 0.0;
            nextRun = every > 0.0 ? now + every : -1.0;
        }
    }

    // Bursts of consecutive frames, for measuring what changes from one frame
    // to the next (the traced light's flicker, tools/rt_flicker.py):
    // WOWEE_BURST_DIR=<folder>, one burst per entry of WOWEE_BURST_VIEWS (the
    // RT debug view to show, 0 the picture, below 0 a pause; "0,5,6,7,11" by default), the
    // first WOWEE_BURST_AT seconds after entering the world (60), then every
    // WOWEE_BURST_EVERY (12), each WOWEE_BURST_FRAMES frames long (8). The
    // view is switched three seconds before its burst so the history has
    // settled. Files: <folder>/<WOWEE_BURST_TAG>_v<view>_<burst>_<frame>.png.
    if (const char* burstDir = std::getenv("WOWEE_BURST_DIR"); burstDir != nullptr && *burstDir != '\0') {
        static double burstWorldSince = -1.0;
        static int burstIndex = 0;
        static int burstFrame = -1;
        auto envNumber = [](const char* name, double fallback) {
            const char* v = std::getenv(name);
            return (v && *v) ? std::atof(v) : fallback;
        };
        std::vector<int> views;
        {
            const char* v = std::getenv("WOWEE_BURST_VIEWS");
            const std::string list = (v && *v) ? v : "0,5,6,7,11";
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string item =
                    list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) views.push_back(std::atoi(item.c_str()));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        const bool inWorld = gameHandler && gameHandler->getState() == game::WorldState::IN_WORLD;
        const double now = core::appTimeSeconds();
        if (!inWorld) {
            burstWorldSince = -1.0;
        } else if (burstWorldSince < 0.0) {
            burstWorldSince = now;
        } else if (burstIndex < static_cast<int>(views.size())) {
            const double at = envNumber("WOWEE_BURST_AT", 60.0) + envNumber("WOWEE_BURST_EVERY", 12.0) * burstIndex;
            const double since = now - burstWorldSince;
            if (burstFrame < 0 && since >= at - 3.0 && views[static_cast<size_t>(burstIndex)] >= 0) {
                renderer->setRtDebugView(views[static_cast<size_t>(burstIndex)]);
            }
            // A view below 0 is a pause: nothing is captured.
            if (burstFrame < 0 && since >= at && views[static_cast<size_t>(burstIndex)] < 0) {
                ++burstIndex;
            } else if (burstFrame < 0 && since >= at) {
                burstFrame = 0;
            }
            if (burstFrame >= 0) {
                const char* tag = std::getenv("WOWEE_BURST_TAG");
                char name[512];
                std::snprintf(name, sizeof(name), "%s/%s_v%d_%d_%02d.png", burstDir, (tag && *tag) ? tag : "burst",
                              views[static_cast<size_t>(burstIndex)], burstIndex, burstFrame);
                renderer->captureScreenshot(name);
                ++burstFrame;
                if (burstFrame >= static_cast<int>(envNumber("WOWEE_BURST_FRAMES", 8.0))) {
                    burstFrame = -1;
                    ++burstIndex;
                    LOG_WARNING("WOWEE_BURST: burst ", burstIndex, " of ", views.size(), " written");
                    if (burstIndex >= static_cast<int>(views.size())) renderer->setRtDebugView(0);
                }
            }
        }
    }

    // A recording of the client, from start-up, then done with - the recorder
    // checked end to end without a world to stand in or a key to press:
    // WOWEE_RECORD=<file.mp4>, for WOWEE_RECORD_SECONDS (five by default).
    // Started once the interface has settled, as the screenshot is; stopped by
    // quitting, which finishes the file on the way out.
    if (const char* record = std::getenv("WOWEE_RECORD"); record != nullptr && *record != '\0') {
        ++envRecordFrames_;
        if (envRecordFrames_ == kScreenshotFrame) {
            std::string error;
            if (renderer->startRecording(record, error)) {
                envRecordStart_ = std::chrono::steady_clock::now();
            } else {
                LOG_WARNING("WOWEE_RECORD: could not start: ", error);
                running = false;
            }
        } else if (envRecordFrames_ > kScreenshotFrame) {
            const char* limit = std::getenv("WOWEE_RECORD_SECONDS");
            const double seconds = (limit && *limit) ? std::atof(limit) : 5.0;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - envRecordStart_).count() >= seconds) {
                running = false;
            }
        }
    }

    stageStatFrames_ += 1;
    reportStageTimes();
    renderingFrame_ = false;
    processDeferredLeaveWorld();
}

void Application::noteStageTime(const char* stage, float milliseconds) {
    auto& stat = stageStats_[stage];
    stat.totalMs += milliseconds;
    stat.frames += 1;
    if (milliseconds > stat.worstMs) stat.worstMs = milliseconds;
}

void Application::reportStageTimes() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (stageStatsSince_.time_since_epoch().count() == 0) {
        stageStatsSince_ = now;
        return;
    }
    const float windowMs = duration<float, std::milli>(now - stageStatsSince_).count();
    if (windowMs < 10000.0f) return;

    if (stageStatFrames_ > 0) {
        const float fps = stageStatFrames_ * 1000.0f / windowMs;
        const bool loud = frameProfileEnabled_;
        std::ostringstream head;
        head << "frame budget over " << static_cast<int>(windowMs / 1000.0f)
             << "s: " << stageStatFrames_ << " frames, " << fps << " fps, "
             << windowMs / static_cast<float>(stageStatFrames_) << "ms each";
        if (loud) LOG_WARNING(head.str()); else LOG_INFO(head.str());
        // Worst first: the stage to look at is the one taking the time, and a
        // long tail matters more than an average on a device that stutters.
        std::vector<std::pair<double, std::string>> byCost;
        byCost.reserve(stageStats_.size());
        for (const auto& [name, stat] : stageStats_) {
            byCost.emplace_back(stat.totalMs, name);
        }
        std::sort(byCost.rbegin(), byCost.rend());
        for (const auto& [total, name] : byCost) {
            const auto& stat = stageStats_[name];
            if (stat.frames == 0) continue;
            std::ostringstream line;
            line << "  " << name << ": "
                 << static_cast<float>(total / stageStatFrames_) << "ms/frame avg, "
                 << stat.worstMs << "ms worst, "
                 << static_cast<int>(100.0 * total / windowMs) << "% of the window";
            if (loud) LOG_WARNING(line.str()); else LOG_INFO(line.str());
        }
    }

    // The GPU's own breakdown, from the last completed frame rather than an
    // average: the CPU stages above are where the frame *waits*, and once
    // beginFrame and endFrame dominate they say nothing about which pass the
    // GPU spent it in. One frame is enough to rank the passes, and reading a
    // window of them would mean keeping the marks alive across slots.
    if (renderer && renderer->getVkContext()) {
        const auto& gpu = renderer->getVkContext()->gpuTimings();
        if (!gpu.empty()) {
            double total = 0.0;
            for (const auto& [name, ms] : gpu) total += ms;
            std::ostringstream head;
            head << "  GPU, last frame: " << total << "ms across " << gpu.size()
                 << " passes";
            if (frameProfileEnabled_) LOG_WARNING(head.str()); else LOG_INFO(head.str());
            for (const auto& [name, ms] : gpu) {
                std::ostringstream line;
                line << "    " << name << ": " << ms << "ms";
                if (frameProfileEnabled_) LOG_WARNING(line.str()); else LOG_INFO(line.str());
            }
            // ...and whether any of that is worth believing.
            //
            // A mark is a timestamp, and this driver resolves a timestamp
            // against the render pass that contains it rather than the draw:
            // every mark inside one pass reads the same clock, so the first
            // mark absorbs the whole pass and the rest report a couple of
            // microseconds. That is how a profile came to say terrain cost
            // 14ms and that the characters, the buildings, the water and
            // every doodad in the city came to twenty microseconds between
            // them. Passes that read sensibly are the ones with a render pass
            // to themselves. Rather than let the breakdown be taken at face
            // value, count the collapsed marks and name the way round it.
            int collapsed = 0;
            for (const auto& [name, ms] : gpu) {
                (void)name;
                if (ms < 0.02) ++collapsed;
            }
            if (collapsed >= 2 && total > 1.0) {
                std::ostringstream note;
                note << "    (" << collapsed << " of those read under 20us: this driver "
                        "resolves a timestamp to its render pass, so the first mark in a "
                        "pass holds the whole pass and the rest are empty. Run with "
                        "WOWEE_PASS_ABLATION=1 to attribute the scene pass.)";
                if (frameProfileEnabled_) LOG_WARNING(note.str()); else LOG_INFO(note.str());
            }
        }
    }

    stageStats_.clear();
    stageStatFrames_ = 0;
    stageStatsSince_ = now;
}

void Application::setupUICallbacks() {
    // Everything below binds references by dereferencing these, and four of
    // them are only created inside `if (assetManager->initialize(...))`. When
    // that fails they stay null, `*entitySpawner_` binds a reference to
    // address zero, and the first callback to touch a member writes through
    // it - the mount callback, which fires from handleLoginVerifyWorld, so the
    // client came up, listed the characters, and died the instant the world
    // was entered. Reported as issue #117 with the fault address 0x9f8, which
    // is a member offset from nothing.
    //
    // A reference bound to null is undefined the moment it is bound, so the
    // check has to be here rather than inside the callbacks.
    if (!entitySpawner_ || !renderer || !gameHandler || !assetManager ||
        !uiManager || !authHandler || !appearanceComposer_) {
        LOG_ERROR("Cannot wire the interface callbacks: ",
                  !assetManager ? "asset manager" :
                  !entitySpawner_ ? "entity spawner" :
                  !appearanceComposer_ ? "appearance composer" :
                  !renderer ? "renderer" :
                  !gameHandler ? "game handler" :
                  !uiManager ? "UI manager" : "auth handler",
                  " was never created - the game data path is the usual reason. "
                  "The client will run without them rather than crash on the "
                  "first packet that needs one.");
        return;
    }

    // ── UI screen callbacks (auth, realm, character selection/creation) ──
    uiScreenCallbacks_ = std::make_unique<UIScreenCallbackHandler>(
        *uiManager, *gameHandler, *authHandler, expansionRegistry_.get(),
        assetManager.get(),
        [this](AppState s) { setState(s); });
    uiScreenCallbacks_->setupCallbacks();

    // ── World entry, unstuck, hearthstone, bind point ──
    worldEntryCallbacks_ = std::make_unique<WorldEntryCallbackHandler>(
        *renderer, *gameHandler, worldLoader_.get(), entitySpawner_.get(),
        assetManager.get());
    worldEntryCallbacks_->setupCallbacks();

    // ── Entity spawn/despawn (creatures, players, game objects) ──
    entitySpawnCallbacks_ = std::make_unique<EntitySpawnCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler,
        [this](uint64_t guid) {
            uint64_t localGuid = gameHandler ? gameHandler->getPlayerGuid() : 0;
            uint64_t activeGuid = gameHandler ? gameHandler->getActiveCharacterGuid() : 0;
            return (localGuid != 0 && guid == localGuid) ||
                   (activeGuid != 0 && guid == activeGuid) ||
                   (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_);
        });
    entitySpawnCallbacks_->setupCallbacks();

    // ── Animation: death, respawn, swing, hit, spell, emote, charge, etc. ──
    animationCallbacks_ = std::make_unique<AnimationCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler);
    animationCallbacks_->setupCallbacks();

    // ── NPC interaction: greeting, farewell, vendor, aggro voice ──
    npcInteractionCallbacks_ = std::make_unique<NPCInteractionCallbackHandler>(
        *gameHandler, audioCoordinator_.get());
    npcInteractionCallbacks_->setupCallbacks();

    // ── Audio: music, sound effects, level-up, achievement, LFG ──
    audioCallbacks_ = std::make_unique<AudioCallbackHandler>(
        *assetManager, audioCoordinator_.get(), renderer.get(),
        uiManager.get(), *gameHandler);
    audioCallbacks_->setupCallbacks();

    // ── Transport: mount, taxi, transport spawn/move ──
    transportCallbacks_ = std::make_unique<TransportCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler);
    transportCallbacks_->setupCallbacks();

    // ── Cinematics: SMSG_TRIGGER_CINEMATIC and StopCinematic ──
    cinematicPlayer_ = std::make_unique<CinematicPlayer>(*gameHandler, *renderer, *assetManager);
    gameHandler->setCinematicCallbacks(
        [this](uint32_t sequenceId) { if (cinematicPlayer_) cinematicPlayer_->start(sequenceId); },
        [this] { if (cinematicPlayer_) cinematicPlayer_->stop(); });
    gameHandler->setWorldLoadingProbe([this] { return worldLoader_ && worldLoader_->isLoadingWorld(); });
}

void Application::spawnPlayerCharacter() {
    if (playerCharacterSpawned) return;
    // The character, whatever the display: a form or a morph is put back on
    // it by syncPlayerDisplayModel the next frame. In the body of a display it
    // wears its own things in, it is that one's character (0x0071d010).
    if (renderer) renderer->setCharacterShownAsDisplay(false);
    const auto body = gameHandler ? gameHandler->getShownBody(gameHandler->getPlayerGuid()) : std::nullopt;
    shownPlayerDisplay_ = body ? body->displayId : 0;
    const game::Race race = body ? static_cast<game::Race>(body->race) : playerRace_;
    const game::Gender gender = !body ? playerGender_ : body->sex == 1 ? game::Gender::FEMALE : game::Gender::MALE;
    if (!renderer || !renderer->getCharacterRenderer() || !renderer->getCamera()) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    auto* camera = renderer->getCamera();
    bool loaded = false;
    // A nonbinary character chose which body to wear, and that choice lives on
    // the character rather than in the gender. Without it this took the
    // two-argument default - the male model - so the paper doll and the
    // character screen showed the body the player picked and the world showed
    // the other one. The voice below already resolves it this way.
    bool useFemaleModel = false;
    if (gender == game::Gender::NONBINARY && gameHandler) {
        if (const game::Character* ch = gameHandler->getActiveCharacter()) {
            useFemaleModel = ch->useFemaleModel;
        }
    }
    std::string m2Path =
        game::getPlayerModelPath(race, gender, useFemaleModel);

    // Try loading selected character model from MPQ
    if (assetManager && assetManager->isInitialized()) {
        auto m2Data = assetManager->readFile(m2Path);
        if (!m2Data.empty()) {
            auto model = pipeline::M2Loader::load(m2Data);
            if (model.name.empty()) model.name = m2Path;

            // Load skin file for submesh/batch data
            std::string skinPath = pipeline::skinPathForM2(m2Path);
            auto skinData = assetManager->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            }

            if (model.isValid()) {
                // Log texture slots
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    auto& tex = model.textures[ti];
                    LOG_INFO("  Texture ", ti, ": type=", tex.type, " name='", tex.filename, "'");
                }

                // Resolve textures from CharSections.dbc via AppearanceComposer
                PlayerTextureInfo texInfo;
                bool useCharSections = true;
                if (appearanceComposer_) {
                    uint32_t appearanceBytes = 0;
                    int facialHairId = -1;
                    if (gameHandler) {
                        const game::Character* activeChar = gameHandler->getActiveCharacter();
                        if (activeChar) {
                            appearanceBytes = activeChar->appearanceBytes;
                            facialHairId = activeChar->facialFeatures;
                        }
                    }
                    texInfo = appearanceComposer_->resolvePlayerTextures(
                        model, race, gender, appearanceBytes, useFemaleModel, facialHairId);
                }

                // Load external .anim files for sequences with external data.
                // Sequences WITH flag 0x20 have their animation data inline in the M2 file.
                // Sequences WITHOUT flag 0x20 store data in external .anim files.
                pipeline::loadExternalAnimations(*assetManager, m2Path, m2Data, model);

                charRenderer->loadModel(model, 1);

                // Apply composited textures via AppearanceComposer (saves skin state for re-compositing)
                if (useCharSections && appearanceComposer_) {
                    appearanceComposer_->compositePlayerSkin(1, texInfo);
                }

                loaded = true;
                LOG_INFO("Loaded character model: ", m2Path, " (", model.vertices.size(), " verts, ",
                         model.bones.size(), " bones, ", model.sequences.size(), " anims, ",
                         model.indices.size(), " indices, ", model.batches.size(), " batches");
                // Log all animation sequence IDs
                for (size_t i = 0; i < model.sequences.size(); i++) {
                }
            }
        }
    }

    // Fallback: create a simple cube if MPQ not available
    if (!loaded) {
        pipeline::M2Model testModel;
        float size = 2.0f;
        glm::vec3 cubePos[] = {
            {-size, -size, -size}, { size, -size, -size},
            { size,  size, -size}, {-size,  size, -size},
            {-size, -size,  size}, { size, -size,  size},
            { size,  size,  size}, {-size,  size,  size}
        };
        for (const auto& pos : cubePos) {
            pipeline::M2Vertex v;
            v.position = pos;
            v.normal = glm::normalize(pos);
            v.texCoords[0] = glm::vec2(0.0f);
            v.boneWeights[0] = 255;
            v.boneWeights[1] = v.boneWeights[2] = v.boneWeights[3] = 0;
            v.boneIndices[0] = 0;
            v.boneIndices[1] = v.boneIndices[2] = v.boneIndices[3] = 0;
            testModel.vertices.push_back(v);
        }
        uint16_t cubeIndices[] = {
            0,1,2, 0,2,3, 4,6,5, 4,7,6,
            0,4,5, 0,5,1, 2,6,7, 2,7,3,
            0,3,7, 0,7,4, 1,5,6, 1,6,2
        };
        for (uint16_t idx : cubeIndices)
            testModel.indices.push_back(idx);

        pipeline::M2Bone bone;
        bone.keyBoneId = -1;
        bone.flags = 0;
        bone.parentBone = -1;
        bone.submeshId = 0;
        bone.pivot = glm::vec3(0.0f);
        testModel.bones.push_back(bone);

        pipeline::M2Sequence seq{};
        seq.id = 0;
        seq.duration = 1000;
        testModel.sequences.push_back(seq);

        testModel.name = "TestCube";
        testModel.globalFlags = 0;
        charRenderer->loadModel(testModel, 1);
        LOG_INFO("Loaded fallback cube model (no MPQ data)");
    }

    // Spawn character at the camera controller's default position (matches hearthstone).
    // Most presets snap to floor; explicit WMO-floor presets keep their authored Z.
    auto* camCtrl = renderer->getCameraController();
    glm::vec3 spawnPos = camCtrl ? camCtrl->getDefaultPosition()
                                 : (camera->getPosition() - glm::vec3(0.0f, 0.0f, 5.0f));
    if (spawnSnapToGround && renderer->getTerrainManager()) {
        auto terrainH = renderer->getTerrainManager()->getHeightAt(spawnPos.x, spawnPos.y);
        if (terrainH) {
            spawnPos.z = *terrainH + 0.1f;
        }
    }
    uint32_t instanceId = charRenderer->createInstance(1, spawnPos,
        glm::vec3(0.0f), 1.0f);  // Scale 1.0 = normal WoW character size

    if (instanceId > 0) {
	        // Set up third-person follow
	        renderer->getCharacterPosition() = spawnPos;
	        renderer->setCharacterFollow(instanceId);

	        // The character component's geosets before the equipment is known.
	        if (appearanceComposer_ && gameHandler) {
	            if (const game::Character* ch = gameHandler->getActiveCharacter()) {
	                charRenderer->setActiveGeosets(
	                    instanceId, appearanceComposer_->playerGeosets(
	                                    appearanceComposer_->playerLook(gameHandler->getShownCharacter(*ch)), instanceId));
	            }
	        }
	        // The player's type 8 slot is filled from CharSections by
	        // resolvePlayerTextures above, so the head-detail batch has art to
	        // draw and is worth drawing. Nothing else is set up for it.
	        charRenderer->setDrawSkinExtra(instanceId, true);

        // Play idle animation
        charRenderer->playAnimation(instanceId, rendering::anim::STAND, true);
        LOG_INFO("Spawned player character at (",
                static_cast<int>(spawnPos.x), ", ",
                static_cast<int>(spawnPos.y), ", ",
                static_cast<int>(spawnPos.z), ")");
        playerCharacterSpawned = true;


        // Track which character's appearance this instance represents so we can
        // respawn if the user logs into a different character without restarting.
        spawnedPlayerGuid_ = gameHandler ? gameHandler->getActiveCharacterGuid() : 0;
        spawnedAppearanceBytes_ = 0;
        spawnedFacialFeatures_ = 0;
        if (gameHandler) {
            if (const game::Character* ch = gameHandler->getActiveCharacter()) {
                spawnedAppearanceBytes_ = ch->appearanceBytes;
                spawnedFacialFeatures_ = ch->facialFeatures;
            }
        }

        // Set up camera controller for first-person player hiding
        if (renderer->getCameraController()) {
            renderer->getCameraController()->setCharacterRenderer(charRenderer, instanceId);
        }

        // Load equipped weapons (sword + shield)
        if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons();
    }
}

void Application::refreshPlayerCharacterModel() {
    if (!playerCharacterSpawned || !gameHandler) return;
    const game::Character* ch = gameHandler->getActiveCharacter();
    if (!ch) return;
    // Only rebuild when the visible appearance actually changed. PLAYER_BYTES_2
    // also carries rest state, so the appearance hook fires on entering/leaving
    // inns and cities - a full respawn on those would be needless and jarring.
    if (ch->appearanceBytes == spawnedAppearanceBytes_ &&
        ch->facialFeatures == spawnedFacialFeatures_) {
        return;
    }
    LOG_INFO("Rebuilding player model in place for appearance change (barber shop)");

    // Keep the character exactly where it is - this is the same respawn path
    // teleport uses (so equipment/geometry are fully re-applied), just live.
    const glm::vec3 savedPos = renderer ? renderer->getCharacterPosition() : glm::vec3(0.0f);

    if (renderer && renderer->getCharacterRenderer()) {
        uint32_t oldInst = renderer->getCharacterInstanceId();
        if (oldInst > 0) {
            renderer->setCharacterFollow(0);
            if (auto* ac = renderer->getAnimationController()) ac->clearMount();
            renderer->getCharacterRenderer()->removeInstance(oldInst);
        }
    }
    playerCharacterSpawned = false;
    spawnedPlayerGuid_ = 0;
    spawnedAppearanceBytes_ = 0;
    spawnedFacialFeatures_ = 0;

    spawnSnapToGround = false; // don't snap Z - stay at the current position
    spawnPlayerCharacter();

    if (renderer) renderer->getCharacterPosition() = savedPos;

    // As it sat: the client changes the seated model, the new one stood up on
    // the chair.
    if (gameHandler && renderer) {
        const uint8_t stand = gameHandler->getStandState();
        if (auto* cc = renderer->getCameraController()) cc->applyServerStandState(stand);
        auto* cr = renderer->getCharacterRenderer();
        if (cr && renderer->getCharacterInstanceId() != 0)
            cr->standStateChanged(renderer->getCharacterInstanceId(), stand);
    }

    // Force equipment geosets/textures to be re-composited onto the fresh model
    // next frame - the respawn only builds the base body, and equipment isn't
    // "dirty" (nothing was equipped), so without this the model loses its armor.
    if (gameHandler) gameHandler->resetEquipmentDirtyTracking();
}

void Application::buildFactionHostilityMap(uint8_t playerRace) {
    if (!assetManager || !gameHandler) return;
    game::buildFactionHostilityMap(*assetManager, *gameHandler, playerRace);
}

// Render bounds/position queries - delegates to EntitySpawner
bool Application::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (entitySpawner_) return entitySpawner_->getRenderBoundsForGuid(guid, outCenter, outRadius);
    return false;
}

bool Application::gameObjectDrawsNothing(uint64_t guid) const {
    return entitySpawner_ && entitySpawner_->gameObjectDrawsNothing(guid);
}

uint64_t Application::gameObjectOfFace(bool building, uint32_t owner, const glm::vec3& at) const {
    return entitySpawner_ ? entitySpawner_->gameObjectOfFace(building, owner, at) : 0;
}

rendering::pointer_pick::ObjectHit Application::rayHitGameObject(uint64_t guid, const glm::vec3& origin,
                                                                const glm::vec3& direction, float maxDistance) const {
    if (!entitySpawner_) return {};
    return entitySpawner_->rayHitGameObject(guid, origin, direction, maxDistance);
}

std::optional<float> Application::rayHitUnit(uint64_t guid, const glm::vec3& origin, const glm::vec3& direction,
                                             float maxDistance) const {
    if (!entitySpawner_) return std::nullopt;
    return entitySpawner_->rayHitUnit(guid, origin, direction, maxDistance);
}

void Application::setHighlightedGameObject(uint64_t guid) {
    if (!renderer) return;
    // Resolved every frame: the object's model can arrive, or be replaced,
    // while the pointer rests on it.
    uint32_t instance = 0;
    int kind = 0;
    if (guid != 0 && entitySpawner_) {
        const auto& objects = entitySpawner_->getGameObjectInstances();
        auto it = objects.find(guid);
        if (it != objects.end()) {
            instance = it->second.instanceId;
            kind = it->second.isWmo ? 2 : 1;
        } else if (auto c = entitySpawner_->getCreatureInstances().find(guid);
                   c != entitySpawner_->getCreatureInstances().end()) {
            // A unit's model, in the character renderer (0x00743c70 is the
            // object's, whatever kind of object it is).
            instance = c->second;
            kind = 3;
        } else if (auto p = entitySpawner_->getPlayerInstances().find(guid);
                   p != entitySpawner_->getPlayerInstances().end()) {
            instance = p->second;
            kind = 3;
        }
    }
    if (instance == highlightedInstance_ && kind == highlightedKind_) return;
    highlightedInstance_ = instance;
    highlightedKind_ = kind;
    // Only now, with a change to make. This runs from the interface every
    // frame, and asking for the model renderer waits for the world's doodads
    // to finish recording - which the highlight they draw does need.
    auto* m2 = renderer->getM2Renderer();
    auto* wmo = renderer->getWMORenderer();
    // 0x00743bc0 on the old one: a walk rather than the remembered id, which
    // a despawn could have handed to another model.
    if (m2) m2->clearInstanceHighlights();
    if (wmo) wmo->clearInstanceHighlights();
    if (auto* cr = renderer->getCharacterRenderer()) cr->setHighlightedInstance(kind == 3 ? instance : 0);
    // 0x00743c70 on the new one. A WMO gets the light's ambient added into
    // its c29 (0x007964a0 compares its GUID with the one recorded here,
    // 0x007a8430); an M2 gets it in +0x18c..0x194, added to its ambient.
    if (kind == 2 && wmo) wmo->setInstanceHighlight(instance, true);
    if (kind == 1 && m2) m2->setInstanceHighlight(instance, 1.0f);
}

std::optional<glm::vec3> Application::getUnitNamePosition(uint64_t guid) const {
    return entitySpawner_ ? entitySpawner_->unitNamePosition(guid) : std::nullopt;
}

std::optional<std::pair<glm::vec3, float>> Application::getSelectionCircle(uint64_t guid) const {
    if (!entitySpawner_) return std::nullopt;
    const auto placement = entitySpawner_->selectionCircle(guid);
    if (!placement) return std::nullopt;
    return std::make_pair(placement->position, placement->radius);
}

bool Application::isUnitHeldForMount(uint64_t guid) const {
    return entitySpawner_ && entitySpawner_->isUnitHeldForMount(guid);
}

bool Application::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    if (entitySpawner_) return entitySpawner_->getRenderPositionForGuid(guid, outPos);
    return false;
}

void Application::updateLootSparkles() {
    auto* sparkles = renderer ? renderer->getLootSparkles() : nullptr;
    if (!sparkles || !gameHandler) return;
    // Dead and marked lootable - the flag the server sends only to a player
    // allowed to loot the body, so a corpse tapped by someone else stays dull.
    std::vector<rendering::LootSparkles::Corpse> corpses;
    for (const auto& [guid, entity] : gameHandler->getEntityManager().getEntities()) {
        if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
        const auto* unit = static_cast<const game::Unit*>(entity.get());
        if (unit->getHealth() != 0) continue;
        if ((unit->getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE) == 0) continue;
        glm::vec3 position;
        if (getRenderPositionForGuid(guid, position)) corpses.emplace_back(guid, position);
    }
    sparkles->update(renderer->getM2Renderer(), assetManager.get(), corpses);
}

void Application::updateWorldFloorSound() {
    // 0x00606f90: the camera's target against the floor under the world -
    // the tile's lower MFBO plane, or -500 where there is none. At or above
    // it the target has ground; below it, having had ground, the sound of
    // falling out of the world plays once.
    if (!gameHandler || !renderer || gameHandler->getState() != game::WorldState::IN_WORLD) return;
    auto* terrain = renderer->getTerrainManager();
    if (!terrain) return;
    const auto& mi = gameHandler->getMovementInfo();
    const glm::vec3 wire = core::coords::canonicalToServer(glm::vec3(mi.x, mi.y, mi.z));
    const float floor = terrain->flightFloorAt(wire.x, wire.y).value_or(-500.0f);
    if (floor <= wire.z) {
        aboveWorldFloor_ = true;
        return;
    }
    if (aboveWorldFloor_) {
        if (auto* ac = getAudioCoordinator(); ac && ac->getUiSoundManager())
            ac->getUiSoundManager()->playByName("SpaceDeathUniversal");
    }
    aboveWorldFloor_ = false;
}

void Application::updateFlightCeiling() {
    // 0x0073a890, the active player flying (CAN_FLY and FLYING): two seconds
    // after its first flight, and every two seconds after each bump, it is
    // held against the tile's upper MFBO plane. Where its top - its position
    // plus its collision height - is within two yards of the ceiling, it
    // swims backwards (45, through 0x007176f0's fallbacks) and takes a wound
    // that is not critical (0x00736640(0)) - not while its own vehicle kit
    // holds its animation (0x004f6210 -> 0x007571c0) - and its sound row's
    // exertion plays.
    if (!gameHandler || !renderer || gameHandler->getState() != game::WorldState::IN_WORLD) return;
    const auto& mi = gameHandler->getMovementInfo();
    constexpr uint32_t kFlyingFlags = static_cast<uint32_t>(game::MovementFlags::CAN_FLY) |
                                      static_cast<uint32_t>(game::MovementFlags::FLYING);
    if ((mi.flags & kFlyingFlags) != kFlyingFlags) return;
    const auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
    constexpr uint64_t kBumpIntervalMs = 2000;
    if (!flightCeilingArmed_) {
        flightCeilingArmed_ = true;
        flightCeilingDueMs_ = nowMs + kBumpIntervalMs;
    }
    if (nowMs < flightCeilingDueMs_) return;
    auto* terrain = renderer->getTerrainManager();
    if (!terrain) return;
    const glm::vec3 wire = core::coords::canonicalToServer(glm::vec3(mi.x, mi.y, mi.z));
    const auto ceiling = terrain->flightCeilingAt(wire.x, wire.y);
    if (!ceiling) return;
    auto* cc = renderer->getCameraController();
    const float height = cc ? cc->collisionHeight() : 0.0f;
    if (!(*ceiling - 2.0f < wire.z + height)) return;
    if (!entitySpawner_ || !entitySpawner_->vehicleKitBusy(gameHandler->getPlayerGuid())) {
        auto* cr = renderer->getCharacterRenderer();
        const uint32_t instance = renderer->getCharacterInstanceId();
        if (cr && instance != 0) {
            cr->playAnimation(instance, rendering::anim::SWIM_BACKWARDS, false);
            if (const auto wound = cr->woundFor(instance, false)) cr->playWound(instance, *wound);
        }
    }
    if (entitySpawner_) {
        if (auto entity = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
            entity && entity->isUnit())
            entitySpawner_->playFlightCeilingSound(static_cast<const game::Unit&>(*entity));
    }
    flightCeilingDueMs_ = nowMs + kBumpIntervalMs;
}

void Application::updateQuestMarkers() {
    auto* markers = renderer ? renderer->getQuestGiverMarkers() : nullptr;
    if (!markers || !gameHandler) return;

    // The low-level marks are hung only while the minimap tracks trivial
    // quests (0x0057bf30).
    const int tracked = gameHandler->minimapOtherTracking();
    const bool trivial =
        tracked >= 0 && tracked < static_cast<int>(game::minimap_tracking::kOther.size()) &&
        game::minimap_tracking::kOther[static_cast<size_t>(tracked)].kind ==
            game::minimap_tracking::Kind::TrivialQuests;

    const rendering::Camera* camera = renderer->getCamera();
    std::vector<rendering::QuestGiverMarkers::Marker> wanted;
    // Not with the interface hidden (Alt-Z): SetUIVisibility re-flags every
    // unit's TalkToMe model by it (0x00517c20 -> 0x00517be0 -> 0x00745140).
    std::unordered_map<uint64_t, game::QuestGiverStatus> statuses;
    if (gameHandler->isUiVisible()) {
        statuses = gameHandler->getNpcQuestStatuses();
        for (const auto& [guid, known] : gameHandler->taxiNodeStatuses())
            if (!known) statuses.try_emplace(guid, game::QuestGiverStatus::NONE);
    }
    for (const auto& [guid, status] : statuses) {
        const char* model = game::questGiverMarkerModel(status, trivial, gameHandler->isTaxiNodeUnknown(guid));
        if (!model) continue;
        auto entity = gameHandler->getEntityManager().getEntity(guid);
        if (!entity) continue;
        // A unit's mark is put up only while it lives (0x007192e0).
        if (entity->getType() == game::ObjectType::UNIT &&
            static_cast<const game::Unit&>(*entity).getHealth() == 0) {
            continue;
        }
        const auto frame = entitySpawner_ ? entitySpawner_->questMarkerFrame(guid) : std::nullopt;
        if (!frame) continue;
        // Turned to the camera, whichever way the questgiver faces.
        wanted.push_back({guid, model,
                          camera ? rendering::markerFrameFacing(*frame, camera->getPosition()) : *frame});
    }
    markers->update(renderer->getM2Renderer(), assetManager.get(), wanted);
}

void Application::setupTestTransport() {
    if (!entitySpawner_) return;
    if (entitySpawner_->isTestTransportSetup()) return;
    if (!gameHandler || !renderer || !assetManager) return;

    auto* transportManager = gameHandler->getTransportManager();
    auto* wmoRenderer = renderer->getWMORenderer();
    if (!transportManager || !wmoRenderer) return;

    LOG_INFO("========================================");
    LOG_INFO("   SETTING UP TEST TRANSPORT");
    LOG_INFO("========================================");

    // Connect transport manager to WMO renderer
    transportManager->setWMORenderer(wmoRenderer);

    // Connect WMORenderer to M2Renderer (for hierarchical transforms: doodads following WMO parents)
    if (renderer->getM2Renderer()) {
        wmoRenderer->setM2Renderer(renderer->getM2Renderer());
        LOG_INFO("WMORenderer connected to M2Renderer for test transport doodad transforms");
    }

    // Define a simple circular path around Stormwind harbor (canonical coordinates)
    // These coordinates are approximate - adjust based on actual harbor layout
    std::vector<glm::vec3> harborPath = {
        {-8833.0f, 628.0f, 94.0f},   // Start point (Stormwind harbor)
        {-8900.0f, 650.0f, 94.0f},   // Move west
        {-8950.0f, 700.0f, 94.0f},   // Northwest
        {-8950.0f, 780.0f, 94.0f},   // North
        {-8900.0f, 830.0f, 94.0f},   // Northeast
        {-8833.0f, 850.0f, 94.0f},   // East
        {-8766.0f, 830.0f, 94.0f},   // Southeast
        {-8716.0f, 780.0f, 94.0f},   // South
        {-8716.0f, 700.0f, 94.0f},   // Southwest
        {-8766.0f, 650.0f, 94.0f},   // Back to start direction
    };

    // Register the path with transport manager
    uint32_t pathId = 1;
    float speed = 12.0f;  // 12 units/sec (slower than taxi for a leisurely boat ride)
    transportManager->loadPathFromNodes(pathId, harborPath, true, speed);
    LOG_INFO("Registered transport path ", pathId, " with ", harborPath.size(), " waypoints, speed=", speed);

    // Try transport WMOs in manifest-backed paths first.
    std::vector<std::string> transportCandidates = {
        "World\\wmo\\transports\\transport_ship\\transportship.wmo",
        "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo",
        "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo",
        "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo",
        // Legacy fallbacks
        "Transports\\Transportship\\Transportship.wmo",
        "Transports\\Boat\\Boat.wmo",
    };

    std::string transportWmoPath;
    std::vector<uint8_t> wmoData;
    for (const auto& candidate : transportCandidates) {
        wmoData = assetManager->readFile(candidate);
        if (!wmoData.empty()) {
            transportWmoPath = candidate;
            break;
        }
    }

    if (wmoData.empty()) {
        LOG_WARNING("No transport WMO found - test transport disabled");
        LOG_INFO("Expected under World\\wmo\\transports\\...");
        return;
    }

    LOG_INFO("Using transport WMO: ", transportWmoPath);

    // Load WMO model
    pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
    LOG_INFO("Transport WMO root loaded: ", transportWmoPath, " nGroups=", wmoModel.nGroups);

    // Load WMO groups
    int loadedGroups = 0;
    if (wmoModel.nGroups > 0) {
        for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
            bool loaded = false;
            for (const std::string& groupPath :
                 pipeline::wmoGroupCandidates(transportWmoPath, gi)) {
                std::vector<uint8_t> groupData = assetManager->readFile(groupPath);
                if (groupData.empty()) continue;
                pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                loadedGroups++;
                loaded = true;
                break;
            }
            if (!loaded) {
                LOG_WARNING("  Failed to load WMO group ", gi, " for: ", transportWmoPath);
            }
        }
    }

    if (loadedGroups == 0 && wmoModel.nGroups > 0) {
        LOG_WARNING("Failed to load any WMO groups for transport");
        return;
    }

    // Load WMO into renderer
    uint32_t wmoModelId = 99999;  // Use high ID to avoid conflicts
    if (!wmoRenderer->loadModel(wmoModel, wmoModelId)) {
        LOG_WARNING("Failed to load transport WMO model into renderer");
        return;
    }

    // Create WMO instance at first waypoint (convert canonical to render coords)
    glm::vec3 startCanonical = harborPath[0];
    glm::vec3 startRender = core::coords::canonicalToRender(startCanonical);

    uint32_t wmoInstanceId = wmoRenderer->createInstance(wmoModelId, startRender,
                                                          glm::vec3(0.0f, 0.0f, 0.0f), 1.0f);

    if (wmoInstanceId == 0) {
        LOG_WARNING("Failed to create transport WMO instance");
        return;
    }

    // Register transport with transport manager
    uint64_t transportGuid = 0x1000000000000001ULL;  // Fake GUID for test
    transportManager->registerTransport(transportGuid, wmoInstanceId, pathId, startCanonical);

    // Optional: Set deck bounds (rough estimate for a ship deck)
    transportManager->setDeckBounds(transportGuid,
                                    glm::vec3(-15.0f, -30.0f, 0.0f),
                                    glm::vec3(15.0f, 30.0f, 10.0f));

    entitySpawner_->setTestTransportSetup(true);
    LOG_INFO("========================================");
    LOG_INFO("Test transport registered:");
    LOG_INFO("  GUID: 0x", std::hex, transportGuid, std::dec);
    LOG_INFO("  WMO Instance: ", wmoInstanceId);
    LOG_INFO("  Path: ", pathId, " (", harborPath.size(), " waypoints)");
    LOG_INFO("  Speed: ", speed, " units/sec");
    LOG_INFO("========================================");
    LOG_INFO("");
    LOG_INFO("To board the transport, use console command:");
    LOG_INFO("  /transport board");
    LOG_INFO("To disembark:");
    LOG_INFO("  /transport leave");
    LOG_INFO("========================================");
}

} // namespace core
} // namespace wowee

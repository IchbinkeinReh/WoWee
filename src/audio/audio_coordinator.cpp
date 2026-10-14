#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "audio/screen_effect_audio.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include <chrono>

namespace wowee {
namespace audio {

AudioCoordinator::AudioCoordinator() = default;

AudioCoordinator::~AudioCoordinator() {
    shutdown();
}

bool AudioCoordinator::initialize() {
    // Initialize AudioEngine (singleton)
    if (!AudioEngine::instance().initialize()) {
        LOG_WARNING("Failed to initialize AudioEngine - audio will be disabled");
        audioAvailable_ = false;
        return false;
    }
    audioAvailable_ = true;

    // Create all audio managers (initialized later with asset manager)
    musicManager_ = std::make_unique<MusicManager>();
    footstepManager_ = std::make_unique<FootstepManager>();
    activitySoundManager_ = std::make_unique<ActivitySoundManager>();
    mountSoundManager_ = std::make_unique<MountSoundManager>();
    npcVoiceManager_ = std::make_unique<NpcVoiceManager>();
    playerVoiceManager_ = std::make_unique<PlayerVoiceManager>();
    ambientSoundManager_ = std::make_unique<AmbientSoundManager>();
    uiSoundManager_ = std::make_unique<UiSoundManager>();
    combatSoundManager_ = std::make_unique<CombatSoundManager>();
    spellSoundManager_ = std::make_unique<SpellSoundManager>();
    movementSoundManager_ = std::make_unique<MovementSoundManager>();

    LOG_INFO("AudioCoordinator initialized with ", 11, " audio managers");
    return true;
}
void AudioCoordinator::shutdown() {
    // Reset all managers first (they may reference AudioEngine)
    movementSoundManager_.reset();
    spellSoundManager_.reset();
    combatSoundManager_.reset();
    uiSoundManager_.reset();
    ambientSoundManager_.reset();
    playerVoiceManager_.reset();
    npcVoiceManager_.reset();
    mountSoundManager_.reset();
    activitySoundManager_.reset();
    footstepManager_.reset();
    musicManager_.reset();

    // Shutdown audio engine last
    if (audioAvailable_) {
        AudioEngine::instance().shutdown();
        audioAvailable_ = false;
    }

    LOG_INFO("AudioCoordinator shutdown complete");
}

void AudioCoordinator::leaveWorld() {
    // 0x004c93b0: the music channel's two tracks at once (0x004c6450(1) on
    // 0xb4ac90) - SMSG_PLAY_MUSIC's among them, its slot (3) emptied - the
    // script's PlayMusic over half a second (0xb4ac3c), and the silence
    // between tracks over.
    if (musicManager_) musicManager_->stopZoneMusic(true);
    stopScriptMusic();
    silenceUntil_ = 0;
    // 0x004c85f0: the ambience's channels (0xb4ac50).
    if (ambientSoundManager_) ambientSoundManager_->stopAll();
    // The slots the next world fills again, from its place (0x0078e960
    // forgets the last) and its screen effect.
    musicSlots_ = {};
    ambiencePlace_.reset();
    worldStateRow_.reset();
    screenEffectAmbienceId_ = 0;
    screenEffectZoneMusicId_ = 0;
    LOG_INFO("World unloaded: music and ambience stopped");
}

void AudioCoordinator::forgetIntroMusic() {
    introDue_.clear();
}

void AudioCoordinator::playServerMusic(uint32_t soundId) {
    zone_music::fillServer(musicSlots_[zone_music::kSlotServer], static_cast<int32_t>(soundId));
}

void AudioCoordinator::playScriptMusic(const std::string& path) {
    // 0x004c9110: nothing with Sound_EnableMusic off (10); otherwise the
    // file replaces what the track held, looping (0x0087f7a0).
    if (!musicEnabled_ || !musicManager_) return;
    musicManager_->stopMusic(0.0f);
    musicManager_->playMusic(path, true, 0.0f);
    // Only once it plays (0x00878730): the hold, and the zone music's two
    // tracks falling over their own falls.
    scriptMusic_.played(musicEnabled_, musicManager_->isPlaying());
    if (scriptMusic_.held) musicManager_->stopZoneMusic(false);
}

void AudioCoordinator::stopScriptMusic() {
    // 0x004c8680: 0x004c6390(0xb4ac3c, 0, 0.5) and the hold lifted.
    if (musicManager_) musicManager_->stopMusic(500.0f);
    scriptMusic_.stopped();
}

void AudioCoordinator::setMusicEnabled(bool enabled) {
    if (enabled == musicEnabled_) return;
    musicEnabled_ = enabled;
    if (enabled) return;
    // 0x004d1590: off, the music stops as at the unload (0x004c93b0),
    // SMSG_PLAY_MUSIC's slot is emptied (0x004c92f0) and the script's music
    // stops (0x004c8680).
    if (musicManager_) musicManager_->stopZoneMusic(true);
    stopScriptMusic();
    musicSlots_[zone_music::kSlotServer] = {};
    silenceUntil_ = 0;
}

uint64_t AudioCoordinator::nowMs() const {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

zone_music::Context AudioCoordinator::musicContext() const {
    zone_music::Context c;
    c.enabled = musicEnabled_;
    c.noDelay = musicNoDelay_;
    c.time = musicTime_;
    c.now = nowMs();
    c.silenceUntil = silenceUntil_;
    c.introDue = &introDue_;
    return c;
}

void AudioCoordinator::setMusicSlotZoneMusic(int slot, int64_t zoneMusicId) {
    namespace zm = zone_music;
    if (slot < 0 || slot >= zm::kSlotCount) return;
    // ZoneMusic.dbc: 0 ID, 2..3 SilenceIntervalMin, 4..5 SilenceIntervalMax,
    // 6..7 Sounds - each day then night.
    std::optional<zm::ZoneMusicRow> row;
    if (zoneMusicId > 0 && cachedAssets_) {
        auto dbc = cachedAssets_->loadDBCOptional("ZoneMusic.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() >= 8
                                ? dbc->findRecordById(static_cast<uint32_t>(zoneMusicId))
                                : -1;
        if (idx >= 0) {
            const auto r = static_cast<uint32_t>(idx);
            row = zm::ZoneMusicRow{{dbc->getUInt32(r, 2), dbc->getUInt32(r, 3)},
                                   {dbc->getUInt32(r, 4), dbc->getUInt32(r, 5)},
                                   {dbc->getInt32(r, 6), dbc->getInt32(r, 7)}};
        }
    }
    zm::fillZoneMusic(musicSlots_[static_cast<size_t>(slot)], row ? &*row : nullptr);
    const zm::Channel ch = musicManager_ ? musicManager_->zoneChannel() : zm::Channel{};
    const zm::Pick now = zm::pick(musicSlots_, ch, musicContext());
    if (zm::zoneMusicEndsSilence(slot, zoneMusicId, row.has_value(), now, ch)) silenceUntil_ = 0;
}

void AudioCoordinator::setMusicSlotIntro(int slot, int64_t introId) {
    namespace zm = zone_music;
    if (slot < 0 || slot >= zm::kSlotCount) return;
    // ZoneIntroMusicTable.dbc: 0 ID, 2 SoundID, 3 Priority, 4 MinDelayMinutes.
    std::optional<zm::IntroRow> row;
    if (introId > 0 && cachedAssets_) {
        auto dbc = cachedAssets_->loadDBCOptional("ZoneIntroMusicTable.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() >= 5
                                ? dbc->findRecordById(static_cast<uint32_t>(introId))
                                : -1;
        if (idx >= 0) {
            const auto r = static_cast<uint32_t>(idx);
            row = zm::IntroRow{dbc->getInt32(r, 2), dbc->getUInt32(r, 4)};
        }
    }
    zm::fillIntro(musicSlots_[static_cast<size_t>(slot)], row ? &*row : nullptr);
}

void AudioCoordinator::updateAmbience(const ZoneAudioContext& ctx) {
    namespace za = zone_ambience;
    namespace zm = zone_music;
    auto* ambience = ambientSoundManager_.get();
    auto* assets = ctx.assetManager;

    // Slot 0, the place (0x0078e9a0): its AreaTable rows' SoundAmbience
    // (+0x1c) and its building's; and the music's slots 0 and 5, its
    // ZoneMusic (+0x20) and intro (+0x24) by the same chain.
    if (ctx.place && assets && ctx.place != ambiencePlace_) {
        ambiencePlace_ = ctx.place;
        const za::Place& at = *ctx.place;
        const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("AreaTable")
                                                            : nullptr;
        auto areas = assets->loadDBCOptional("AreaTable.dbc");
        const auto column = [&](const char* name) { return layout ? layout->tryField(name) : 0xFFFFFFFFu; };
        const auto rowValue = [&](uint32_t id, uint32_t col) -> uint32_t {
            if (id == 0 || !areas || !areas->isLoaded() || col >= areas->getFieldCount()) return 0;
            const int32_t idx = areas->findRecordById(id);
            return idx >= 0 ? areas->getUInt32(static_cast<uint32_t>(idx), col) : 0;
        };
        const auto chain = [&](uint32_t col, uint32_t building, uint32_t group) {
            return za::placeAmbience(rowValue(at.zone, col), rowValue(at.area, col), building, group, at.indoors);
        };
        if (ambience) {
            ambience->setSlotAmbience(za::kSlotPlace,
                                      chain(column("SoundAmbience"), at.buildingAmbience, at.groupAmbience));
        }
        // 0x0078e9a0 fills the intro (0x004c8cf0) before the music
        // (0x004c8d80).
        setMusicSlotIntro(zm::kSlotPlaceIntro, chain(column("IntroSound"), at.buildingIntro, at.groupIntro));
        setMusicSlotZoneMusic(zm::kSlotPlace, chain(column("ZoneMusic"), at.buildingMusic, at.groupMusic));
    }

    // Slot 1, the world states' (0x004cbe70): WorldStateZoneSounds.dbc, 0
    // the world state, 1 the value, 2 the area, 3 the WMOAreaTable row, 4 the
    // intro, 5 the ZoneMusic, 6 the SoundAmbience. Its intro goes to the
    // music's slot 6 and its ZoneMusic to its slot 1.
    if (!worldStateSoundsLoaded_ && assets) {
        worldStateSoundsLoaded_ = true;
        auto dbc = assets->loadDBCOptional("WorldStateZoneSounds.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 8) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                za::WorldStateSound row;
                row.worldState = dbc->getUInt32(i, 0);
                row.value = dbc->getUInt32(i, 1);
                row.area = dbc->getUInt32(i, 2);
                row.wmoArea = dbc->getUInt32(i, 3);
                row.intro = dbc->getUInt32(i, 4);
                row.music = dbc->getUInt32(i, 5);
                row.ambience = dbc->getUInt32(i, 6);
                worldStateSounds_.push_back(row);
            }
        }
    }
    if (ambiencePlace_) {
        const auto value = [&](uint32_t state) -> uint32_t {
            if (!ctx.worldStates) return 0;
            const auto it = ctx.worldStates->find(state);
            return it != ctx.worldStates->end() ? it->second : 0;
        };
        const std::optional<za::WorldStateSound> row = za::worldStateRow(worldStateSounds_, *ambiencePlace_, value);
        if (row != worldStateRow_) {
            worldStateRow_ = row;
            if (ambience) ambience->setSlotAmbience(za::kSlotWorldState, row ? row->ambience : 0);
            setMusicSlotIntro(zm::kSlotWorldStateIntro, row ? int64_t{row->intro} : -1);
            setMusicSlotZoneMusic(zm::kSlotWorldState, row ? int64_t{row->music} : -1);
        }
    }

    // Slot 8, the weather's (0x00526ade): the Weather.dbc row's AmbienceID
    // (+4), where it has one, when SMSG_WEATHER names it.
    if (ctx.weatherSerial != weatherSerialSeen_ && assets && ambience) {
        weatherSerialSeen_ = ctx.weatherSerial;
        int32_t sound = -1;
        auto dbc = assets->loadDBCOptional("Weather.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() > 1 ? dbc->findRecordById(ctx.weatherId)
                                                                                : -1;
        if (idx >= 0) sound = dbc->getInt32(static_cast<uint32_t>(idx), 1);
        ambience->setSlotSound(za::kSlotWeather, sound > 0 ? sound : -1);
    }

    // Slot 9, under liquid (0x004c8630), and slot 10, the screen effect's
    // ambience and music (0x004c8fa0).
    if (ambience) ambience->setCameraLiquid(ctx.cameraLiquidType);
    if (ctx.screenEffectAmbienceId != screenEffectAmbienceId_) {
        screenEffectAmbienceId_ = ctx.screenEffectAmbienceId;
        if (ambience) ambience->setSlotAmbience(za::kSlotScreenEffect, screenEffectAmbienceId_);
    }
    if (ctx.screenEffectZoneMusicId != screenEffectZoneMusicId_) {
        screenEffectZoneMusicId_ = ctx.screenEffectZoneMusicId;
        setMusicSlotZoneMusic(zm::kSlotScreenEffect, screenEffectZoneMusicId_);
    }

    if (!ambience) return;
    const bool indoors = ambiencePlace_ && ambiencePlace_->indoors;
    ambience->update(screen_effect_audio::isSoundDaytime(ctx.gameTimeHours), indoors);
    ambience->updateLiquid(ctx.deltaTime, ctx.playerPosition, indoors, ctx.liquidQuery);
}

void AudioCoordinator::updateMusic(const ZoneAudioContext& ctx) {
    namespace zm = zone_music;
    if (!musicManager_) return;
    auto& music = *musicManager_;
    music.update(ctx.deltaTime);

    // What each track's end does (0x004c5d20's notes).
    for (const auto& end : music.takeEndedZoneTracks()) {
        switch (zm::onEnd(end.slot)) {
        case zm::OnEnd::Silence: {
            // 0x004c8ab0: the silence, if the slots still pick the sound.
            const zm::Pick now = zm::pick(musicSlots_, music.zoneChannel(), musicContext());
            silenceUntil_ = zm::silenceAfter(end.sound, end.silenceMin, end.silenceMax, now.sound, nowMs(),
                                             static_cast<uint32_t>(musicRng_()));
            break;
        }
        case zm::OnEnd::EmptyServerSlot:
            musicSlots_[zm::kSlotServer] = {};  // 0x004c90c0
            break;
        case zm::OnEnd::IntroDelay:
            introDue_[end.sound] = zm::introDueAfter(end.silenceMin, nowMs());  // 0x004c9080
            break;
        case zm::OnEnd::None:
            break;
        }
    }

    // 0x004c9910: the channel given what the slots pick, unless it plays it.
    const zm::Channel ch = music.zoneChannel();
    const zm::Pick want = zm::pick(musicSlots_, ch, musicContext());
    if (!zm::shouldStart(want, ch, scriptMusic_.held)) return;
    if (want.slot >= zm::kSlotCount) {
        music.releaseZoneTrack(want.slot);
        return;
    }
    music.playZoneTrack(want.sound, want.slot, zm::fadeInSeconds(ch), zm::kFadeSeconds,
                        {want.slot, want.sound, want.silenceMin, want.silenceMax});
}

void AudioCoordinator::updateZoneAudio(const ZoneAudioContext& ctx) {
    cachedAssets_ = ctx.assetManager;
    musicTime_ = zone_music::timeIndex(screen_effect_audio::isSoundDaytime(ctx.gameTimeHours));
    updateAmbience(ctx);
    updateMusic(ctx);
}

} // namespace audio
} // namespace wowee

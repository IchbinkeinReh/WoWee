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
#include "game/zone_manager.hpp"
#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"

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
    // 0x004c93b0: the zone music's channels at once (0x004c6450(1) on
    // 0xb4ac90, no fade) and the script music SMSG_PLAY_MUSIC and PlayMusic
    // start (0xb4ac3c) over half a second (0x004c6390 with 0.5, seconds by
    // 0x0087a2f0), with its slot (3) cleared. Here they share the one music
    // manager, so whichever is playing stops its own way.
    if (musicManager_) musicManager_->stopMusic(musicManager_->isScriptMusic() ? 500.0f : 0.0f);
    // 0x004c85f0: the ambience's channels (0xb4ac50).
    if (ambientSoundManager_) ambientSoundManager_->stopAll();
    // The slots the next world fills again.
    screenEffectSounds_ = {};
    screenEffectMusicPlaying_.clear();
    ambiencePlace_.reset();
    worldStateAmbience_.reset();
    screenEffectAmbienceId_ = 0;
    currentZoneId_ = 0;
    currentZoneName_.clear();
    inTavern_ = false;
    inBlacksmith_ = false;
    musicSwitchCooldown_ = 0.0f;
    LOG_INFO("World unloaded: music and ambience stopped");
}

void AudioCoordinator::playZoneMusic(const std::string& music) {
    if (music.empty() || !musicManager_) return;
    if (music.rfind("file:", 0) == 0) {
        musicManager_->crossfadeToFile(music.substr(5));
    } else {
        musicManager_->crossfadeTo(music);
    }
}

void AudioCoordinator::setZoneMusicLooping(bool loop) {
    if (musicManager_) musicManager_->setLooping(loop);
}

void AudioCoordinator::onOriginalSoundtrackDisabled(game::ZoneManager* zm) {
    if (!zm || !musicManager_) return;
    if (!musicManager_->isCurrentTrackFile()) return;
    if (!musicManager_->isPlaying() && !musicManager_->isLoading()) return;
    // Only act in-world with a known zone; the login screen intentionally
    // plays a file track and is not part of the zone rotation this setting
    // controls.
    if (currentZoneId_ == 0) return;

    std::string music = zm->getRandomMusic(currentZoneId_);
    if (!music.empty() && music.rfind("file:", 0) != 0) {
        playZoneMusic(music);
        musicSwitchCooldown_ = 6.0f;
    } else {
        musicManager_->stopMusic();
    }
}

namespace {

// The file a SoundEntries row plays: its DirectoryBase (23) and the first of
// its ten File columns (3-12) that is set.
std::string soundEntryFile(pipeline::AssetManager& assets, uint32_t soundId) {
    if (soundId == 0) return {};
    auto dbc = assets.loadDBCOptional("SoundEntries.dbc");
    const int32_t idx = dbc && dbc->getFieldCount() >= 24 ? dbc->findRecordById(soundId) : -1;
    if (idx < 0) return {};
    const auto row = static_cast<uint32_t>(idx);
    const std::string dir = dbc->getString(row, 23);
    for (uint32_t f = 3; f <= 12; ++f) {
        const std::string name = dbc->getString(row, f);
        if (!name.empty()) return dir.empty() ? name : dir + "\\" + name;
    }
    return {};
}

// A SoundAmbience or ZoneMusic row's sound for the time.
std::string daySoundFile(pipeline::AssetManager& assets, const char* table, uint32_t id, uint32_t dayCol,
                         uint32_t nightCol, bool isDay) {
    if (id == 0) return {};
    auto dbc = assets.loadDBCOptional(table);
    const int32_t idx = dbc && dbc->getFieldCount() > nightCol ? dbc->findRecordById(id) : -1;
    if (idx < 0) return {};
    const auto row = static_cast<uint32_t>(idx);
    return soundEntryFile(assets, screen_effect_audio::dayOrNight(dbc->getUInt32(row, dayCol),
                                                                  dbc->getUInt32(row, nightCol), isDay));
}

}  // namespace

bool AudioCoordinator::updateScreenEffectAudio(const ZoneAudioContext& ctx) {
    namespace sea = screen_effect_audio;
    const bool isDay = sea::isSoundDaytime(ctx.gameTimeHours);
    ScreenEffectSounds& want = screenEffectSounds_;
    if (ctx.assetManager && (want.zoneMusicId != ctx.screenEffectZoneMusicId || want.isDay != isDay)) {
        want.zoneMusicId = ctx.screenEffectZoneMusicId;
        want.isDay = isDay;
        want.music = daySoundFile(*ctx.assetManager, "ZoneMusic.dbc", want.zoneMusicId, sea::kZoneMusicDayCol,
                                  sea::kZoneMusicNightCol, isDay);
    }

    // The music: at once, and again whenever it ends.
    if (!musicManager_) return false;
    if (!want.music.empty()) {
        if (want.music != screenEffectMusicPlaying_ || (!musicManager_->isPlaying() && !musicManager_->isLoading())) {
            playZoneMusic(want.music);
            screenEffectMusicPlaying_ = want.music;
        }
        musicManager_->update(ctx.deltaTime);
        return true;
    }
    if (!screenEffectMusicPlaying_.empty()) {
        // The slot emptied: the zone's music again.
        screenEffectMusicPlaying_.clear();
        musicManager_->stopMusic();
        musicSwitchCooldown_ = 0.0f;
    }
    return false;
}

void AudioCoordinator::updateAmbience(const ZoneAudioContext& ctx) {
    namespace za = zone_ambience;
    if (!ambientSoundManager_) return;
    auto& ambience = *ambientSoundManager_;
    auto* assets = ctx.assetManager;

    // Slot 0, the place (0x0078e9a0): its AreaTable rows' SoundAmbience
    // (+0x1c) and its building's.
    if (ctx.place && assets && ctx.place != ambiencePlace_) {
        ambiencePlace_ = ctx.place;
        const za::Place& at = *ctx.place;
        uint32_t zoneAmbience = 0, areaAmbience = 0;
        const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("AreaTable")
                                                            : nullptr;
        const uint32_t col = layout ? layout->tryField("SoundAmbience") : 0xFFFFFFFFu;
        auto areas = assets->loadDBCOptional("AreaTable.dbc");
        const auto rowAmbience = [&](uint32_t id) -> uint32_t {
            if (id == 0 || !areas || !areas->isLoaded() || col >= areas->getFieldCount()) return 0;
            const int32_t idx = areas->findRecordById(id);
            return idx >= 0 ? areas->getUInt32(static_cast<uint32_t>(idx), col) : 0;
        };
        zoneAmbience = rowAmbience(at.zone);
        areaAmbience = rowAmbience(at.area);
        ambience.setSlotAmbience(za::kSlotPlace, za::placeAmbience(zoneAmbience, areaAmbience, at.buildingAmbience,
                                                                   at.groupAmbience, at.indoors));
    }

    // Slot 1, the world states' (0x004cbe70): WorldStateZoneSounds.dbc, 0
    // the world state, 1 the value, 2 the area, 3 the WMOAreaTable row, 6
    // the SoundAmbience.
    if (!worldStateSoundsLoaded_ && assets) {
        worldStateSoundsLoaded_ = true;
        auto dbc = assets->loadDBCOptional("WorldStateZoneSounds.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 8) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                worldStateSounds_.push_back({dbc->getUInt32(i, 0), dbc->getUInt32(i, 1), dbc->getUInt32(i, 2),
                                             dbc->getUInt32(i, 3), dbc->getUInt32(i, 6)});
        }
    }
    if (ambiencePlace_) {
        const auto value = [&](uint32_t state) -> uint32_t {
            if (!ctx.worldStates) return 0;
            const auto it = ctx.worldStates->find(state);
            return it != ctx.worldStates->end() ? it->second : 0;
        };
        const std::optional<uint32_t> row = za::worldStateAmbience(worldStateSounds_, *ambiencePlace_, value);
        if (row != worldStateAmbience_) {
            worldStateAmbience_ = row;
            ambience.setSlotAmbience(za::kSlotWorldState, row.value_or(0));
        }
    }

    // Slot 8, the weather's (0x00526ade): the Weather.dbc row's AmbienceID
    // (+4), where it has one, when SMSG_WEATHER names it.
    if (ctx.weatherSerial != weatherSerialSeen_ && assets) {
        weatherSerialSeen_ = ctx.weatherSerial;
        int32_t sound = -1;
        auto dbc = assets->loadDBCOptional("Weather.dbc");
        const int32_t idx = dbc && dbc->isLoaded() && dbc->getFieldCount() > 1 ? dbc->findRecordById(ctx.weatherId)
                                                                                : -1;
        if (idx >= 0) sound = dbc->getInt32(static_cast<uint32_t>(idx), 1);
        ambience.setSlotSound(za::kSlotWeather, sound > 0 ? sound : -1);
    }

    // Slot 9, under liquid (0x004c8630), and slot 10, the screen effect's
    // (0x004c8fa0).
    ambience.setCameraLiquid(ctx.cameraLiquidType);
    if (ctx.screenEffectAmbienceId != screenEffectAmbienceId_) {
        screenEffectAmbienceId_ = ctx.screenEffectAmbienceId;
        ambience.setSlotAmbience(za::kSlotScreenEffect, screenEffectAmbienceId_);
    }

    const bool indoors = ambiencePlace_ && ambiencePlace_->indoors;
    ambience.update(screen_effect_audio::isSoundDaytime(ctx.gameTimeHours), indoors);
    ambience.updateLiquid(ctx.deltaTime, ctx.playerPosition, indoors, ctx.liquidQuery);
}

void AudioCoordinator::updateZoneAudio(const ZoneAudioContext& ctx) {
    float deltaTime = ctx.deltaTime;
    if (musicSwitchCooldown_ > 0.0f) {
        musicSwitchCooldown_ = std::max(0.0f, musicSwitchCooldown_ - deltaTime);
    }

    updateAmbience(ctx);

    // The zone the music is for.
    auto* zm = ctx.zoneManager;
    const uint32_t tileZoneId = (zm && ctx.hasTile)
        ? zm->getZoneId(ctx.tileX, ctx.tileY)
        : 0;
    const uint32_t serverZoneId = (zm && ctx.serverZoneId != 0)
        ? zm->resolveAreaZoneId(ctx.serverZoneId)
        : ctx.serverZoneId;
    uint32_t zoneId = serverZoneId != 0 ? serverZoneId : tileZoneId;

    // The screen effect's slot is above all the zone's.
    if (updateScreenEffectAudio(ctx)) return;

    // ── Zone detection and music transitions ──
    if (!zm || !musicManager_ || !ctx.hasTile) return;

    bool insideTavern = false;
    bool insideBlacksmith = false;
    std::string tavernMusic;

    // WMO-based location overrides (taverns, blacksmiths, city zones)
    if (ctx.insideWmo) {
        uint32_t wmoModelId = ctx.insideWmoId;

        // Stormwind WMO → force Stormwind City zone
        if (wmoModelId == 10047) zoneId = 1519;

        // Log WMO transitions
        static uint32_t lastLoggedWmoId = 0;
        if (wmoModelId != lastLoggedWmoId) {
            LOG_INFO("Inside WMO model ID: ", wmoModelId);
            lastLoggedWmoId = wmoModelId;
        }

        // Blacksmith detection (the music stops inside)
        if (wmoModelId == 96048) {
            insideBlacksmith = true;
            LOG_INFO("Detected blacksmith WMO ", wmoModelId);
        }

        // Tavern / inn detection
        if (wmoModelId == 191 || wmoModelId == 71414 || wmoModelId == 190 ||
            wmoModelId == 220 || wmoModelId == 221 ||
            wmoModelId == 5392 || wmoModelId == 5393) {
            insideTavern = true;
            // The first slot is our own tavern track. It falls back to the
            // Blizzard one it replaces when the file is missing, and the
            // original-soundtrack setting drops it the same way zone music does.
            static const std::string tavernRemix =
                game::ZoneManager::resolveOriginalMusicFile("TavernAllianceREMIX.mp3");
            const bool useRemix = !tavernRemix.empty() &&
                (!ctx.zoneManager || ctx.zoneManager->getUseOriginalSoundtrack());
            const std::vector<std::string> tavernTracks = {
                useRemix ? tavernRemix
                         : std::string("Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3"),
                "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
            };
            static int tavernTrackIndex = 0;
            tavernMusic = tavernTracks[tavernTrackIndex++ % tavernTracks.size()];
            LOG_INFO("Detected tavern WMO ", wmoModelId, ", playing: ", tavernMusic);
        }
    }

    // Tavern music transitions
    if (insideTavern) {
        if (!inTavern_ && !tavernMusic.empty()) {
            inTavern_ = true;
            LOG_INFO("Entered tavern");
            musicManager_->playMusic(tavernMusic, true);
            musicSwitchCooldown_ = 6.0f;
        }
    } else if (inTavern_) {
        inTavern_ = false;
        LOG_INFO("Exited tavern");
        auto* info = zm->getZoneInfo(currentZoneId_);
        if (info) {
            std::string music = zm->getRandomMusic(currentZoneId_);
            if (!music.empty()) {
                playZoneMusic(music);
                musicSwitchCooldown_ = 6.0f;
            }
        }
    }

    // Blacksmith transitions (stop music, let ambience play)
    if (insideBlacksmith) {
        if (!inBlacksmith_) {
            inBlacksmith_ = true;
            LOG_INFO("Entered blacksmith - stopping music");
            musicManager_->stopMusic();
        }
    } else if (inBlacksmith_) {
        inBlacksmith_ = false;
        LOG_INFO("Exited blacksmith - restoring music");
        auto* info = zm->getZoneInfo(currentZoneId_);
        if (info) {
            std::string music = zm->getRandomMusic(currentZoneId_);
            if (!music.empty()) {
                playZoneMusic(music);
                musicSwitchCooldown_ = 6.0f;
            }
        }
    }

    // Normal zone transitions
    if (!insideTavern && !insideBlacksmith && zoneId != currentZoneId_ && zoneId != 0) {
        currentZoneId_ = zoneId;
        auto* info = zm->getZoneInfo(zoneId);
        if (info) {
            currentZoneName_ = info->name;
            LOG_INFO("Entered zone: ", info->name);
            if (musicSwitchCooldown_ <= 0.0f) {
                std::string music = zm->getRandomMusic(zoneId);
                if (!music.empty()) {
                    playZoneMusic(music);
                    musicSwitchCooldown_ = 6.0f;
                }
            }
        }
    }

    musicManager_->update(deltaTime);

    // When a track finishes, pick a new random track from the current zone
    if (!musicManager_->isPlaying() && !inTavern_ && !inBlacksmith_ &&
        currentZoneId_ != 0 && musicSwitchCooldown_ <= 0.0f) {
        std::string music = zm->getRandomMusic(currentZoneId_);
        if (!music.empty()) {
            playZoneMusic(music);
            musicSwitchCooldown_ = 2.0f;
        }
    }
}

} // namespace audio
} // namespace wowee

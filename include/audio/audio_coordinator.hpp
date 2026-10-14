#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <random>
#include <glm/vec3.hpp>
#include "audio/zone_ambience.hpp"
#include "audio/zone_music.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

class MusicManager;
class FootstepManager;
class ActivitySoundManager;
class MountSoundManager;
class NpcVoiceManager;
class PlayerVoiceManager;
class AmbientSoundManager;
class UiSoundManager;
class CombatSoundManager;
class SpellSoundManager;
class MovementSoundManager;

/// Flat context passed from Renderer into updateZoneAudio() each frame.
/// All values are pre-queried so AudioCoordinator needs no rendering pointers.
struct ZoneAudioContext {
    float deltaTime = 0.0f;
    // The game clock (hours in [0, 24)): the sound day (0x004c9850).
    float gameTimeHours = 12.0f;
    // Where the player is for the ambience (0x0078f020), none until known.
    std::optional<zone_ambience::Place> place;
    glm::vec3 playerPosition{0.0f};
    // The Weather.dbc row SMSG_WEATHER last named, and a count of them.
    uint32_t weatherId = 0;
    uint32_t weatherSerial = 0;
    // The liquid the camera is in, 0 for none (0x00780620).
    uint32_t cameraLiquidType = 0;
    // The liquid cells about a point (0x0077f390).
    zone_ambience::LiquidQuery liquidQuery;
    // The world states the server has sent.
    const std::unordered_map<uint32_t, uint32_t>* worldStates = nullptr;
    // The screen effect's SoundAmbience and ZoneMusic rows, 0 for none
    // (ScreenEffect.dbc +0x20, +0x24; 0x004f7020).
    uint32_t screenEffectAmbienceId = 0;
    uint32_t screenEffectZoneMusicId = 0;
    pipeline::AssetManager* assetManager = nullptr;
};

/// Coordinates all audio subsystems.
/// Extracted from Renderer to separate audio lifecycle from rendering.
/// Owned by Application; Renderer and UI components access through Application.
class AudioCoordinator {
public:
    AudioCoordinator();
    ~AudioCoordinator();

    /// Initialize the audio engine and all managers.
    /// @return true if audio is available (engine initialized successfully)
    [[nodiscard]] bool initialize();


    /// Shutdown all audio managers and engine.
    void shutdown();

    /// The world unloads - a logout, the client quitting from the world - or
    /// a loading screen ends: a map change (SMSG_NEW_WORLD -> 0x006e7f50), a
    /// teleport of more than 150 yards on the same map (0x006ecf80), or a far
    /// clip raised by more than 10 yards (0x007831a0). An instance difficulty
    /// changed (SMSG_CHANGEPLAYER_DIFFICULTY_RESULT 9, 0x0052c460) does it too.
    ///
    /// What the client's unload (0x00409550) stops: the music channel's two
    /// tracks at once, with SMSG_PLAY_MUSIC's slot emptied and the silence
    /// between tracks ended (0x004c93b0), and the ambience's channels
    /// (0x004c85f0). The place they were for is forgotten too, so the next
    /// world fills the slots afresh.
    void leaveWorld();
    /// Entering the world from the character screen (0x00405540 ->
    /// 0x004c9330): every intro may play again at once.
    void forgetIntroMusic();

    /// SMSG_PLAY_MUSIC (0x277 -> 0x004c92b0): the sound goes to the music's
    /// slot 3, above the zone's, until it ends.
    void playServerMusic(uint32_t soundId);
    /// The script's PlayMusic (0x004c9110(file, 5)): unless Sound_EnableMusic
    /// is off, the file loops on its own track (0xb4ac44); once it plays the
    /// zone music falls over its own fall (0x004c6450(0)) and none starts
    /// (0xb4aaa4) until StopMusic.
    void playScriptMusic(const std::string& path);
    /// StopMusic (0x004c8680): the script's file falls over half a second
    /// and the zone music may start again.
    void stopScriptMusic();
    [[nodiscard]] bool scriptMusicPlaying() const { return scriptMusic_.held; }
    /// Sound_EnableMusic (0x004d1590): off stops the music at once as the
    /// unload does, and the script's as StopMusic does; the slots pick
    /// nothing while it is off.
    void setMusicEnabled(bool enabled);
    /// Sound_ZoneMusicNoDelay, the options' Loop Music: no silence between
    /// the zone's tracks (0x004c87d0).
    void setZoneMusicNoDelay(bool noDelay) { musicNoDelay_ = noDelay; }

    /// Per-frame sound slots: the ambience and the music.
    /// Called from Renderer::update() with a pre-filled context.
    void updateZoneAudio(const ZoneAudioContext& ctx);

    // Accessors for all audio managers (same interface as Renderer had)
    MusicManager* getMusicManager() { return musicManager_.get(); }
    FootstepManager* getFootstepManager() { return footstepManager_.get(); }
    ActivitySoundManager* getActivitySoundManager() { return activitySoundManager_.get(); }
    MountSoundManager* getMountSoundManager() { return mountSoundManager_.get(); }
    NpcVoiceManager* getNpcVoiceManager() { return npcVoiceManager_.get(); }
    PlayerVoiceManager* getPlayerVoiceManager() { return playerVoiceManager_.get(); }
    AmbientSoundManager* getAmbientSoundManager() { return ambientSoundManager_.get(); }
    UiSoundManager* getUiSoundManager() { return uiSoundManager_.get(); }
    CombatSoundManager* getCombatSoundManager() { return combatSoundManager_.get(); }
    SpellSoundManager* getSpellSoundManager() { return spellSoundManager_.get(); }
    MovementSoundManager* getMovementSoundManager() { return movementSoundManager_.get(); }

private:
    /// The slots from the place, the world states, the weather, the camera's
    /// liquid and the screen effect, and the ambience and the liquid's sound.
    void updateAmbience(const ZoneAudioContext& ctx);
    /// The music channel, once a frame (0x004c9910), and the ends of its
    /// tracks.
    void updateMusic(const ZoneAudioContext& ctx);
    /// 0x004c8d80: a music slot from a ZoneMusic row (-1 or 0 for none).
    void setMusicSlotZoneMusic(int slot, int64_t zoneMusicId);
    /// 0x004c8cf0: an intro slot from a ZoneIntroMusicTable row.
    void setMusicSlotIntro(int slot, int64_t introId);
    [[nodiscard]] zone_music::Context musicContext() const;
    [[nodiscard]] uint64_t nowMs() const;

    std::unique_ptr<MusicManager> musicManager_;
    std::unique_ptr<FootstepManager> footstepManager_;
    std::unique_ptr<ActivitySoundManager> activitySoundManager_;
    std::unique_ptr<MountSoundManager> mountSoundManager_;
    std::unique_ptr<NpcVoiceManager> npcVoiceManager_;
    std::unique_ptr<PlayerVoiceManager> playerVoiceManager_;
    std::unique_ptr<AmbientSoundManager> ambientSoundManager_;
    std::unique_ptr<UiSoundManager> uiSoundManager_;
    std::unique_ptr<CombatSoundManager> combatSoundManager_;
    std::unique_ptr<SpellSoundManager> spellSoundManager_;
    std::unique_ptr<MovementSoundManager> movementSoundManager_;

    bool audioAvailable_ = false;

    // What the slots were last filled from.
    std::optional<zone_ambience::Place> ambiencePlace_;
    std::optional<zone_ambience::WorldStateSound> worldStateRow_;
    std::vector<zone_ambience::WorldStateSound> worldStateSounds_;
    bool worldStateSoundsLoaded_ = false;
    uint32_t weatherSerialSeen_ = 0;
    uint32_t screenEffectAmbienceId_ = 0;
    uint32_t screenEffectZoneMusicId_ = 0;

    // The music's slots (0xb4aab0) and what the pick reads besides.
    zone_music::Slots musicSlots_{};
    int musicTime_ = 0;              ///< 0x00ac3878
    bool musicEnabled_ = true;       ///< Sound_EnableMusic
    bool musicNoDelay_ = false;      ///< Sound_ZoneMusicNoDelay
    zone_music::ScriptMusicHold scriptMusic_;  ///< 0xb4aaa4
    uint64_t silenceUntil_ = 0;      ///< 0xb4aaa0
    pipeline::AssetManager* cachedAssets_ = nullptr;
    std::unordered_map<int32_t, uint64_t> introDue_;  ///< 0xb4acd8
    std::mt19937 musicRng_{std::random_device{}()};
};

} // namespace audio
} // namespace wowee

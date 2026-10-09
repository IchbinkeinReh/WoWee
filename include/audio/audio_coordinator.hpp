#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <glm/vec3.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game { class ZoneManager; }
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
    glm::vec3 cameraPosition{0.0f};
    bool isSwimming = false;
    bool insideWmo = false;
    uint32_t insideWmoId = 0;
    // Visual weather state for ambient audio sync
    int weatherType = 0;      // 0=none, 1=rain, 2=snow, 3=storm
    float weatherIntensity = 0.0f;
    // Visible sky clock after zone ambience overrides (hours in [0, 24)).
    float gameTimeHours = 12.0f;
    // Terrain tile for offline zone lookup
    int tileX = 0, tileY = 0;
    bool hasTile = false;
    // Server-authoritative zone (from SMSG_INIT_WORLD_STATES); 0 = offline
    uint32_t serverZoneId = 0;
    // Zone manager pointer (for zone info and music queries)
    game::ZoneManager* zoneManager = nullptr;
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
    /// Loop Music, from the audio options. Takes effect on the next
    /// track; a track already running is not restarted to obey it.
    void setZoneMusicLooping(bool loop);

    AudioCoordinator();
    ~AudioCoordinator();

    /// Initialize the audio engine and all managers.
    /// @return true if audio is available (engine initialized successfully)
    [[nodiscard]] bool initialize();


    /// Shutdown all audio managers and engine.
    void shutdown();

    /// Per-frame zone detection, music transitions, and ambient weather sync.
    /// Called from Renderer::update() with a pre-filled context.
    void updateZoneAudio(const ZoneAudioContext& ctx);

    [[nodiscard]] const std::string& getCurrentZoneName() const { return currentZoneName_; }
    [[nodiscard]] uint32_t getCurrentZoneId() const { return currentZoneId_; }

    /// Called when the "Enable WoWee Music" setting is turned off. If one of
    /// the WoWee (file-based) tracks is currently playing, crossfade to a
    /// stock track so the disable takes effect immediately instead of waiting
    /// for the track to end.
    void onOriginalSoundtrackDisabled(game::ZoneManager* zm);

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
    void playZoneMusic(const std::string& music);
    /// The screen effect's slot (0x004c8fa0, slot 10): its ambience and
    /// music over the zone's. True while its music plays.
    bool updateScreenEffectAudio(const ZoneAudioContext& ctx);

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

    // Zone/music state - moved from Renderer
    uint32_t currentZoneId_ = 0;
    std::string currentZoneName_;
    bool inTavern_ = false;
    bool inBlacksmith_ = false;
    float musicSwitchCooldown_ = 0.0f;
    // The screen effect's sounds as last resolved, and its ambience's loop.
    struct ScreenEffectSounds {
        uint32_t ambienceId = 0, zoneMusicId = 0;
        bool isDay = true;
        std::string ambience, music;
    };
    ScreenEffectSounds screenEffectSounds_;
    std::string screenEffectAmbiencePlaying_;
    uint32_t screenEffectAmbienceHandle_ = 0;
    std::string screenEffectMusicPlaying_;
};

} // namespace audio
} // namespace wowee

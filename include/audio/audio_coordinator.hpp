#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/vec3.hpp>
#include "audio/zone_ambience.hpp"

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

    /// The world unloads - a logout, the client quitting from the world - or
    /// a loading screen ends: a map change (SMSG_NEW_WORLD -> 0x006e7f50), a
    /// teleport of more than 150 yards on the same map (0x006ecf80), or a far
    /// clip raised by more than 10 yards (0x007831a0). An instance difficulty
    /// changed (SMSG_CHANGEPLAYER_DIFFICULTY_RESULT 9, 0x0052c460) does it too.
    ///
    /// What the client's unload (0x00409550) stops: the zone music's two
    /// channels at once and the script music over half a second (0x004c93b0),
    /// and the ambience's channels (0x004c85f0). The zone they were for is
    /// forgotten too, so the next world entry starts its music afresh.
    void leaveWorld();

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
    /// The ambience's slots from the place, the world states, the weather
    /// and the camera's liquid, and the ambience and the liquid's sound.
    void updateAmbience(const ZoneAudioContext& ctx);

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
    // The screen effect's sounds as last resolved.
    struct ScreenEffectSounds {
        uint32_t zoneMusicId = 0;
        bool isDay = true;
        std::string music;
    };
    ScreenEffectSounds screenEffectSounds_;
    std::string screenEffectMusicPlaying_;
    // What the ambience's slots were last filled from.
    std::optional<zone_ambience::Place> ambiencePlace_;
    std::optional<uint32_t> worldStateAmbience_;
    std::vector<zone_ambience::WorldStateSound> worldStateSounds_;
    bool worldStateSoundsLoaded_ = false;
    uint32_t weatherSerialSeen_ = 0;
    uint32_t screenEffectAmbienceId_ = 0;
};

} // namespace audio
} // namespace wowee

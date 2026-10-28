#pragma once

#include "audio/footstep_manager.hpp"
#include "platform/process.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

class ActivitySoundManager {
public:
    ActivitySoundManager();
    ~ActivitySoundManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    void update(float deltaTime);
    [[nodiscard]] bool isInitialized() const { return initialized; }

    void playJump();
    void playLanding(FootstepSurface surface, bool hardLanding);
    void setSwimmingState(bool swimming, bool moving);
    void setCharacterVoiceProfile(const std::string& modelName);
    void setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void playAttackGrunt();
    void playWound(bool isCrit = false);
    void setVolumeScale(float scale) { volumeScale = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale; }

private:
    // A clip here is picked by archive path, never by a SoundEntries id, so the
    // row that names it is found by leaf file name. rowVolume < 0 means no row
    // names this file at all and the play keeps its own invented level.
    struct Sample {
        std::string path;
        std::vector<uint8_t> data;
        float rowVolume = -1.0f;
        uint32_t flags = 0;
    };

    struct SurfaceLandingSet {
        std::vector<Sample> clips;
    };

    bool initialized = false;
    pipeline::AssetManager* assetManager = nullptr;

    std::vector<Sample> jumpClips;
    std::vector<Sample> swimLoopClips;
    std::vector<Sample> hardLandClips;
    std::vector<Sample> attackGruntClips;
    std::vector<Sample> woundClips;
    std::vector<Sample> woundCritClips;
    std::vector<Sample> deathClips;
    std::array<SurfaceLandingSet, 7> landingSets;

    bool swimmingActive = false;
    bool swimMoving = false;
    ProcessHandle swimLoopPid = INVALID_PROCESS;
    std::string loopTempPath = platform::getTempFilePath("wowee_swim_loop.wav");
    std::mt19937 rng;

    std::chrono::steady_clock::time_point lastJumpAt{};
    std::chrono::steady_clock::time_point lastLandAt{};
    std::chrono::steady_clock::time_point lastAttackGruntAt{};
    std::chrono::steady_clock::time_point lastWoundAt{};
    std::chrono::steady_clock::time_point lastSwimStrokeAt{};
    std::string voiceProfileKey;
    float volumeScale = 1.0f;

    // What a SoundEntries row asks of a play, indexed by lowered leaf file name.
    struct RowAsk {
        float volume = 1.0f;
        uint32_t flags = 0;
    };
    std::unordered_map<std::string, RowAsk> rowsByLeaf;

    void indexRowAsks();
    void askedByRow(const Sample& sample, float own, float& volume, float& pitch);

    void preloadCandidates(std::vector<Sample>& out, const std::vector<std::string>& candidates);
    void preloadLandingSet(FootstepSurface surface, const std::string& material);
    void rebuildJumpClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildSwimLoopClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildHardLandClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void rebuildCombatVocalClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male);
    void startSwimLoop();
    void stopSwimLoop();
    void stopOneShot();
    void reapProcesses();
};

} // namespace audio
} // namespace wowee

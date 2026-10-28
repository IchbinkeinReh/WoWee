#pragma once

#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <random>
#include <chrono>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

struct VoiceSample {
    std::string path;
    std::vector<uint8_t> data;
    /// What the SoundEntries row naming this clip asks of a 3D play: its
    /// MinDistance (26) and DistanceCutoff (27), 0 when no row names it. Every
    /// one of the 525 rows NPCSounds points at names a reach - 8 yards of full
    /// volume inside a 45-yard carry, and none wider than that - so a line is
    /// heard where its own row says, not at a guessed radius.
    float minDistance = 0.0f;
    float cutoff = 0.0f;
};

// NPC voice types (based on creature model/gender)
enum class VoiceType {
    HUMAN_MALE,
    HUMAN_FEMALE,
    DWARF_MALE,
    DWARF_FEMALE,
    NIGHTELF_MALE,
    NIGHTELF_FEMALE,
    ORC_MALE,
    ORC_FEMALE,
    TAUREN_MALE,
    TAUREN_FEMALE,
    TROLL_MALE,
    TROLL_FEMALE,
    UNDEAD_MALE,
    UNDEAD_FEMALE,
    GNOME_MALE,
    GNOME_FEMALE,
    GOBLIN_MALE,
    GOBLIN_FEMALE,
    BLOODELF_MALE,
    BLOODELF_FEMALE,
    DRAENEI_MALE,
    DRAENEI_FEMALE,
    GENERIC,  // Fallback
};

class NpcVoiceManager {
public:
    NpcVoiceManager();
    ~NpcVoiceManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // Play NPC interaction sounds
    void playGreeting(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position);
    void playFarewell(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position);
    void playVendor(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position);
    void playPissed(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position);

    void setVolumeScale(float scale) { volumeScale_ = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

private:
    enum class SoundCategory {
        GREETING,
        FAREWELL,
        VENDOR,
        PISSED,
        FLEE
    };

    void loadVoiceSounds();
    /// Every file the SoundEntries rows name, once, with that row's MinDistance
    /// (26) and DistanceCutoff (27) - so a clip carries the reach its own row
    /// asks for instead of one guessed here.
    void indexSoundEntriesReach();
    bool loadSound(const std::string& path, VoiceSample& sample);
    void playSound(uint64_t npcGuid, VoiceType voiceType, SoundCategory category, const glm::vec3& position);

    pipeline::AssetManager* assetManager_ = nullptr;
    float volumeScale_ = 1.0f;

    // Voice samples grouped by type and category
    std::unordered_map<VoiceType, std::vector<VoiceSample>> greetingLibrary_;
    std::unordered_map<VoiceType, std::vector<VoiceSample>> farewellLibrary_;
    std::unordered_map<VoiceType, std::vector<VoiceSample>> vendorLibrary_;
    std::unordered_map<VoiceType, std::vector<VoiceSample>> pissedLibrary_;
    std::unordered_map<VoiceType, std::vector<VoiceSample>> fleeLibrary_;

    // A clip's reach, by the lower-case file name SoundEntries names it by.
    std::unordered_map<std::string, std::pair<float, float>> reachByName_;

    // Cooldown tracking (prevent spam clicking same NPC)
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastPlayTime_;
    std::unordered_map<uint64_t, int> clickCount_;  // Track clicks for pissed sounds
    static constexpr float GREETING_COOLDOWN = 2.0f;  // seconds
    static constexpr int PISSED_CLICK_THRESHOLD = 5;  // clicks before pissed

    std::mt19937 rng_;
};

} // namespace audio
} // namespace wowee

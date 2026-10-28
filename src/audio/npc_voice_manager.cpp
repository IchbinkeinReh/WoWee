#include "audio/npc_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_distance.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>

namespace wowee {
namespace audio {

namespace {

std::string leafLower(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return leaf;
}

}  // namespace

NpcVoiceManager::NpcVoiceManager() : rng_(std::random_device{}()) {}

NpcVoiceManager::~NpcVoiceManager() {
    shutdown();
}

bool NpcVoiceManager::initialize(pipeline::AssetManager* assets) {
    assetManager_ = assets;
    if (!assetManager_) {
        LOG_WARNING("NPC voice manager: no asset manager");
        return false;
    }

    // Files are .WAV not .OGG in WotLK 3.3.5a!
    LOG_INFO("=== Probing for NPC voice files (.wav format) ===");
    std::vector<std::string> testPaths = {
        "Sound\\Creature\\HumanMaleStandardNPC\\HumanMaleStandardNPCGreetings01.wav",
        "Sound\\Creature\\HumanFemaleStandardNPC\\HumanFemaleStandardNPCGreeting01.wav",
        "Sound\\Creature\\DwarfMaleStandardNPC\\DwarfMaleStandardNPCGreeting01.wav",
        "Sound\\Creature\\OrcMaleStandardNPC\\OrcMaleStandardNPCGreeting01.wav",
    };
    for (const auto& path : testPaths) {
        bool exists = assetManager_->fileExists(path);
        LOG_INFO("  ", path, ": ", (exists ? "EXISTS" : "NOT FOUND"));
    }
    LOG_INFO("===================================");

    loadVoiceSounds();

    int totalSamples = 0;
    for (const auto& [type, samples] : greetingLibrary_) totalSamples += samples.size();
    for (const auto& [type, samples] : farewellLibrary_) totalSamples += samples.size();
    for (const auto& [type, samples] : vendorLibrary_) totalSamples += samples.size();
    for (const auto& [type, samples] : pissedLibrary_) totalSamples += samples.size();
    for (const auto& [type, samples] : fleeLibrary_) totalSamples += samples.size();
    LOG_INFO("NPC voice manager initialized (", totalSamples, " voice clips)");
    return true;
}

void NpcVoiceManager::shutdown() {
    greetingLibrary_.clear();
    farewellLibrary_.clear();
    vendorLibrary_.clear();
    pissedLibrary_.clear();
    fleeLibrary_.clear();
    lastPlayTime_.clear();
    clickCount_.clear();
    reachByName_.clear();
    assetManager_ = nullptr;
}

// What a voice clip's own SoundEntries row asks of a 3D play. The rows name the
// files by name, so the reach is looked up by the file the library loaded - and
// every row NPCSounds points at names one: of the 525 rows behind its 301
// entries, all 525 name files, 504 ask a full-volume core of 8 yards, and none
// carries further than 45.
void NpcVoiceManager::indexSoundEntriesReach() {
    if (!assetManager_) return;
    auto dbc = assetManager_->loadDBCOptional("SoundEntries.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() <= 27) return;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const float minDistance = dbc->getFloat(i, 26);
        const float cutoff = dbc->getFloat(i, 27);
        for (uint32_t f = 0; f < 10; ++f) {
            const std::string file = dbc->getString(i, 3 + f);
            if (file.empty()) continue;
            reachByName_.try_emplace(leafLower(file), minDistance, cutoff);
        }
    }
}

void NpcVoiceManager::loadVoiceSounds() {
    if (!assetManager_) return;
    indexSoundEntriesReach();

    // Helper to load voice category for a race/gender
    auto loadCategory = [this](
        std::unordered_map<VoiceType, std::vector<VoiceSample>>& library,
        VoiceType type,
        const std::string& npcType,
        const std::string& soundType,
        int count) {

        auto& samples = library[type];
        for (int i = 1; i <= count; ++i) {
            std::string num = (i < 10) ? ("0" + std::to_string(i)) : std::to_string(i);
            std::string path = "Sound\\Creature\\" + npcType + "\\" + npcType + soundType + num + ".wav";
            VoiceSample sample;
            if (loadSound(path, sample)) samples.push_back(std::move(sample));
        }
    };

    // Generic fallback voices (male only)
    auto& genericGreet = greetingLibrary_[VoiceType::GENERIC];
    for (const auto& path : {
        "Sound\\Creature\\DwarfMaleStandardNPC\\DwarfMaleStandardNPCGreeting01.wav",
        "Sound\\Creature\\GnomeMaleStandardNPC\\GnomeMaleStandardNPCGreeting01.wav",
        "Sound\\Creature\\NightElfMaleStandardNPC\\NightElfMaleStandardNPCGreeting01.wav",
        "Sound\\Creature\\OrcMaleStandardNPC\\OrcMaleStandardNPCGreeting01.wav",
    }) {
        VoiceSample sample;
        if (loadSound(path, sample)) genericGreet.push_back(std::move(sample));
    }

    // Load all race/gender combinations
    // Human Male uses "Greetings" (plural), others use "Greeting" (singular)
    loadCategory(greetingLibrary_, VoiceType::HUMAN_MALE, "HumanMaleStandardNPC", "Greetings", 6);
    loadCategory(farewellLibrary_, VoiceType::HUMAN_MALE, "HumanMaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::HUMAN_MALE, "HumanMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::HUMAN_MALE, "HumanMaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::HUMAN_FEMALE, "HumanFemaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::HUMAN_FEMALE, "HumanFemaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::HUMAN_FEMALE, "HumanFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::HUMAN_FEMALE, "HumanFemaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::DWARF_MALE, "DwarfMaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::DWARF_MALE, "DwarfMaleStandardNPC", "Farewell", 4);
    loadCategory(vendorLibrary_, VoiceType::DWARF_MALE, "DwarfMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::DWARF_MALE, "DwarfMaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::GNOME_MALE, "GnomeMaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::GNOME_MALE, "GnomeMaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::GNOME_MALE, "GnomeMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::GNOME_MALE, "GnomeMaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::GNOME_FEMALE, "GnomeFemaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::GNOME_FEMALE, "GnomeFemaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::GNOME_FEMALE, "GnomeFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::GNOME_FEMALE, "GnomeFemaleStandardNPC", "Pissed", 4);

    // Goblin Male
    loadCategory(greetingLibrary_, VoiceType::GOBLIN_MALE, "GoblinMaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::GOBLIN_MALE, "GoblinMaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::GOBLIN_MALE, "GoblinMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::GOBLIN_MALE, "GoblinMaleStandardNPC", "Pissed", 4);

    // Goblin Female
    loadCategory(greetingLibrary_, VoiceType::GOBLIN_FEMALE, "GoblinFemaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::GOBLIN_FEMALE, "GoblinFemaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::GOBLIN_FEMALE, "GoblinFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::GOBLIN_FEMALE, "GoblinFemaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::NIGHTELF_MALE, "NightElfMaleStandardNPC", "Greeting", 8);
    loadCategory(farewellLibrary_, VoiceType::NIGHTELF_MALE, "NightElfMaleStandardNPC", "Farewell", 7);
    loadCategory(vendorLibrary_, VoiceType::NIGHTELF_MALE, "NightElfMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::NIGHTELF_MALE, "NightElfMaleStandardNPC", "Pissed", 6);

    loadCategory(greetingLibrary_, VoiceType::NIGHTELF_FEMALE, "NightElfFemaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::NIGHTELF_FEMALE, "NightElfFemaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::NIGHTELF_FEMALE, "NightElfFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::NIGHTELF_FEMALE, "NightElfFemaleStandardNPC", "Pissed", 6);

    loadCategory(greetingLibrary_, VoiceType::ORC_MALE, "OrcMaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::ORC_MALE, "OrcMaleStandardNPC", "Farewell", 5);
    loadCategory(vendorLibrary_, VoiceType::ORC_MALE, "OrcMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::ORC_MALE, "OrcMaleStandardNPC", "Pissed", 4);

    loadCategory(greetingLibrary_, VoiceType::ORC_FEMALE, "OrcFemaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::ORC_FEMALE, "OrcFemaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::ORC_FEMALE, "OrcFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::ORC_FEMALE, "OrcFemaleStandardNPC", "Pissed", 6);

    loadCategory(greetingLibrary_, VoiceType::TAUREN_MALE, "TaurenMaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::TAUREN_MALE, "TaurenMaleStandardNPC", "Farewell", 5);
    // Tauren Male has no Vendor/Pissed sounds in manifest

    loadCategory(greetingLibrary_, VoiceType::TAUREN_FEMALE, "TaurenFemaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::TAUREN_FEMALE, "TaurenFemaleStandardNPC", "Farewell", 5);
    // Tauren Female has no Vendor/Pissed sounds in manifest

    loadCategory(greetingLibrary_, VoiceType::TROLL_MALE, "TrollMaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::TROLL_MALE, "TrollMaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::TROLL_MALE, "TrollMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::TROLL_MALE, "TrollMaleStandardNPC", "Pissed", 6);

    loadCategory(greetingLibrary_, VoiceType::TROLL_FEMALE, "TrollFemaleStandardNPC", "Greeting", 5);
    loadCategory(farewellLibrary_, VoiceType::TROLL_FEMALE, "TrollFemaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::TROLL_FEMALE, "TrollFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::TROLL_FEMALE, "TrollFemaleStandardNPC", "Pissed", 5);

    loadCategory(greetingLibrary_, VoiceType::UNDEAD_MALE, "UndeadMaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::UNDEAD_MALE, "UndeadMaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::UNDEAD_MALE, "UndeadMaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::UNDEAD_MALE, "UndeadMaleStandardNPC", "Pissed", 6);

    loadCategory(greetingLibrary_, VoiceType::UNDEAD_FEMALE, "UndeadFemaleStandardNPC", "Greeting", 6);
    loadCategory(farewellLibrary_, VoiceType::UNDEAD_FEMALE, "UndeadFemaleStandardNPC", "Farewell", 6);
    loadCategory(vendorLibrary_, VoiceType::UNDEAD_FEMALE, "UndeadFemaleStandardNPC", "Vendor", 2);
    loadCategory(pissedLibrary_, VoiceType::UNDEAD_FEMALE, "UndeadFemaleStandardNPC", "Pissed", 6);

    // Blood Elf Male (TBC+ NPCBloodElfMaleStandard, sparse numbering up to 12)
    loadCategory(greetingLibrary_, VoiceType::BLOODELF_MALE, "NPCBloodElfMaleStandard", "Greeting", 12);
    loadCategory(farewellLibrary_, VoiceType::BLOODELF_MALE, "NPCBloodElfMaleStandard", "Farewell", 12);
    loadCategory(vendorLibrary_, VoiceType::BLOODELF_MALE, "NPCBloodElfMaleStandard", "Vendor", 6);
    loadCategory(pissedLibrary_, VoiceType::BLOODELF_MALE, "NPCBloodElfMaleStandard", "Pissed", 10);

    // Blood Elf Female
    loadCategory(greetingLibrary_, VoiceType::BLOODELF_FEMALE, "NPCBloodElfFemaleStandard", "Greeting", 12);
    loadCategory(farewellLibrary_, VoiceType::BLOODELF_FEMALE, "NPCBloodElfFemaleStandard", "Farewell", 12);
    loadCategory(vendorLibrary_, VoiceType::BLOODELF_FEMALE, "NPCBloodElfFemaleStandard", "Vendor", 6);
    loadCategory(pissedLibrary_, VoiceType::BLOODELF_FEMALE, "NPCBloodElfFemaleStandard", "Pissed", 10);

    // Draenei Male
    loadCategory(greetingLibrary_, VoiceType::DRAENEI_MALE, "NPCDraeneiMaleStandard", "Greeting", 12);
    loadCategory(farewellLibrary_, VoiceType::DRAENEI_MALE, "NPCDraeneiMaleStandard", "Farewell", 12);
    loadCategory(vendorLibrary_, VoiceType::DRAENEI_MALE, "NPCDraeneiMaleStandard", "Vendor", 6);
    loadCategory(pissedLibrary_, VoiceType::DRAENEI_MALE, "NPCDraeneiMaleStandard", "Pissed", 10);

    // Draenei Female
    loadCategory(greetingLibrary_, VoiceType::DRAENEI_FEMALE, "NPCDraeneiFemaleStandard", "Greeting", 12);
    loadCategory(farewellLibrary_, VoiceType::DRAENEI_FEMALE, "NPCDraeneiFemaleStandard", "Farewell", 12);
    loadCategory(vendorLibrary_, VoiceType::DRAENEI_FEMALE, "NPCDraeneiFemaleStandard", "Vendor", 6);
    loadCategory(pissedLibrary_, VoiceType::DRAENEI_FEMALE, "NPCDraeneiFemaleStandard", "Pissed", 10);

    // Load combat sounds from Character vocal files
    // These use a different path structure: Sound\Character\{Race}\{Race}Vocal{Gender}\{Race}{Gender}{Sound}.wav
    auto loadCombatCategory = [this](
        std::unordered_map<VoiceType, std::vector<VoiceSample>>& library,
        VoiceType type,
        const std::string& raceFolder,
        const std::string& raceGender,
        const std::string& soundType,
        int count) {

        auto& samples = library[type];
        for (int i = 1; i <= count; ++i) {
            std::string num = (i < 10) ? ("0" + std::to_string(i)) : std::to_string(i);
            std::string path = "Sound\\Character\\" + raceFolder + "\\" + raceFolder + "Vocal" +
                               (raceGender.find("Male") != std::string::npos ? "Male" : "Female") +
                               "\\" + raceGender + soundType + num + ".wav";
            VoiceSample sample;
            if (loadSound(path, sample)) samples.push_back(std::move(sample));
        }
    };

    // A race's flee lines, which nothing asks for yet - kept because the
    // library's shape is the same one the interaction lines use. What an angry
    // unit says is not here: the client asks that out of the unit's own
    // CreatureSoundData row (0x007474b0 kind 0, through EntitySpawner).
    loadCombatCategory(fleeLibrary_, VoiceType::HUMAN_MALE, "Human", "HumanMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::HUMAN_FEMALE, "Human", "HumanFemale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::DWARF_MALE, "Dwarf", "DwarfMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::GNOME_MALE, "Gnome", "GnomeMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::GNOME_FEMALE, "Gnome", "GnomeFemale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::NIGHTELF_MALE, "NightElf", "NightElfMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::NIGHTELF_FEMALE, "NightElf", "NightElfFemale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::ORC_MALE, "Orc", "OrcMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::ORC_FEMALE, "Orc", "OrcFemale", "Flee", 2);
    // The Undead lines sit under a Scourge folder.
    loadCombatCategory(fleeLibrary_, VoiceType::UNDEAD_MALE, "Scourge", "UndeadMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::UNDEAD_FEMALE, "Scourge", "UndeadFemale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::TAUREN_MALE, "Tauren", "TaurenMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::TAUREN_FEMALE, "Tauren", "TaurenFemale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::TROLL_MALE, "Troll", "TrollMale", "Flee", 2);
    loadCombatCategory(fleeLibrary_, VoiceType::TROLL_FEMALE, "Troll", "TrollFemale", "Flee", 2);

    // Blood Elf and Draenei combat sounds (flat folder structure, no VocalMale/Female subfolder)
    auto loadCombatFlat = [this](
        std::unordered_map<VoiceType, std::vector<VoiceSample>>& library,
        VoiceType type,
        const std::string& raceFolder,
        const std::string& raceGender,
        const std::string& soundType,
        int count) {

        auto& samples = library[type];
        for (int i = 1; i <= count; ++i) {
            std::string num = (i < 10) ? ("0" + std::to_string(i)) : std::to_string(i);
            std::string path = "Sound\\Character\\" + raceFolder + "\\" + raceGender + soundType + num + ".wav";
            VoiceSample sample;
            if (loadSound(path, sample)) samples.push_back(std::move(sample));
        }
    };

    // Blood Elf and Draenei, whose folders have no VocalMale/VocalFemale half.
    loadCombatFlat(fleeLibrary_, VoiceType::BLOODELF_MALE, "BloodElf", "BloodElfMale", "Flee", 3);
    loadCombatFlat(fleeLibrary_, VoiceType::BLOODELF_FEMALE, "BloodElf", "BloodElfFemale", "Flee", 3);
    loadCombatFlat(fleeLibrary_, VoiceType::DRAENEI_MALE, "Draenei", "DraeneiMale", "Flee", 3);
    loadCombatFlat(fleeLibrary_, VoiceType::DRAENEI_FEMALE, "Draenei", "DraeneiFemale", "Flee", 3);
}

bool NpcVoiceManager::loadSound(const std::string& path, VoiceSample& sample) {
    if (!assetManager_) return false;

    if (!assetManager_->fileExists(path)) {
        return false;
    }

    sample.path = path;
    // The reach the SoundEntries row naming this file asks for, when a row
    // names it. One that no row names keeps the reach these plays had anyway.
    if (const auto it = reachByName_.find(leafLower(path)); it != reachByName_.end()) {
        sample.minDistance = it->second.first;
        sample.cutoff = it->second.second;
    }
    sample.data = assetManager_->readFile(path);

    if (sample.data.empty()) {
        LOG_WARNING("NPC voice: Failed to load sound data from ", path);
        return false;
    }

    return true;
}

void NpcVoiceManager::playSound(uint64_t npcGuid, VoiceType voiceType, SoundCategory category, const glm::vec3& position) {
    if (!AudioEngine::instance().isInitialized()) {
        return;
    }

    // Check cooldown (except for the two kinds that override it)
    auto now = std::chrono::steady_clock::now();
    if (category != SoundCategory::PISSED && category != SoundCategory::FLEE) {
        auto it = lastPlayTime_.find(npcGuid);
        if (it != lastPlayTime_.end()) {
            float elapsed = std::chrono::duration<float>(now - it->second).count();
            if (elapsed < GREETING_COOLDOWN) {
                return;
            }
        }
    }

    // Select library based on category
    std::unordered_map<VoiceType, std::vector<VoiceSample>>* library = nullptr;
    switch (category) {
        case SoundCategory::GREETING: library = &greetingLibrary_; break;
        case SoundCategory::FAREWELL: library = &farewellLibrary_; break;
        case SoundCategory::VENDOR: library = &vendorLibrary_; break;
        case SoundCategory::PISSED: library = &pissedLibrary_; break;
        case SoundCategory::FLEE: library = &fleeLibrary_; break;
    }

    // Find voice samples for this type
    auto libIt = library->find(voiceType);
    if (libIt == library->end() || libIt->second.empty()) {
        // Fallback to GENERIC
        libIt = library->find(VoiceType::GENERIC);
        if (libIt == library->end() || libIt->second.empty()) {
            return;
        }
    }

    const auto& samples = libIt->second;
    std::uniform_int_distribution<size_t> dist(0, samples.size() - 1);
    const auto& sample = samples[dist(rng_)];

    // Play sound at the reach the clip's own row asks for. A voice line is a
    // sound of the few yards round the unit saying it: 8 yards at full volume
    // inside a 45-yard carry for every row behind NPCSounds, where this asked
    // for 60 and never named a core at all. A clip no row names keeps the 60,
    // rather than the wider reach the helper assumes of a row that named none.
    float minDistance = 1.0f, maxDistance = 60.0f;
    if (sample.cutoff > 0.0f)
        sound_distance::entryDistance(sample.minDistance, sample.cutoff, minDistance, maxDistance);
    std::uniform_real_distribution<float> pitchDist(0.98f, 1.02f);
    bool success = AudioEngine::instance().playSound3D(
        sample.data,
        position,
        1.0f * volumeScale_,
        pitchDist(rng_),
        maxDistance,
        minDistance
    );

    if (success) {
        lastPlayTime_[npcGuid] = now;
    }
}

void NpcVoiceManager::playGreeting(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position) {
    // Check if on cooldown - if so, increment pissed counter instead
    auto now = std::chrono::steady_clock::now();
    auto it = lastPlayTime_.find(npcGuid);
    if (it != lastPlayTime_.end()) {
        float elapsed = std::chrono::duration<float>(now - it->second).count();
        if (elapsed < GREETING_COOLDOWN) {
            // On cooldown - increment click count and maybe play pissed sound
            playPissed(npcGuid, voiceType, position);
            return;
        }
    }

    // Reset click count on successful greeting
    clickCount_[npcGuid] = 0;
    playSound(npcGuid, voiceType, SoundCategory::GREETING, position);
}

void NpcVoiceManager::playFarewell(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position) {
    playSound(npcGuid, voiceType, SoundCategory::FAREWELL, position);
}

void NpcVoiceManager::playVendor(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position) {
    playSound(npcGuid, voiceType, SoundCategory::VENDOR, position);
}

void NpcVoiceManager::playPissed(uint64_t npcGuid, VoiceType voiceType, const glm::vec3& position) {
    // Increment click count
    clickCount_[npcGuid]++;

    // Only play pissed sound after threshold
    if (clickCount_[npcGuid] >= PISSED_CLICK_THRESHOLD) {
        playSound(npcGuid, voiceType, SoundCategory::PISSED, position);
        clickCount_[npcGuid] = 0;  // Reset after playing
    }
}

} // namespace audio
} // namespace wowee

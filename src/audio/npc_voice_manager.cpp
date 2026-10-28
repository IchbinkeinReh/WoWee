#include "audio/npc_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_distance.hpp"
#include "audio/zone_music.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>

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
    lineHandle_.clear();
    askCount_.clear();
    reachByName_.clear();
    kitIdByDisplay_.clear();
    kitRows_.clear();
    soundEntryRowById_.clear();
    kitSamples_.clear();
    kitLastFile_.clear();
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
        const float volume = dbc->getFloat(i, 24);
        const uint32_t flags = dbc->getUInt32(i, 25);
        for (uint32_t f = 0; f < 10; ++f) {
            const std::string file = dbc->getString(i, 3 + f);
            if (file.empty()) continue;
            reachByName_.try_emplace(leafLower(file), EntryAsk{minDistance, cutoff, volume, flags});
        }
    }
}

// Which voice kit a unit's display names.
//
// CreatureDisplayInfo.dbc column 12 (stride 0x40, 16 columns) names an NPCSounds
// row: 10508 of its 24262 rows carry a value there, every one of them inside the
// id range NPCSounds keys by (33..336, exact), and no other table in the client's
// own data takes that id space. NPCSounds' row is five dwords - the id, three
// SoundEntries rows, and a fifth column that is 0 in all 301 rows. Read against
// CreatureDisplayInfoExtra's DisplayRaceID and DisplaySexID, the kit agrees with
// what the model looks like 7913 times and differs 2033: a gnome model asks the
// Dwarf kit, a blood elf model the NightElf kit, a troll model the Tauren kit.
// So the kit follows the voice the unit is given, not the body it has, and a
// race/sex guess picks a different line than the row does for one display in five.
//
// The three columns are, by what the kits they name go on to play: 1 the line on
// the interaction (a greeting for most kits, a vendor's own for the 87 rows whose
// kit is a vendor's, an attack cry for a few), 2 the farewell, 3 the irritated
// answer. Established from the file names those SoundEntries rows carry, not from
// client code - the routine that reads column 12 was not found, and the loader of
// NPCSounds (asserting 5 columns and a 0x14 row size, which is this table's
// shape) is reached only through a function-pointer run the decompile never
// resolves. What is read here is the data's own agreement, twice over.
void NpcVoiceManager::indexVoiceKits() {
    if (!assetManager_) return;

    // Older expansions' CreatureDisplayInfo has no column 12 at all, which is
    // why the field count is asked before the column is.
    if (auto cd = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
        cd && cd->isLoaded() && cd->getFieldCount() > 12) {
        for (uint32_t i = 0; i < cd->getRecordCount(); ++i) {
            if (const uint32_t kitId = cd->getUInt32(i, 12); kitId != 0)
                kitIdByDisplay_.try_emplace(cd->getUInt32(i, 0), kitId);
        }
    }

    if (auto ns = assetManager_->loadDBCOptional("NPCSounds.dbc");
        ns && ns->isLoaded() && ns->getFieldCount() >= 4) {
        for (uint32_t i = 0; i < ns->getRecordCount(); ++i) {
            kitRows_.try_emplace(ns->getUInt32(i, 0),
                                 std::array<uint32_t, 3>{ns->getUInt32(i, 1), ns->getUInt32(i, 2),
                                                         ns->getUInt32(i, 3)});
        }
    }

    // Kit rows are asked by id, and SoundEntries is keyed by an id that is not
    // its row number - 12941 rows, so a scan per click is not the answer.
    if (auto se = assetManager_->loadDBCOptional("SoundEntries.dbc"); se && se->isLoaded()) {
        for (uint32_t i = 0; i < se->getRecordCount(); ++i)
            soundEntryRowById_.try_emplace(se->getUInt32(i, 0), i);
    }
}

std::pair<const std::vector<VoiceSample>*, int32_t*> NpcVoiceManager::kitSlot(uint32_t displayId, size_t slot) {
    if (!assetManager_ || slot >= 3) return {};

    const auto display = kitIdByDisplay_.find(displayId);
    if (display == kitIdByDisplay_.end()) return {};

    // Read once per kit, however many units share the display. A slot with no
    // clip in it stays cached empty, so it is not gone looking for again per click.
    if (const auto loaded = kitSamples_.find(display->second); loaded != kitSamples_.end()) {
        const auto& samples = loaded->second[slot];
        if (samples.empty()) return {};
        const auto last = kitLastFile_.find(display->second);
        if (last == kitLastFile_.end()) return {&samples, nullptr};
        return {&samples, &last->second[slot]};
    }

    auto& samples = kitSamples_[display->second][slot];
    auto& lastFiles = kitLastFile_[display->second];
    lastFiles.fill(-1);

    const auto row = kitRows_.find(display->second);
    if (row == kitRows_.end() || row->second[slot] == 0) return {};
    const auto entry = soundEntryRowById_.find(row->second[slot]);
    auto se = assetManager_->loadDBCOptional("SoundEntries.dbc");
    if (entry == soundEntryRowById_.end() || !se || !se->isLoaded() || se->getFieldCount() <= 22)
        return {};

    for (uint32_t f = 0; f < 10; ++f) {
        const std::string file = se->getString(entry->second, 3 + f);
        if (file.empty()) continue;
        VoiceSample sample;
        if (!loadSound(file, sample)) continue;
        sample.frequency = se->getUInt32(entry->second, 13 + f);
        samples.push_back(std::move(sample));
    }
    return samples.empty() ? std::pair<const std::vector<VoiceSample>*, int32_t*>{}
                           : std::pair{&samples, &lastFiles[slot]};
}

void NpcVoiceManager::loadVoiceSounds() {
    if (!assetManager_) return;
    indexSoundEntriesReach();
    indexVoiceKits();

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
        sample.minDistance = it->second.minDistance;
        sample.cutoff = it->second.cutoff;
        sample.volume = it->second.volume;
        sample.flags = it->second.flags;
    }
    sample.data = assetManager_->readFile(path);

    if (sample.data.empty()) {
        LOG_WARNING("NPC voice: Failed to load sound data from ", path);
        return false;
    }

    return true;
}

bool NpcVoiceManager::playSound(uint64_t npcGuid, uint32_t displayId, VoiceType voiceType,
                                SoundCategory category, int32_t variant, const glm::vec3& position) {
    if (!AudioEngine::instance().isInitialized())
        return false;

    // Whether a line is already going for this unit is asked by the callers, at
    // the same points the client asks it - on the interaction (00720000.c:8066)
    // and on the farewell (:382) - not here, so that a refused ask leaves the
    // unit's ask counter where it was, which is what the client's does too.
    const VoiceSample* sample = nullptr;

    // The line the unit's own display names, when it names one: the client takes
    // the interaction line, the farewell and the irritated answer out of the kit
    // NPCSounds gives that display, and from among its files by their weights.
    // The vendor's line is not asked of the kit. A kit's first slot holds a
    // vendor's own clips for the 87 rows whose kit is a vendor's, which is a
    // property of the display rather than the rule "a vendor window is open", and
    // where the client's vendor line really comes from has not been found.
    size_t slot = 0;
    bool kitAsked = true;
    switch (category) {
        case SoundCategory::GREETING: slot = 0; break;
        case SoundCategory::FAREWELL: slot = 1; break;
        case SoundCategory::PISSED: slot = 2; break;
        default: kitAsked = false; break;
    }
    if (kitAsked) {
        if (const auto kit = kitSlot(displayId, slot); kit.first != nullptr) {
            const auto& clips = *kit.first;
            size_t chosen = 0;
            if (variant >= 0) {
                // Picked by position rather than by weight: once the unit's counter
                // is past the first slot's five asks, 0x00746900 walks the kit's
                // alternates in order and hands back the position as the variant
                // index. The caller only asks a position it has just counted, so a
                // position that is not in the kit means it changed under the ask -
                // refuse it, which is the answer the client's counter turns into a
                // reset and a first-slot line.
                if (static_cast<size_t>(variant) >= clips.size()) return false;
                chosen = static_cast<size_t>(variant);
            } else {
                std::vector<uint32_t> frequency;
                frequency.reserve(clips.size());
                for (const auto& candidate : clips) frequency.push_back(candidate.frequency);
                // Which of a kit's files, by those weights, is the client's own
                // rule (0x004cbac0), and this repo already implements it for the
                // music. A voice line is asked through it with the variant index
                // left unset, which is the form that remembers the file it last
                // gave on the SoundEntries record (+0x44) and will not give that
                // one again while another file has weight; the same is remembered
                // per kit slot here. A kit with no weight on any file is left to
                // the even pick below, where the music would play none.
                std::optional<size_t> lastFile;
                if (kit.second != nullptr && *kit.second >= 0)
                    lastFile = static_cast<size_t>(*kit.second);
                const uint32_t roll = std::uniform_int_distribution<uint32_t>(0, UINT32_MAX)(rng_);
                if (const auto picked = zone_music::pickFile(frequency, lastFile, roll);
                    picked.has_value()) {
                    chosen = *picked;
                    if (kit.second != nullptr) *kit.second = static_cast<int32_t>(chosen);
                } else {
                    chosen = std::uniform_int_distribution<size_t>(0, clips.size() - 1)(rng_);
                }
            }
            sample = &clips[chosen];
        } else {
            // The display names no kit, or none behind the slot asked for, and the
            // client asks for no line at that point at all: 0x00746900 returns -1
            // when the row is missing or its first column is 0, and 0x00720860
            // refuses when the row cannot be found. Most displays are in that
            // group - 13754 of CreatureDisplayInfo's 24262 rows name no kit - and
            // that is the client's "the wolf says nothing". Falling back to a race
            // library here was this code's own, and it put a guard's line in a
            // wolf's mouth.
            return false;
        }
    }

    if (sample == nullptr) {
        // Only the vendor's line (and a flee, which has no slot in these kits
        // either) is still taken from the race library, because where the client's
        // vendor line comes from has not been found.

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
                return false;
            }
        }

        const auto& samples = libIt->second;
        std::uniform_int_distribution<size_t> dist(0, samples.size() - 1);
        sample = &samples[dist(rng_)];
    }

    // Play sound at the reach the clip's own row asks for. A voice line is a
    // sound of the few yards round the unit saying it: 8 yards at full volume
    // inside a 45-yard carry for every row behind NPCSounds, where this asked
    // for 60 and never named a core at all. A clip no row names keeps the 60,
    // rather than the wider reach the helper assumes of a row that named none.
    float minDistance = 1.0f, maxDistance = 60.0f;
    if (sample->cutoff > 0.0f)
        sound_distance::entryDistance(sample->minDistance, sample->cutoff, minDistance, maxDistance);
    // Whether the line swings in pitch or in volume at all is the row's to say,
    // not ours: SoundEntries Flags (column 25) bit 0x400 gives the play a pitch of
    // 0.85 + 0.3*rand, and bit 0x800 adds -0.15 + 0.3*rand to its volume
    // (0x004c6a40 at 004c0000.c:6013-6022, each factor built from the pair of
    // bounds set in front of the draw). The one is applied to the sound's base
    // pitch (0x00878760 multiplies it onto +0x70 and hands the product to the
    // handle); the other reaches the volume setter, 0x00879710, which clamps what
    // it is asked into [0,1] - so a swing can take a line quiet or full but never
    // past full. Of the 12941 rows only 126 ask for a pitch and 7 for a volume,
    // and of the 527 behind these kits **none asks either**: the pitch jitter here
    // used to be 0.98-1.02 on every line, which was this code's own, not the
    // client's, and it made the same clip sound different from one click to the
    // next. A voice line now plays at the pitch it was recorded at.
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    float pitch = 1.0f;
    if ((sample->flags & 0x400u) != 0)
        pitch = 0.85f + 0.3f * unit(rng_);
    float volume = sample->volume;
    if ((sample->flags & 0x800u) != 0)
        volume += -0.15f + 0.3f * unit(rng_);
    volume = std::clamp(volume * volumeScale_, 0.0f, 1.0f);

    // The handle the play went into is kept per unit, because that is the slot the
    // client asks about before asking for the next line - the unit's own sound
    // handle, unit + 0x8f4, tested by 0x004c5be0 at 00720000.c:8066 and :382.
    uint32_t handle = 0;
    const bool success = AudioEngine::instance().playSound3D(
        sample->data,
        position,
        volume,
        pitch,
        maxDistance,
        minDistance,
        Channel::Own,
        PlayLimit::None,
        voice_priority::kDefault,
        &handle
    );

    if (success)
        lineHandle_[npcGuid] = handle;
    return success;
}

bool NpcVoiceManager::linePlaying(uint64_t npcGuid) const {
    const auto it = lineHandle_.find(npcGuid);
    return it != lineHandle_.end() && it->second != 0 &&
           AudioEngine::instance().isSoundPlaying(it->second);
}

void NpcVoiceManager::playGreeting(uint64_t npcGuid, uint32_t displayId, VoiceType voiceType, const glm::vec3& position) {
    // One line at a time. While this unit's line is still going the ask is refused
    // outright and its counter does not move - 0x0072a680 asks the same question of
    // the unit's sound handle at 00720000.c:8066, and there is no timer anywhere in
    // that path. The wall-clock cooldown this used to keep refused the second click
    // for two seconds and then answered the cooldown with the irritated line, which
    // is not what a second click does in the client: it is simply not asked.
    if (linePlaying(npcGuid))
        return;

    // Which slot serves, and which alternate of it, is the counter rule of
    // 0x00746900: five asks answered from the kit's first slot, then its third
    // slot walked once per alternate, then the counter resets and the first slot
    // serves again. The counter moves only when a line actually started (:8087).
    int& counter = askCount_[npcGuid];
    SoundCategory category = SoundCategory::GREETING;
    int32_t variant = -1;
    if (counter >= kAsksFromFirstSlot) {
        const int extra = counter - kAsksFromFirstSlot;
        const auto walked = kitSlot(displayId, 2);
        if (walked.first == nullptr || static_cast<size_t>(extra) >= walked.first->size()) {
            counter = 0;  // walked out of the third slot: the first serves again
        } else {
            category = SoundCategory::PISSED;
            variant = extra;
        }
    }

    if (playSound(npcGuid, displayId, voiceType, category, variant, position))
        ++counter;
}

void NpcVoiceManager::playFarewell(uint64_t npcGuid, uint32_t displayId, VoiceType voiceType, const glm::vec3& position) {
    // The ask count starts over on a hello or a goodbye, before anything else the
    // routine does (00720000.c:376-377 clears the counter and the unit it belongs
    // to, ahead of the busy test at :382).
    askCount_.erase(npcGuid);
    if (linePlaying(npcGuid))
        return;
    playSound(npcGuid, displayId, voiceType, SoundCategory::FAREWELL, -1, position);
}

void NpcVoiceManager::playVendor(uint64_t npcGuid, uint32_t displayId, VoiceType voiceType, const glm::vec3& position) {
    // Still the race library, and still not asked of the kit: where the client's
    // vendor line comes from has not been found. Nor is it known whether that path
    // is one line at a time, so no busy test is applied that the client has not
    // been seen to apply.
    playSound(npcGuid, displayId, voiceType, SoundCategory::VENDOR, -1, position);
}

void NpcVoiceManager::playPissed(uint64_t npcGuid, uint32_t displayId, VoiceType voiceType, const glm::vec3& position) {
    // The repeat-click path reaches the kit's third slot through playGreeting's
    // counter now, which is how the client walks it. This stays for a caller that
    // wants the irritated line directly - and an angry unit's line is in fact
    // asked of the unit's own reaction change (SMSG_AI_REACTION), which is where
    // the client really asks for it.
    if (linePlaying(npcGuid))
        return;
    playSound(npcGuid, displayId, voiceType, SoundCategory::PISSED, -1, position);
}

} // namespace audio
} // namespace wowee

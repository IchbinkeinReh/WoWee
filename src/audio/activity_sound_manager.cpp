#include "audio/activity_sound_manager.hpp"
#include "audio/footstep_paths.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entry.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace wowee {
namespace audio {

namespace {

// SoundEntries names its files by leaf (no folder), while this manager reaches
// its clips by full archive path, so joining the two needs the same lowered
// leaf the voice manager joins on.
using sound_entry::leafLower;

} // namespace

ActivitySoundManager::ActivitySoundManager() : rng(std::random_device{}()) {}
ActivitySoundManager::~ActivitySoundManager() { shutdown(); }

bool ActivitySoundManager::initialize(pipeline::AssetManager* assets) {
    shutdown();
    assetManager = assets;
    if (!assetManager) return false;

    // Voice profile clips (jump, swim, hardLand, combat vocals) are set at
    // character spawn via setCharacterVoiceProfile() with the correct race/gender.

    // Read before any clip is preloaded: every candidate looks its ask up here.
    indexRowAsks();

    preloadLandingSet(FootstepSurface::STONE, "Stone");
    preloadLandingSet(FootstepSurface::DIRT, "Dirt");
    preloadLandingSet(FootstepSurface::GRASS, "Grass");
    preloadLandingSet(FootstepSurface::WOOD, "Wood");
    preloadLandingSet(FootstepSurface::METAL, "Metal");
    preloadLandingSet(FootstepSurface::WATER, "Water");
    preloadLandingSet(FootstepSurface::SNOW, "Snow");

    initialized = true;
    core::Logger::getInstance().info("Activity SFX loaded: jump=", jumpClips.size(),
                                     " swimLoop=", swimLoopClips.size());
    return true;
}

void ActivitySoundManager::shutdown() {
    stopSwimLoop();
    stopOneShot();
    std::remove(loopTempPath.c_str());
    for (auto& set : landingSets) set.clips.clear();
    jumpClips.clear();
    swimLoopClips.clear();
    hardLandClips.clear();
    rowsByLeaf.clear();
    swimmingActive = false;
    swimMoving = false;
    initialized = false;
    assetManager = nullptr;
}

void ActivitySoundManager::update([[maybe_unused]] float deltaTime) {
    reapProcesses();

    // Play swimming stroke sounds periodically when swimming and moving
    if (swimmingActive && swimMoving && !swimLoopClips.empty()) {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastSwimStrokeAt).count();

        // Play swimming stroke sound every 0.8 seconds (swim stroke rhythm)
        if (lastSwimStrokeAt.time_since_epoch().count() == 0 || elapsed >= 0.8f) {
            std::uniform_int_distribution<size_t> clipDist(0, swimLoopClips.size() - 1);
            const Sample& sample = swimLoopClips[clipDist(rng)];

            // Play as one-shot 2D sound. The water rows name these files: 0.6
            // for the medium set (which is the level this play used by guess)
            // and 1.0 for the small one, both with no swing asked.
            float volume = 0.0f, pitch = 1.0f;
            askedByRow(sample, 0.6f, volume, pitch);
            AudioEngine::instance().playSound2D(sample.data, volume, pitch);

            lastSwimStrokeAt = now;
        }
    } else if (!swimmingActive) {
        // Reset timer when not swimming
        lastSwimStrokeAt = std::chrono::steady_clock::time_point{};
    }
}

// PlayerExertions naming is inconsistent across races: most use
// {Race}{Gender}Final\{Race}{Gender}Main{Type}{Letter}.wav, but OrcMale's
// folder drops "Final", TaurenFemale and UndeadFemale name files with a
// "Final" stem instead of "Main", and Blizzard shipped typo'd stems for
// HumanFemale ("HumanFeamle") and TrollFemale ("TrollFemal"). Generate every
// observed folder\stem prefix; preloadCandidates skips paths that don't exist.
static std::vector<std::string> exertionPrefixes(const std::string& raceBase, bool male) {
    const std::string gender = male ? "Male" : "Female";
    // Undead uses "Scourge" in raceBase but "Undead" in PlayerExertions
    const std::string race = (raceBase == "Scourge") ? "Undead" : raceBase;
    const std::string stem = race + gender;
    const std::string base = "Sound\\Character\\PlayerExertions\\";

    std::vector<std::string> prefixes;
    prefixes.push_back(base + stem + "Final\\" + stem + "Main");   // most races
    prefixes.push_back(base + stem + "\\" + stem + "Main");        // OrcMale folder
    prefixes.push_back(base + stem + "Final\\" + stem + "Final");  // TaurenFemale/UndeadFemale stem
    if (race == "Human" && !male) {
        prefixes.push_back(base + stem + "Final\\HumanFeamleMain");
    }
    if (race == "Troll" && !male) {
        prefixes.push_back(base + stem + "Final\\TrollFemalMain");
    }
    return prefixes;
}

// Every clip here is chosen by archive path, never by a SoundEntries id --
// these are the exertions and the footstep/foley files, which the client also
// plays, but from rows addressed by id. Joining the two by leaf file name is
// what lets the row's own ask (VolumeFloat 24, Flags 25) reach the play.
//
// A leaf can be named by more than one row, and then the ask is not this
// clip's ask but whichever row was read first -- which is a guess, not the
// data. So a leaf whose rows disagree about volume or flags is dropped: it
// keeps the level this manager would have used anyway. "mfoothugegiantdirta"
// (rows 732 CharacterGiantDirt and 12126 Giant Stomp) and
// "mdeathimpactgiantwatera" (1268 DeathThudWaterGiant and 8461 Pagle's Point
// Cast Impact) are the leaves that land in that pile; the splash sets are
// named by nine rows apiece that all ask the same thing, so they stay.
void ActivitySoundManager::indexRowAsks() {
    if (!assetManager) return;
    rowsByLeaf.clear();
    auto dbc = assetManager->loadDBCOptional("SoundEntries.dbc");
    // Older clients' SoundEntries stops before the volume/flags pair.
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() <= 25) return;
    std::unordered_set<std::string> disagreeing;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const float volume = dbc->getFloat(i, 24);
        const uint32_t flags = dbc->getUInt32(i, 25);
        for (uint32_t f = 0; f < 10; ++f) {
            const std::string file = dbc->getString(i, 3 + f);
            if (file.empty()) continue;
            const auto [ask, inserted] = rowsByLeaf.try_emplace(leafLower(file), RowAsk{volume, flags});
            if (!inserted && (ask->second.volume != volume || ask->second.flags != flags))
                disagreeing.insert(ask->first);
        }
    }
    for (const auto& leaf : disagreeing) rowsByLeaf.erase(leaf);
}

// The level and pitch this clip's own row asks for, swung as its Flags ask.
// `own` is the level this manager used before any row was read, and it survives
// only for a clip no row names: without a row there is no ask to honour and no
// flag to swing by, so such a play keeps its level and stays at pitch 1.
void ActivitySoundManager::askedByRow(const Sample& sample, float own, float& volume, float& pitch) {
    const float unitDraw = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
    const float base = (sample.rowVolume >= 0.0f ? sample.rowVolume : own) * volumeScale;
    volume = sound_entry::volumeForFlags(sample.flags, base, unitDraw);
    pitch = sound_entry::pitchForFlags(sample.flags, unitDraw);
}

void ActivitySoundManager::preloadCandidates(std::vector<Sample>& out, const std::vector<std::string>& candidates) {
    if (!assetManager) return;
    for (const auto& path : candidates) {
        if (!assetManager->fileExists(path)) continue;
        auto data = assetManager->readFile(path);
        if (data.empty()) continue;
        Sample sample;
        sample.path = path;
        sample.data = std::move(data);
        if (const auto ask = rowsByLeaf.find(leafLower(path)); ask != rowsByLeaf.end()) {
            sample.rowVolume = ask->second.volume;
            sample.flags = ask->second.flags;
        }
        out.push_back(std::move(sample));
    }
}

void ActivitySoundManager::preloadLandingSet(FootstepSurface surface, const std::string& material) {
    auto& clips = landingSets[static_cast<size_t>(surface)].clips;
    preloadCandidates(clips, classicFootstepPaths(material));
}

void ActivitySoundManager::rebuildJumpClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male) {
    jumpClips.clear();
    const std::string gender = male ? "Male" : "Female";
    const std::string prefix = "Sound\\Character\\" + raceFolder + "\\";
    const std::string stem = raceBase + gender;

    std::vector<std::string> candidates;
    for (const auto& exert : exertionPrefixes(raceBase, male)) {
        candidates.push_back(exert + "Jump.wav");
    }
    // movement_sound_manager convention (also verified working)
    candidates.push_back(prefix + stem + "Jump1.wav");
    candidates.push_back(prefix + stem + "Land1.wav");
    // Other common variants
    candidates.push_back(prefix + stem + "JumpA.wav");
    candidates.push_back(prefix + stem + "JumpB.wav");
    candidates.push_back(prefix + stem + "Jump.wav");
    candidates.push_back(prefix + gender + "\\" + stem + "JumpA.wav");
    candidates.push_back(prefix + gender + "\\" + stem + "JumpB.wav");
    candidates.push_back(prefix + stem + "\\" + stem + "Jump01.wav");
    candidates.push_back(prefix + stem + "\\" + stem + "Jump02.wav");
    preloadCandidates(jumpClips, candidates);
    if (jumpClips.empty()) {
        LOG_WARNING("No jump clips found for ", stem);
    } else {
        LOG_INFO("Loaded ", jumpClips.size(), " jump clips for ", stem);
    }
}

void ActivitySoundManager::rebuildSwimLoopClipsForProfile([[maybe_unused]] const std::string& raceFolder, [[maybe_unused]] const std::string& raceBase, [[maybe_unused]] bool male) {
    swimLoopClips.clear();

    // WoW 3.3.5a doesn't have dedicated swim loop sounds
    // Use water splash/footstep sounds as swimming stroke sounds
    preloadCandidates(swimLoopClips, {
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterB.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterC.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterD.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsMediumWaterE.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterA.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterB.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterC.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterD.wav",
        "Sound\\Character\\Footsteps\\WaterSplash\\FootStepsSmallWaterE.wav"
    });
}

void ActivitySoundManager::rebuildHardLandClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male) {
    hardLandClips.clear();
    const std::string gender = male ? "Male" : "Female";
    const std::string prefix = "Sound\\Character\\" + raceFolder + "\\";
    const std::string stem = raceBase + gender;
    preloadCandidates(hardLandClips, {
        prefix + stem + "\\" + stem + "LandHard01.wav",
        prefix + stem + "\\" + stem + "LandHard02.wav",
        prefix + stem + "LandHard01.wav",
        prefix + stem + "LandHard02.wav"
    });
}

void ActivitySoundManager::startSwimLoop() {
    // Swimming sounds now handled by periodic playback in update() method
    // This method kept for API compatibility but does nothing
}

void ActivitySoundManager::stopSwimLoop() {
    platform::killProcess(swimLoopPid);
}

void ActivitySoundManager::stopOneShot() {
}

void ActivitySoundManager::reapProcesses() {
    if (swimLoopPid != INVALID_PROCESS) {
        platform::isProcessRunning(swimLoopPid);
    }
}

void ActivitySoundManager::playJump() {
    if (!AudioEngine::instance().isInitialized() || jumpClips.empty()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastJumpAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastJumpAt).count() < 0.35f) return;
    }

    // Pick random clip
    std::uniform_int_distribution<size_t> dist(0, jumpClips.size() - 1);
    const Sample& sample = jumpClips[dist(rng)];

    // The exertion rows that name these clips ask one level for all of them --
    // 0.69, with no swing -- so the jitter this play used to invent is gone
    // wherever a row names the clip it picked.
    float volume = 0.0f, pitch = 1.0f;
    askedByRow(sample, 0.7f, volume, pitch);

    if (AudioEngine::instance().playSound2D(sample.data, volume, pitch)) {
        lastJumpAt = now;
    }
}

void ActivitySoundManager::playLanding(FootstepSurface surface, bool hardLanding) {
    if (!AudioEngine::instance().isInitialized()) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (lastLandAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastLandAt).count() < 0.10f) return;
    }

    const auto& clips = landingSets[static_cast<size_t>(surface)].clips;
    if (!clips.empty()) {
        std::uniform_int_distribution<size_t> dist(0, clips.size() - 1);
        const Sample& sample = clips[dist(rng)];

        // These are the client's own footstep clips, and their rows ask levels
        // of 0.5 (dirt, wood), 0.7 (grass, snow) and 0.8 (stone), every one of
        // them with a pitch swing (flags 0x400) -- quieter and more varied than
        // this play's invented 0.82/1.00 and +-5%. No row names the metal set,
        // and the hard/soft difference is ours alone, so both of those keep
        // this play's own level.
        float volume = 0.0f, pitch = 1.0f;
        askedByRow(sample, hardLanding ? 1.00f : 0.82f, volume, pitch);
        AudioEngine::instance().playSound2D(sample.data, volume, pitch);
        lastLandAt = now;
    }

    if (hardLanding && !hardLandClips.empty()) {
        std::uniform_int_distribution<size_t> dist(0, hardLandClips.size() - 1);
        const Sample& sample = hardLandClips[dist(rng)];
        float volume = 0.0f, pitch = 1.0f;
        askedByRow(sample, 0.84f, volume, pitch);
        AudioEngine::instance().playSound2D(sample.data, volume, pitch);
    }
}

void ActivitySoundManager::setSwimmingState(bool swimming, bool moving) {
    swimMoving = moving;
    if (swimming == swimmingActive) return;
    swimmingActive = swimming;
    if (swimmingActive) {
        LOG_INFO("Swimming started - playing swim loop");
        startSwimLoop();
    } else {
        LOG_INFO("Swimming stopped - stopping swim loop");
        stopSwimLoop();
    }
}

void ActivitySoundManager::setCharacterVoiceProfile(const std::string& modelName) {
    if (!assetManager || modelName.empty()) return;

    std::string lower = modelName;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool male = (lower.find("female") == std::string::npos);
    std::string folder = "Human";
    std::string base = "Human";

    struct RaceMap { const char* token; const char* folder; const char* base; };
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table whose
    // columns are its field names, with the struct in view directly above.
    static constexpr RaceMap races[] = {
        {"human", "Human", "Human"},
        {"orc", "Orc", "Orc"},
        {"dwarf", "Dwarf", "Dwarf"},
        {"nightelf", "NightElf", "NightElf"},
        {"scourge", "Scourge", "Scourge"},
        {"undead", "Scourge", "Scourge"},
        {"tauren", "Tauren", "Tauren"},
        {"gnome", "Gnome", "Gnome"},
        {"troll", "Troll", "Troll"},
        {"bloodelf", "BloodElf", "BloodElf"},
        {"draenei", "Draenei", "Draenei"},
        {"goblin", "Goblin", "Goblin"},
        {"worgen", "Worgen", "Worgen"},
    };
    // NOLINTEND(modernize-use-designated-initializers)
    for (const auto& r : races) {
        if (lower.find(r.token) != std::string::npos) {
            folder = r.folder;
            base = r.base;
            break;
        }
    }

    std::string key = folder + "|" + base + "|" + (male ? "M" : "F");
    if (key == voiceProfileKey) return;
    voiceProfileKey = key;
    rebuildJumpClipsForProfile(folder, base, male);
    rebuildSwimLoopClipsForProfile(folder, base, male);
    rebuildHardLandClipsForProfile(folder, base, male);
    core::Logger::getInstance().info("Activity SFX voice profile: ", voiceProfileKey,
                                     " jump clips=", jumpClips.size(),
                                     " swim clips=", swimLoopClips.size(),
                                     " hardLand clips=", hardLandClips.size());
}

void ActivitySoundManager::setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male) {
    if (!assetManager) return;
    std::string key = raceFolder + "|" + raceBase + "|" + (male ? "M" : "F");
    if (key == voiceProfileKey) return;
    voiceProfileKey = key;
    rebuildJumpClipsForProfile(raceFolder, raceBase, male);
    rebuildSwimLoopClipsForProfile(raceFolder, raceBase, male);
    rebuildHardLandClipsForProfile(raceFolder, raceBase, male);
    core::Logger::getInstance().info("Activity SFX voice profile (explicit): ", voiceProfileKey,
                                     " jump clips=", jumpClips.size(),
                                     " swim clips=", swimLoopClips.size(),
                                     " hardLand clips=", hardLandClips.size());
}

} // namespace audio
} // namespace wowee

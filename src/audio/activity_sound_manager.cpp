#include "audio/activity_sound_manager.hpp"
#include "audio/footstep_paths.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entry.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cctype>

namespace wowee {
namespace audio {

namespace {

// SoundEntries names its files by leaf (no folder), while this manager reaches
// its clips by full archive path, so joining the two needs the same lowered
// leaf the voice manager joins on.
std::string leafLower(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return leaf;
}

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
// The join is safe for the sets this manager builds, and it is not safe in
// general, so the caveat belongs here: of the 574 candidate paths the rebuild*
// sets and the landing sets offer, 74 leaf files are named by a row, and no
// leaf named twice disagrees about its ask. Other leaf names in the same table
// do -- "mfoothugegiant*" is named by row 732 (CharacterGiant<Surface>) and by
// 12126 ("Giant Stomp") with different asks, and "mdeathimpactgiantwatera.wav"
// by 1268 and 8461 -- so reaching those families by leaf would need the row id
// rather than a first-wins lookup.
void ActivitySoundManager::indexRowAsks() {
    if (!assetManager) return;
    rowsByLeaf.clear();
    auto dbc = assetManager->loadDBCOptional("SoundEntries.dbc");
    // Older clients' SoundEntries stops before the volume/flags pair.
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() <= 25) return;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        const float volume = dbc->getFloat(i, 24);
        const uint32_t flags = dbc->getUInt32(i, 25);
        for (uint32_t f = 0; f < 10; ++f) {
            const std::string file = dbc->getString(i, 3 + f);
            if (file.empty()) continue;
            rowsByLeaf.try_emplace(leafLower(file), RowAsk{volume, flags});
        }
    }
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

    // PlayerExertions prefixes (same naming quirks as combat vocals)
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
    rebuildCombatVocalClipsForProfile(folder, base, male);
    core::Logger::getInstance().info("Activity SFX voice profile: ", voiceProfileKey,
                                     " jump clips=", jumpClips.size(),
                                     " swim clips=", swimLoopClips.size(),
                                     " hardLand clips=", hardLandClips.size(),
                                     " attackGrunt clips=", attackGruntClips.size(),
                                     " wound clips=", woundClips.size(),
                                     " death clips=", deathClips.size());
}

void ActivitySoundManager::setCharacterVoiceProfile(const std::string& raceFolder, const std::string& raceBase, bool male) {
    if (!assetManager) return;
    std::string key = raceFolder + "|" + raceBase + "|" + (male ? "M" : "F");
    if (key == voiceProfileKey) return;
    voiceProfileKey = key;
    rebuildJumpClipsForProfile(raceFolder, raceBase, male);
    rebuildSwimLoopClipsForProfile(raceFolder, raceBase, male);
    rebuildHardLandClipsForProfile(raceFolder, raceBase, male);
    rebuildCombatVocalClipsForProfile(raceFolder, raceBase, male);
    core::Logger::getInstance().info("Activity SFX voice profile (explicit): ", voiceProfileKey,
                                     " jump clips=", jumpClips.size(),
                                     " swim clips=", swimLoopClips.size(),
                                     " hardLand clips=", hardLandClips.size(),
                                     " attackGrunt clips=", attackGruntClips.size(),
                                     " wound clips=", woundClips.size(),
                                     " death clips=", deathClips.size());
}

void ActivitySoundManager::rebuildCombatVocalClipsForProfile(const std::string& raceFolder, const std::string& raceBase, bool male) {
    attackGruntClips.clear();
    woundClips.clear();
    woundCritClips.clear();
    deathClips.clear();

    const std::string gender = male ? "Male" : "Female";
    const std::string stem = raceBase + gender;  // e.g. HumanFemale

    // WoW 3.3.5a has two sound sources for player combat vocalizations:
    //
    // 1) Vox files (some races only):
    //    Sound\Character\{Race}\{Gender}\m{Race}{Gender}{Type}Vox{Letter}.wav
    //    e.g. Sound\Character\Human\Female\mHumanFemaleAttackVoxA.wav
    //
    // 2) PlayerExertions (all races):
    //    Sound\Character\PlayerExertions\{Race}{Gender}Final\{Race}{Gender}Main{Type}{Letter}.wav
    //    e.g. Sound\Character\PlayerExertions\HumanMaleFinal\HumanMaleMainAttackA.wav
    //    EXCEPTIONS:
    //    - OrcMale uses folder "OrcMale" (no "Final" suffix)
    //    - HumanFemale files have Blizzard typo: "HumanFeamle" instead of "HumanFemale"

    const std::vector<std::string> exertPrefixes_ = exertionPrefixes(raceBase, male);
    const std::string voxPrefix = "Sound\\Character\\" + raceFolder + "\\" + gender + "\\m" + stem;

    // Attack grunts
    std::vector<std::string> attackPaths;
    for (char c = 'A'; c <= 'F'; ++c) {
        std::string s(1, c);
        for (const auto& exert : exertPrefixes_) {
            attackPaths.push_back(exert + "Attack" + s + ".wav");
        }
        attackPaths.push_back(voxPrefix + "AttackVox" + s + ".wav");
    }
    preloadCandidates(attackGruntClips, attackPaths);

    // Wound sounds (UndeadFemale ships one variant with no letter suffix)
    std::vector<std::string> woundPaths;
    for (char c = 'A'; c <= 'F'; ++c) {
        std::string s(1, c);
        for (const auto& exert : exertPrefixes_) {
            woundPaths.push_back(exert + "Wound" + s + ".wav");
        }
        woundPaths.push_back(voxPrefix + "WoundVox" + s + ".wav");
    }
    for (const auto& exert : exertPrefixes_) {
        woundPaths.push_back(exert + "Wound.wav");
    }
    preloadCandidates(woundClips, woundPaths);

    // Wound crit sounds. Some races have WoundCrit without a letter suffix,
    // and UndeadFemale's is typo'd "WoundCriatA".
    std::vector<std::string> woundCritPaths;
    for (char c = 'A'; c <= 'C'; ++c) {
        std::string s(1, c);
        for (const auto& exert : exertPrefixes_) {
            woundCritPaths.push_back(exert + "WoundCrit" + s + ".wav");
            woundCritPaths.push_back(exert + "WoundCriat" + s + ".wav");
        }
        woundCritPaths.push_back(voxPrefix + "WoundCriticalVox" + s + ".wav");
    }
    for (const auto& exert : exertPrefixes_) {
        woundCritPaths.push_back(exert + "WoundCrit.wav");
    }
    preloadCandidates(woundCritClips, woundCritPaths);

    // Death sounds (NightElf/Troll/Undead ship "Death" with no letter suffix)
    std::vector<std::string> deathPaths;
    for (char c = 'A'; c <= 'C'; ++c) {
        std::string s(1, c);
        for (const auto& exert : exertPrefixes_) {
            deathPaths.push_back(exert + "Death" + s + ".wav");
        }
        deathPaths.push_back(voxPrefix + "DeathVox" + s + ".wav");
    }
    for (const auto& exert : exertPrefixes_) {
        deathPaths.push_back(exert + "Death.wav");
    }
    preloadCandidates(deathClips, deathPaths);

    LOG_INFO("Combat vocals for ", stem, ": attack=", attackGruntClips.size(),
             " wound=", woundClips.size(), " woundCrit=", woundCritClips.size(),
             " death=", deathClips.size());
    if (!attackGruntClips.empty()) LOG_INFO("  First attack: ", attackGruntClips[0].path);
    if (!woundClips.empty()) LOG_INFO("  First wound: ", woundClips[0].path);
    if (attackGruntClips.empty() && woundClips.empty()) {
        LOG_WARNING("No combat vocal sounds found for ", stem);
        LOG_WARNING("  Tried ", exertPrefixes_.size(), " exert prefixes, first: ",
                    exertPrefixes_.empty() ? "(none)" : exertPrefixes_[0]);
        LOG_WARNING("  Tried vox prefix: ", voxPrefix);
    }
}

void ActivitySoundManager::playAttackGrunt() {
    if (!AudioEngine::instance().isInitialized() || attackGruntClips.empty()) return;
    auto now = std::chrono::steady_clock::now();
    if (lastAttackGruntAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastAttackGruntAt).count() < 1.5f) return;
    }
    // ~30% chance per swing to grunt (not every hit)
    std::uniform_int_distribution<int> chance(0, 9);
    if (chance(rng) > 2) return;

    std::uniform_int_distribution<size_t> dist(0, attackGruntClips.size() - 1);
    const Sample& sample = attackGruntClips[dist(rng)];
    float volume = 0.0f, pitch = 1.0f;
    askedByRow(sample, 0.62f, volume, pitch);
    if (AudioEngine::instance().playSound2D(sample.data, volume, pitch)) {
        lastAttackGruntAt = now;
    }
}

void ActivitySoundManager::playWound(bool isCrit) {
    if (!AudioEngine::instance().isInitialized()) return;
    auto& clips = (isCrit && !woundCritClips.empty()) ? woundCritClips : woundClips;
    if (clips.empty()) return;
    auto now = std::chrono::steady_clock::now();
    if (lastWoundAt.time_since_epoch().count() != 0) {
        if (std::chrono::duration<float>(now - lastWoundAt).count() < 0.8f) return;
    }
    std::uniform_int_distribution<size_t> dist(0, clips.size() - 1);
    const Sample& sample = clips[dist(rng)];
    float volume = 0.0f, pitch = 1.0f;
    askedByRow(sample, isCrit ? 0.80f : 0.65f, volume, pitch);
    if (AudioEngine::instance().playSound2D(sample.data, volume, pitch)) {
        lastWoundAt = now;
    }
}
} // namespace audio
} // namespace wowee

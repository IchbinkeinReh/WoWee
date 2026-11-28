#include "audio/sound_names.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_distance.hpp"
#include "audio/sound_entry.hpp"
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

using sound_entry::leafLower;

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

    indexSoundEntriesReach();
    indexVoiceKits();
    LOG_INFO("NPC voice manager initialized (", kitRows_.size(), " kits)");
    return true;
}

void NpcVoiceManager::shutdown() {
    lineHandle_.clear();
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
    const uint32_t kitId = display->second;

    // Read once per kit, however many units share the display - all three of
    // its slots at once, so the greeting asked first does not leave the
    // farewell and the irritated line cached empty. A slot with no clip in it
    // stays empty, so it is not gone looking for again per click.
    auto loaded = kitSamples_.find(kitId);
    if (loaded == kitSamples_.end()) {
        const auto row = kitRows_.find(kitId);
        auto se = assetManager_->loadDBCOptional("SoundEntries.dbc");
        const bool rowsHere = row != kitRows_.end() && se && se->isLoaded() && se->getFieldCount() > 23;
        // Streaming mode: a kit whose clips are still on the server is not
        // waited for (a frozen frame at a click); this line is not spoken and
        // the next ask reads on (pipeline/no_wait_reads.hpp).
        if (rowsHere) {
            std::vector<std::string> paths;
            for (size_t k = 0; k < 3; ++k) {
                if (row->second[k] == 0) continue;
                const auto entry = soundEntryRowById_.find(row->second[k]);
                if (entry == soundEntryRowById_.end()) continue;
                const std::string directory = se->getString(entry->second, 23);
                for (uint32_t f = 0; f < 10; ++f) {
                    const std::string file = se->getString(entry->second, 3 + f);
                    if (file.empty()) continue;
                    const std::string path = sound_entry::filePath(directory, file);
                    if (assetManager_->fileExists(path)) paths.push_back(path);
                }
            }
            if (!reads_.ready(paths, [this](const std::string& p) { return assetManager_->readFile(p); }))
                return {};
        }
        loaded = kitSamples_.try_emplace(kitId).first;
        kitLastFile_[kitId].fill(-1);
        if (rowsHere) {
            for (size_t k = 0; k < 3; ++k) {
                if (row->second[k] == 0) continue;
                const auto entry = soundEntryRowById_.find(row->second[k]);
                if (entry == soundEntryRowById_.end()) continue;
                // Columns 3..12 name the files, column 23 their folder
                // (sound_entry::filePath).
                const std::string directory = se->getString(entry->second, 23);
                for (uint32_t f = 0; f < 10; ++f) {
                    const std::string file = se->getString(entry->second, 3 + f);
                    if (file.empty()) continue;
                    const std::string path = sound_entry::filePath(directory, file);
                    VoiceSample sample;
                    if (!loadSound(path, sample)) {
                        LOG_DEBUG("NPC voice kit ", kitId, " slot ", k, ": no file ", path);
                        continue;
                    }
                    LOG_DEBUG("NPC voice kit ", kitId, " slot ", k, ": ", path);
                    sample.frequency = se->getUInt32(entry->second, 13 + f);
                    loaded->second[k].push_back(std::move(sample));
                }
            }
        }
        LOG_INFO("NPC voice kit ", kitId, ": ", loaded->second[0].size(), " greeting, ",
                 loaded->second[1].size(), " farewell, ", loaded->second[2].size(), " irritated clips");
    }
    auto& samples = loaded->second[slot];
    if (samples.empty()) return {};
    return {&samples, &kitLastFile_[kitId][slot]};
}

bool NpcVoiceManager::loadSound(const std::string& path, VoiceSample& sample) {
    if (!assetManager_) return false;

    if (!assetManager_->fileExists(path)) {
        return false;
    }
    // Read by kitSlot's ready() (held there; a path asked twice in a kit is
    // read again).
    sample.data = reads_.take(path);
    if (sample.data.empty()) sample.data = assetManager_->readFile(path);

    sample.path = path;
    // The reach the SoundEntries row naming this file asks for, when a row
    // names it. One that no row names keeps the reach these plays had anyway.
    if (const auto it = reachByName_.find(leafLower(path)); it != reachByName_.end()) {
        sample.minDistance = it->second.minDistance;
        sample.cutoff = it->second.cutoff;
        sample.volume = it->second.volume;
        sample.flags = it->second.flags;
    }
    sound_names::note(sample.data, path);

    if (sample.data.empty()) {
        LOG_WARNING("NPC voice: Failed to load sound data from ", path);
        return false;
    }

    return true;
}

bool NpcVoiceManager::playSound(uint64_t npcGuid, uint32_t displayId, size_t slot, int32_t variant,
                                const glm::vec3& position) {
    if (!AudioEngine::instance().isInitialized()) return false;
    // Most displays name no kit (13754 of CreatureDisplayInfo's 24262 rows), and
    // those units say nothing: 0x00746900 answers -1 where the row or its first
    // line is missing, 0x00720860 refuses where the row is.
    const auto kit = kitSlot(displayId, slot);
    if (kit.first == nullptr) {
        // A kit the display names that is not read yet (its clips still on
        // the streaming server): the line waits for it a moment (update())
        // instead of the first click on a new NPC saying nothing (T456).
        if (const auto d = kitIdByDisplay_.find(displayId);
            d != kitIdByDisplay_.end() && kitSamples_.find(d->second) == kitSamples_.end() && variant < 0) {
            pending_ = PendingLine{npcGuid, displayId, slot, position,
                                   std::chrono::steady_clock::now() + kPendingWindow, true};
            return false;
        }
        static int toldNone = 0;
        if (toldNone < 20) {
            ++toldNone;
            LOG_INFO("NPC voice: nothing for display ", displayId, " slot ", slot,
                     kitIdByDisplay_.count(displayId) ? " (kit without a clip here, or still read)" : " (no kit)");
        }
        return false;
    }
    const auto& clips = *kit.first;
    size_t chosen = 0;
    if (variant >= 0) {
        if (static_cast<size_t>(variant) >= clips.size()) return false;
        chosen = static_cast<size_t>(variant);
    } else {
        // By the rows' weights (0x004cbac0), not the file given last while
        // another has weight (the SoundEntries record's +0x44).
        std::vector<uint32_t> frequency;
        frequency.reserve(clips.size());
        for (const auto& candidate : clips) frequency.push_back(candidate.frequency);
        std::optional<size_t> lastFile;
        if (kit.second != nullptr && *kit.second >= 0) lastFile = static_cast<size_t>(*kit.second);
        const uint32_t roll = std::uniform_int_distribution<uint32_t>(0, UINT32_MAX)(rng_);
        if (const auto picked = zone_music::pickFile(frequency, lastFile, roll); picked.has_value()) {
            chosen = *picked;
            if (kit.second != nullptr) *kit.second = static_cast<int32_t>(chosen);
        } else {
            chosen = std::uniform_int_distribution<size_t>(0, clips.size() - 1)(rng_);
        }
    }
    const VoiceSample* sample = &clips[chosen];

    // Play sound at the reach the clip's own row asks for. A voice line is a
    // sound of the few yards round the unit saying it: 8 yards at full volume
    // inside a 45-yard carry for every row behind NPCSounds, where this asked
    // for 60 and never named a core at all. A clip no row names keeps the 60,
    // rather than the wider reach the helper assumes of a row that named none.
    float minDistance = 1.0f, maxDistance = 60.0f;
    if (sample->cutoff > 0.0f)
        sound_distance::entryDistance(sample->minDistance, sample->cutoff, minDistance, maxDistance);
    // Whether the line swings in pitch or in volume at all is the row's to say,
    // not ours, and what a swing costs is the same for every play of a row
    // (audio/sound_entry.hpp). Of the 12941 rows 126 ask for a pitch and 7 of
    // those also for a volume, and of the 527 these kits reach **none asks
    // either** - which is why the pitch jitter this used to put on every line,
    // 0.98-1.02, was its own invention and not the client's: it made the same clip
    // sound different from one click to the next. A voice line plays at the pitch
    // it was recorded at.
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float pitch = sound_entry::pitchForFlags(sample->flags, unit(rng_));
    const float volume = sound_entry::volumeForFlags(sample->flags, sample->volume * volumeScale_, unit(rng_));

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
    // Said for the first plays of a session: whether a line went out, and if
    // the engine refused it, what it had to go on (T456: lines not seen).
    static int told = 0;
    if (told < 40) {
        ++told;
        const auto& engine = AudioEngine::instance();
        const glm::vec3 d = position - engine.getListenerPosition();
        LOG_INFO("NPC voice: ", sample->path, success ? " played" : " NOT played", " (slot ", slot,
                 ", display ", displayId, ", distance ", glm::length(d), " of reach ", maxDistance,
                 ", volume ", volume, ", master ", engine.getMasterVolume(),
                 ", world sound held ", engine.isWorldSoundHeld() ? 1 : 0, ")");
    }
    return success;
}

bool NpcVoiceManager::linePlaying(uint64_t npcGuid) const {
    const auto it = lineHandle_.find(npcGuid);
    return it != lineHandle_.end() && it->second != 0 &&
           AudioEngine::instance().isSoundPlaying(it->second);
}

void NpcVoiceManager::playClick(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position) {
    // 0x0072a680: nothing while the unit's line is still going, and its counter
    // then does not move.
    if (linePlaying(npcGuid)) return;
    const auto irritated = kitSlot(displayId, 2);
    const auto ask = counter_.ask(npcGuid, irritated.first ? irritated.first->size() : 0);
    if (playSound(npcGuid, displayId, ask.irritated ? 2 : 0, ask.irritated ? static_cast<int32_t>(ask.line) : -1, position))
        counter_.started(ask);
}

void NpcVoiceManager::update() {
    if (!pending_.active) return;
    if (std::chrono::steady_clock::now() > pending_.until || linePlaying(pending_.guid)) {
        pending_.active = false;
        return;
    }
    const auto d = kitIdByDisplay_.find(pending_.displayId);
    if (d == kitIdByDisplay_.end()) {
        pending_.active = false;
        return;
    }
    const PendingLine line = pending_;
    pending_.active = false;
    // Asks for the kit's reads again; still not in, and playSound keeps it
    // pending with a fresh deadline - so the deadline of the first ask holds.
    playSound(line.guid, line.displayId, line.slot, -1, line.position);
    if (pending_.active) pending_.until = line.until;
}

void NpcVoiceManager::playHello(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position) {
    counter_.forget();
    if (!linePlaying(npcGuid)) playSound(npcGuid, displayId, 0, -1, position);
}

void NpcVoiceManager::playFarewell(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position) {
    counter_.forget();
    if (!linePlaying(npcGuid)) playSound(npcGuid, displayId, 1, -1, position);
}

} // namespace audio
} // namespace wowee

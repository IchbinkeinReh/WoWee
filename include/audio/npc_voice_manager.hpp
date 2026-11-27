#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <random>
#include <glm/glm.hpp>

#include "audio/npc_line_counter.hpp"
#include "pipeline/no_wait_reads.hpp"

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
    /// The SoundEntries row's Freq weight for this file (column 13+slot), 0 from
    /// a race library. A kit's files are asked by those weights, as the client
    /// asks any kit's file (0x004cbac0).
    uint32_t frequency = 0;
    /// The row's Flags (column 25), which is what decides whether the play swings
    /// in pitch or in volume at all - see playSound.
    uint32_t flags = 0;
    /// The row's VolumeFloat (column 24), 1.0 when no row names the clip. The
    /// client plays a voice line at that times the ask's own volume, and the rows
    /// behind these kits are not all 1.0: of the 527 they reach, 268 ask less -
    /// 0.8, 0.69, 0.5, 0.45 or 0.2.
    float volume = 1.0f;
};

class NpcVoiceManager {
public:
    NpcVoiceManager();
    ~NpcVoiceManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    // The display id is the unit's own: its CreatureDisplayInfo row names the NPCSounds
    // kit the line is taken from, or none.
    /// The line of a click at a unit (0x0072a680).
    void playClick(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position);
    /// The hello as a window opens with it, and the farewell (0x00720860).
    void playHello(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position);
    void playFarewell(uint64_t npcGuid, uint32_t displayId, const glm::vec3& position);

    void setVolumeScale(float scale) { volumeScale_ = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

private:
    /// Which kit each display names, and the rows behind it: see the comment on
    /// the definition for what the columns are and how they were established.
    void indexVoiceKits();
    /// The line a display's kit holds for a slot - 0 the interaction line, 1 the
    /// farewell, 2 the irritated one - loaded the first time a unit of that
    /// display asks for it. Null when the display names no kit, or that slot is
    /// empty in the kit it names; with it, where to remember which file of the
    /// slot was asked last, or null when there is no slot to remember it for.
    std::pair<const std::vector<VoiceSample>*, int32_t*> kitSlot(uint32_t displayId, size_t slot);
    /// Every file the SoundEntries rows name, once, with that row's MinDistance
    /// (26) and DistanceCutoff (27) - so a clip carries the reach its own row
    /// asks for instead of one guessed here.
    void indexSoundEntriesReach();
    bool loadSound(const std::string& path, VoiceSample& sample);
    /// Whether a line this manager started for that unit is still going.
    [[nodiscard]] bool linePlaying(uint64_t npcGuid) const;
    /// A kit's line of a slot played at the unit. `variant` is the client's
    /// variant index: a kit alternate picked by position rather than by weight,
    /// which is how its third slot is walked once each; -1 leaves the choice to the
    /// kit's weights. False when nothing started, as for a display that names no
    /// kit (0x00746900's -1).
    bool playSound(uint64_t npcGuid, uint32_t displayId, size_t slot, int32_t variant, const glm::vec3& position);

    pipeline::AssetManager* assetManager_ = nullptr;
    float volumeScale_ = 1.0f;

    // What a clip's own SoundEntries row asks of the play, by the lower-case
    // file name it names the clip by.
    struct EntryAsk {
        float minDistance = 0.0f;
        float cutoff = 0.0f;
        float volume = 1.0f;
        uint32_t flags = 0;
    };
    std::unordered_map<std::string, EntryAsk> reachByName_;

    // The kit a display names, and the rows behind it (indexVoiceKits).
    std::unordered_map<uint32_t, uint32_t> kitIdByDisplay_;
    std::unordered_map<uint32_t, std::array<uint32_t, 3>> kitRows_;
    std::unordered_map<uint32_t, uint32_t> soundEntryRowById_;
    // A kit's loaded clips by NPCSounds id, so a line is read from disk once
    // however many units share the display; an empty slot stays empty.
    std::unordered_map<uint32_t, std::array<std::vector<VoiceSample>, 3>> kitSamples_;
    // Clips of kits still coming from the streaming server.
    pipeline::NoWaitReads reads_;
    // Which file of a kit slot was asked last (-1 for none), so the next ask is
    // not the same line while another one has weight. The client keeps this on the
    // SoundEntries record it is picking from (+0x44); it is kept per kit slot here
    // because a kit's slot is what is picked from.
    std::unordered_map<uint32_t, std::array<int32_t, 3>> kitLastFile_;

    // One line per unit at a time: the client asks the unit's own sound slot
    // (unit + 0x8f4, 0x004c5be0) before any line and keeps no timer.
    std::unordered_map<uint64_t, uint32_t> lineHandle_;
    NpcLineCounter counter_;

    std::mt19937 rng_;
};

} // namespace audio
} // namespace wowee

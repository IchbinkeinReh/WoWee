#include "audio/advanced_kit.hpp"

#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee::audio::advanced_kit {
namespace {

/// SoundEntries id -> the kit id its column 29 names, keeping only the rows whose
/// kit the table really has: the client resolves that column to a record pointer as
/// it flattens the row (0x004cd4a0 filling record+0xac, 004c0000.c:11520-11655), and
/// a play only takes the advanced branch while that pointer is non-null
/// (004c0000.c:5954). All 1617 naming rows resolve in this install; the 26 table rows
/// no row names are simply never asked about.
std::unordered_map<uint32_t, uint32_t> kitsBySound;
bool kitsLoaded = false;

/// The advanced plays still going, as the client keeps its own (DAT_00b4ad2c, walked
/// by 0x004ca130): the kit id, and the position the play started at, which is what
/// the distance is measured from.
struct LivePlay {
    uint32_t kit = 0;
    glm::vec3 at{};
    uint32_t handle = 0;
};
std::vector<LivePlay> livePlays;

}  // namespace

uint32_t kitIdFor(uint32_t soundEntriesId, pipeline::AssetManager* assets) {
    if (!kitsLoaded && assets) {
        kitsLoaded = true;
        auto advanced = assets->loadDBCOptional("SoundEntriesAdvanced.dbc");
        auto entries = assets->loadDBCOptional("SoundEntries.dbc");
        if (advanced && advanced->isLoaded() && entries && entries->isLoaded() &&
            entries->getFieldCount() > 29) {
            std::unordered_set<uint32_t> tableIds;
            tableIds.reserve(advanced->getRecordCount());
            for (uint32_t i = 0; i < advanced->getRecordCount(); ++i)
                tableIds.insert(advanced->getUInt32(i, 0));
            for (uint32_t row = 0; row < entries->getRecordCount(); ++row) {
                const uint32_t kit = entries->getUInt32(row, 29);
                if (kit != 0 && tableIds.contains(kit))
                    kitsBySound.emplace(entries->getUInt32(row, 0), kit);
            }
        }
    }
    const auto it = kitsBySound.find(soundEntriesId);
    return it != kitsBySound.end() ? it->second : 0;
}

bool refusedNear(uint32_t kitId, const glm::vec3& at) {
    if (kitId == 0) return false;
    auto& engine = AudioEngine::instance();
    // An instance leaves the client's list as its sound ends; asking the engine the
    // same question it answers for a unit's one-at-a-time line (0x004c5be0) keeps
    // ours the same length.
    std::erase_if(livePlays, [&](const LivePlay& p) { return !engine.isSoundPlaying(p.handle); });
    return std::any_of(livePlays.begin(), livePlays.end(), [&](const LivePlay& p) {
        const glm::vec3 d = p.at - at;
        return p.kit == kitId && glm::dot(d, d) < 6.0f;
    });
}

void notePlay(uint32_t kitId, const glm::vec3& at, uint32_t handle) {
    if (kitId == 0 || handle == 0) return;
    livePlays.push_back(LivePlay{.kit = kitId, .at = at, .handle = handle});
}

}  // namespace wowee::audio::advanced_kit

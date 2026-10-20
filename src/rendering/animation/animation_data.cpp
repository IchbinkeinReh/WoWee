#include "rendering/animation/animation_data.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee {
namespace rendering {
namespace anim {

namespace {
// Set once at world entry, read from the main thread's animation calls.
std::shared_ptr<const AnimationDataTable>& loadedTable() {
    static std::shared_ptr<const AnimationDataTable> table;
    return table;
}
}  // namespace

const AnimationDataTable& animationData() {
    static const AnimationDataTable empty;
    const auto& table = loadedTable();
    return table ? *table : empty;
}

void setAnimationData(std::shared_ptr<const AnimationDataTable> table) {
    loadedTable() = std::move(table);
}

std::shared_ptr<const AnimationDataTable> loadAnimationData(const pipeline::DBCFile& dbc) {
    // ID, Name, WeaponFlags, BodyFlags, Flags, Fallback, BehaviorID,
    // BehaviorTier: the eight columns 3.3.5 ships.
    if (dbc.getFieldCount() < 8) return nullptr;
    std::vector<AnimationDataRecord> rows;
    rows.reserve(dbc.getRecordCount());
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        rows.push_back({.id = dbc.getUInt32(i, 0),
                        .flags = dbc.getUInt32(i, 4),
                        .fallback = dbc.getUInt32(i, 5),
                        .behavior = dbc.getUInt32(i, 6),
                        .tier = dbc.getUInt32(i, 7)});
    }
    return std::make_shared<const AnimationDataTable>(std::move(rows));
}

}  // namespace anim
}  // namespace rendering
}  // namespace wowee

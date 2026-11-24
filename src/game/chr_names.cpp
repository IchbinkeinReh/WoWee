#include "game/chr_names.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

namespace wowee {
namespace game {

namespace {

// Name, then Name_female and Name_male, seventeen columns each.
GenderedName rowName(pipeline::AssetManager* assets, const char* file, const char* layoutName, uint32_t id,
                     int gender) {
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* layout = layouts ? layouts->getLayout(layoutName) : nullptr;
    const uint32_t nameF = layout ? layout->tryField("Name") : 0xFFFFFFFFu;
    auto dbc = assets && assets->isInitialized() ? assets->loadDBCOptional(file) : nullptr;
    if (!dbc || !dbc->isLoaded() || nameF == 0xFFFFFFFFu || nameF + 34 >= dbc->getFieldCount()) return {};
    const int32_t row = dbc->findRecordById(id);
    if (row < 0) return {};
    const auto r = static_cast<uint32_t>(row);
    return pickGenderedName(dbc->getString(r, nameF), dbc->getString(r, nameF + 17), dbc->getString(r, nameF + 34),
                            gender);
}

}  // namespace

GenderedName chrRaceName(pipeline::AssetManager* assets, uint32_t raceId, int gender) {
    return rowName(assets, "ChrRaces.dbc", "ChrRaces", raceId, gender);
}

GenderedName chrClassName(pipeline::AssetManager* assets, uint32_t classId, int gender) {
    return rowName(assets, "ChrClasses.dbc", "ChrClasses", classId, gender);
}

}  // namespace game
}  // namespace wowee

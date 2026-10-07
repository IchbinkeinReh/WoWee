#include "core/character_geosets.hpp"

#include "core/helm_visual.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <unordered_set>

namespace wowee {
namespace core {

ItemGeosets itemGeosets(pipeline::AssetManager& assets, uint32_t displayInfoId) {
    ItemGeosets out;
    if (displayInfoId == 0) return out;
    auto dbc = assets.loadDBC("ItemDisplayInfo.dbc");
    if (!dbc || !dbc->isLoaded()) return out;
    const int32_t row = dbc->findRecordById(displayInfoId);
    if (row < 0) return out;
    const auto r = static_cast<uint32_t>(row);
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t gg1 = layout ? (*layout)["GeosetGroup1"] : 7u;
    const uint32_t gg3 = layout ? (*layout)["GeosetGroup3"] : 9u;
    if (gg1 == 0xFFFFFFFFu || gg3 == 0xFFFFFFFFu || gg3 + 1 >= dbc->getFieldCount()) return out;
    out.worn = true;
    out.group = {dbc->getUInt32(r, gg1), dbc->getUInt32(r, gg1 + 1), dbc->getUInt32(r, gg3)};
    out.flags = dbc->getUInt32(r, gg3 + 1);  // Flags follows the three groups
    // Regions 1 to 6: lower arm, hand, upper and lower torso, upper and
    // lower leg.
    for (const char* region : {"TextureArmLower", "TextureHand", "TextureTorsoUpper",
                               "TextureTorsoLower", "TextureLegUpper", "TextureLegLower"}) {
        const uint32_t f = layout ? (*layout)[region] : 0xFFFFFFFFu;
        if (f != 0xFFFFFFFFu && f < dbc->getFieldCount() && !dbc->getString(r, f).empty()) {
            out.texturesArmsOrBody = true;
            break;
        }
    }
    return out;
}

CharacterEquipmentGeosets equipmentGeosets(pipeline::AssetManager& assets, const WornDisplays& worn,
                                           CharacterGeosetFlags* flags) {
    CharacterEquipmentGeosets eq;
    eq.shirt = itemGeosets(assets, worn.shirt);
    eq.chest = itemGeosets(assets, worn.chest);
    eq.belt = itemGeosets(assets, worn.belt);
    eq.legs = itemGeosets(assets, worn.legs);
    eq.boots = itemGeosets(assets, worn.boots);
    eq.gloves = itemGeosets(assets, worn.gloves);
    eq.tabard = itemGeosets(assets, worn.tabard);
    eq.cape = itemGeosets(assets, worn.cape);
    if (flags) flags->chestOwnsLegs = eq.chest.worn && (eq.chest.flags & 0x4) != 0;
    return eq;
}

bool faceHasDeathKnightGlow(pipeline::AssetManager& assets, uint8_t raceId, uint8_t genderId,
                            uint8_t faceId, uint8_t skinId) {
    auto dbc = assets.loadDBC("CharSections.dbc");
    if (!dbc || !dbc->isLoaded()) return false;
    static const pipeline::DBCFile* cachedFor = nullptr;
    static std::unordered_set<uint32_t> glowing;
    if (cachedFor != dbc.get()) {
        glowing.clear();
        const auto* csL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        const auto f = pipeline::detectCharSectionsFields(dbc.get(), csL);
        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
            if (dbc->getUInt32(r, f.baseSection) != 1) continue;
            if ((dbc->getUInt32(r, f.flags) & 0x4) == 0) continue;
            glowing.insert((dbc->getUInt32(r, f.raceId) & 0xFF) << 24 |
                           (dbc->getUInt32(r, f.sexId) & 0xFF) << 16 |
                           (dbc->getUInt32(r, f.variationIndex) & 0xFF) << 8 |
                           (dbc->getUInt32(r, f.colorIndex) & 0xFF));
        }
        cachedFor = dbc.get();
    }
    return glowing.count(static_cast<uint32_t>(raceId) << 24 | static_cast<uint32_t>(genderId) << 16 |
                         static_cast<uint32_t>(faceId) << 8 | skinId) != 0;
}

AppearanceGeosetTables loadAppearanceGeosetTables(pipeline::AssetManager& assets) {
    AppearanceGeosetTables t;
    if (auto chg = assets.loadDBC("CharHairGeosets.dbc"); chg && chg->isLoaded()) {
        const auto* chgL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharHairGeosets") : nullptr;
        for (uint32_t i = 0; i < chg->getRecordCount(); i++) {
            const uint32_t race = chg->getUInt32(i, chgL ? (*chgL)["RaceID"] : 1);
            const uint32_t sex = chg->getUInt32(i, chgL ? (*chgL)["SexID"] : 2);
            const uint32_t variation = chg->getUInt32(i, chgL ? (*chgL)["Variation"] : 3);
            const uint32_t geoset = chg->getUInt32(i, chgL ? (*chgL)["GeosetID"] : 4);
            // The first row for a key, as 0x004ea050's scan finds it.
            t.hair.emplace(appearanceKey(static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                                         static_cast<uint8_t>(variation)),
                           geoset);
        }
    }
    if (auto cfh = assets.loadDBC("CharacterFacialHairStyles.dbc"); cfh && cfh->isLoaded()) {
        const auto* cfhL = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
        const auto f = pipeline::detectFacialHairFields(cfh.get(), cfhL);
        auto column = [&](uint32_t row, uint32_t field) -> uint32_t {
            return field < cfh->getFieldCount() ? cfh->getUInt32(row, field) : 0;
        };
        for (uint32_t i = 0; i < cfh->getRecordCount(); i++) {
            const uint32_t race = cfh->getUInt32(i, cfhL ? (*cfhL)["RaceID"] : 0);
            const uint32_t sex = cfh->getUInt32(i, cfhL ? (*cfhL)["SexID"] : 1);
            const uint32_t variation = cfh->getUInt32(i, cfhL ? (*cfhL)["Variation"] : 2);
            FacialGeosetColumns c;
            c.column = {column(i, f.geoset100), column(i, f.geoset300), column(i, f.geoset200),
                        column(i, f.geoset1600), column(i, f.geoset1700)};
            t.facial.emplace(appearanceKey(static_cast<uint8_t>(race), static_cast<uint8_t>(sex),
                                           static_cast<uint8_t>(variation)),
                             c);
        }
    }
    return t;
}

std::unordered_set<uint16_t> characterLookGeosets(pipeline::AssetManager& assets,
                                                  const CharacterLook& look) {
    CharacterGeosetDefaults d =
        characterGeosetDefaults(look.hairGeoset, look.facial ? &*look.facial : nullptr);
    if (look.worn.head != 0) {
        if (auto masks = helmetGeosetVisMasks(assets, look.worn.head, look.genderId)) {
            applyHelmetGeosetVis(d, *masks, look.raceId);
        }
    }
    CharacterGeosetFlags flags;
    const CharacterEquipmentGeosets eq = equipmentGeosets(assets, look.worn, &flags);
    const bool glow = look.classId == kClassDeathKnight ||
                      faceHasDeathKnightGlow(assets, look.raceId, look.genderId, look.faceId, look.skinId);
    return characterGeosets(d, glow, eq, flags);
}

}  // namespace core
}  // namespace wowee

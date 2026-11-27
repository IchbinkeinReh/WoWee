// poi_row.hpp - one AreaPOI.dbc row as the world map keeps it.
//
// Kept apart from DataRepository (which needs the asset manager) so a test can
// hand it a file built in memory and see what each column becomes.
#pragma once

#include "rendering/world_map/world_map_types.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/coordinates.hpp"

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {
namespace world_map {

/// AreaPOI's columns, from the active layout or 3.3.5a's: 0 ID,
/// 1 Importance, 2-10 Icon[9], 11 FactionID, 12-14 X/Y/Z, 15 ContinentID,
/// 16 Flags, 17 AreaID, 18-34 Name_lang, 35-51 Description_lang,
/// 52 WorldStateID. The name is 18, not 16 - 16 is Flags, read as a string
/// offset it named every POI wrongly or not at all.
struct PoiColumns {
    uint32_t id = 0, importance = 1, icon = 2, faction = 11;
    uint32_t x = 12, y = 13, z = 14, map = 15, flags = 16, area = 17;
    uint32_t state = 52, name = 18, description = 35;

    static PoiColumns of(const pipeline::DBCFieldMap* l) {
        PoiColumns c;
        if (!l) return c;
        c.id = (*l)["ID"];
        c.importance = (*l)["Importance"];
        c.icon = (*l)["Icon0"];
        c.faction = (*l)["FactionID"];
        c.x = (*l)["X"];
        c.y = (*l)["Y"];
        c.z = (*l)["Z"];
        c.map = (*l)["ContinentID"];
        c.flags = (*l)["Flags"];
        c.area = (*l)["AreaID"];
        c.state = l->tryField("WorldStateID");
        c.name = (*l)["Name"];
        // T414: the description block follows the name's sixteen locales and
        // its flags column. An installed layout copied before the column was
        // named does not refresh, so one without it takes that place.
        c.description = l->tryField("Description");
        if (c.description == 0xFFFFFFFFu && c.name != 0xFFFFFFFFu) c.description = c.name + 17;
        return c;
    }
};

/// Row `i` of AreaPOI as 12340 copies it into its POI record (0x00ad312c,
/// stride 0x58): flags at +0x40, area +0x44, name +0x48, description +0x4c
/// (GetMapLandmarkInfo's second value, 0x00545a30), world state +0x50.
inline POI readPoiRow(const pipeline::DBCFile& dbc, uint32_t i, const PoiColumns& c) {
    const uint32_t fieldCount = dbc.getFieldCount();
    POI poi;
    poi.id = dbc.getUInt32(i, c.id);
    poi.importance = dbc.getUInt32(i, c.importance);
    poi.iconType = dbc.getUInt32(i, c.icon);
    poi.factionId = dbc.getUInt32(i, c.faction);
    // The file's axes are the wire's. Taken as canonical, every point's
    // two were exchanged: Bloodhoof at 85/3 of Mulgore's map.
    const glm::vec3 place = core::coords::serverToCanonical(
        glm::vec3(dbc.getFloat(i, c.x), dbc.getFloat(i, c.y), dbc.getFloat(i, c.z)));
    poi.wowX = place.x;
    poi.wowY = place.y;
    poi.wowZ = place.z;
    poi.mapId = dbc.getUInt32(i, c.map);
    // A layout that carries no Flags column reads as no flags, which costs
    // the interface's battle-map pin test (lua_system_api.cpp) and nothing
    // else - the map itself draws these pins without asking the flag.
    poi.flags = c.flags < fieldCount ? dbc.getUInt32(i, c.flags) : 0;
    poi.areaId = c.area < fieldCount ? dbc.getUInt32(i, c.area) : 0;
    poi.worldStateId = c.state < fieldCount ? dbc.getUInt32(i, c.state) : 0;
    for (uint32_t k = 0; k < 9 && c.icon + k < fieldCount; ++k) poi.icons[k] = dbc.getInt32(i, c.icon + k);
    poi.name = dbc.getString(i, c.name);
    // T414: the second line of a pin's tooltip ('Umkämpft' under 'Hof' in
    // Arathi Basin) - never read, so every description was empty.
    if (c.description < fieldCount) poi.description = dbc.getString(i, c.description);
    return poi;
}

} // namespace world_map
} // namespace rendering
} // namespace wowee

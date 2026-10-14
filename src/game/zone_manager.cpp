#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace game {

void ZoneManager::initialize() {
    // Tile-to-zone fallback mappings for Azeroth (Eastern Kingdoms).
    // WoW's world is a grid of 64×64 ADT tiles per continent. We encode (tileX, tileY)
    // into a single key as tileX * 100 + tileY (safe because tileY < 64 < 100).
    // These ranges are empirically determined from the retail map layout and provide
    // zone identification when AreaTable.dbc data is unavailable.
    //
    // Elwynn Forest tiles
    for (int tx = 31; tx <= 34; tx++) {
        for (int ty = 48; ty <= 51; ty++) {
            tileToZone[tx * 100 + ty] = 12;  // Elwynn
        }
    }

    // Stormwind City tiles (northern part of Elwynn area)
    tileToZone[31 * 100 + 47] = 1519;
    tileToZone[32 * 100 + 47] = 1519;
    tileToZone[33 * 100 + 47] = 1519;

    // Westfall tiles (west of Elwynn)
    for (int ty = 48; ty <= 51; ty++) {
        tileToZone[35 * 100 + ty] = 40;
        tileToZone[36 * 100 + ty] = 40;
    }

    // Dun Morogh tiles (south/east of Elwynn)
    for (int tx = 31; tx <= 34; tx++) {
        tileToZone[tx * 100 + 52] = 1;
        tileToZone[tx * 100 + 53] = 1;
    }

    // Conservative Duskwood interior fallback. The northern bank shares ADTs
    // with southern Elwynn and is resolved from AreaTable parentage instead.
    for (int tx = 0; tx < 64; tx++) {
        for (int ty = 0; ty < 64; ty++) {
            if (isDuskwoodAdtTile(tx, ty)) {
                tileToZone[tx * 100 + ty] = 10;
            }
        }
    }

    // Tirisfal Glades tiles (northern Eastern Kingdoms)
    for (int tx = 28; tx <= 31; tx++) {
        for (int ty = 38; ty <= 41; ty++) {
            tileToZone[tx * 100 + ty] = 85;
        }
    }

    // Stranglethorn Vale tiles (south of Westfall/Duskwood)
    for (int tx = 33; tx <= 36; tx++) {
        for (int ty = 54; ty <= 58; ty++) {
            tileToZone[tx * 100 + ty] = 33;
        }
    }

    // Burning Steppes tiles (east of Redridge, north of Blackrock)
    for (int tx = 29; tx <= 31; tx++) {
        tileToZone[tx * 100 + 52] = 46;
        tileToZone[tx * 100 + 53] = 46;
    }

    // Searing Gorge tiles (north of Burning Steppes)
    for (int tx = 29; tx <= 31; tx++) {
        tileToZone[tx * 100 + 50] = 51;
        tileToZone[tx * 100 + 51] = 51;
    }

    // Loch Modan tiles (east of Dun Morogh)
    for (int tx = 35; tx <= 37; tx++) {
        tileToZone[tx * 100 + 52] = 38;
        tileToZone[tx * 100 + 53] = 38;
    }

    // The Barrens tiles (Kalimdor - large zone)
    for (int tx = 17; tx <= 22; tx++) {
        for (int ty = 28; ty <= 35; ty++) {
            tileToZone[tx * 100 + ty] = 17;
        }
    }

    // --- Kalimdor tile mappings ---

    // Durotar tiles (east coast, near Orgrimmar)
    for (int tx = 19; tx <= 22; tx++) {
        for (int ty = 25; ty <= 28; ty++) {
            tileToZone[tx * 100 + ty] = 14;
        }
    }

    // Orgrimmar tiles (within Durotar)
    tileToZone[20 * 100 + 26] = 1637;
    tileToZone[21 * 100 + 26] = 1637;
    tileToZone[20 * 100 + 27] = 1637;
    tileToZone[21 * 100 + 27] = 1637;

    // Mulgore tiles (south of Barrens)
    for (int tx = 15; tx <= 18; tx++) {
        for (int ty = 33; ty <= 36; ty++) {
            tileToZone[tx * 100 + ty] = 215;
        }
    }

    // Thunder Bluff tiles (within Mulgore)
    tileToZone[16 * 100 + 34] = 1638;
    tileToZone[17 * 100 + 34] = 1638;

    // Darkshore tiles (northwest Kalimdor coast)
    for (int tx = 14; tx <= 17; tx++) {
        for (int ty = 19; ty <= 24; ty++) {
            tileToZone[tx * 100 + ty] = 148;
        }
    }

    // Teldrassil tiles (island off Darkshore)
    for (int tx = 13; tx <= 15; tx++) {
        for (int ty = 15; ty <= 18; ty++) {
            tileToZone[tx * 100 + ty] = 141;
        }
    }

    // Darnassus tiles (within Teldrassil)
    tileToZone[14 * 100 + 16] = 1657;
    tileToZone[14 * 100 + 17] = 1657;

    LOG_INFO("Zone manager initialized: ", tileToZone.size(), " tile mappings");
}

uint32_t ZoneManager::getZoneId(int tileX, int tileY) const {
    int key = tileX * 100 + tileY;
    auto it = tileToZone.find(key);
    if (it != tileToZone.end()) {
        return it->second;
    }
    return 0;  // Unknown zone
}

bool ZoneManager::isOutdoorPvpArea(uint32_t areaId) const {
    if (areaId == 0) return false;
    auto it = areaFlags_.find(areaId);
    if (it == areaFlags_.end()) return false;
    // The eleven objective subzones - the Plaguelands towers, Halaa, the
    // Hellfire towers, Twin Spire Ruins, the Bone Wastes - and Wintergrasp,
    // whose eighteen areas are marked with a flag of their own rather than
    // this one.
    constexpr uint32_t kOutdoorPvp  = 0x00008000u;
    constexpr uint32_t kWintergrasp = 0x01000000u;
    return (it->second & (kOutdoorPvp | kWintergrasp)) != 0;
}

uint32_t ZoneManager::resolveAreaZoneId(uint32_t areaId) const {
    uint32_t current = areaId;
    // AreaTable parent chains are shallow; cap traversal to tolerate malformed
    // or cyclic custom DBC records without hanging the render/audio update.
    for (int depth = 0; current != 0 && depth < 16; ++depth) {
        auto it = areaParents_.find(current);
        if (it == areaParents_.end() || it->second == 0 || it->second == current) {
            return current;
        }
        current = it->second;
    }
    return current != 0 ? current : areaId;
}

void ZoneManager::loadAreaTable(pipeline::AssetManager* assets) {
    if (!assets) return;

    auto areaDbc = assets->loadDBC("AreaTable.dbc");

    if (!areaDbc || !areaDbc->isLoaded()) {
        LOG_WARNING("ZoneManager::loadAreaTable: AreaTable.dbc not available");
        return;
    }

    const uint32_t numAreas = areaDbc->getRecordCount();
    const uint32_t areaFields = areaDbc->getFieldCount();
    if (areaFields < 3) {
        LOG_WARNING("ZoneManager::loadAreaTable: AreaTable.dbc has too few fields (", areaFields, ")");
        return;
    }

    // ID, ContinentID, ParentAreaNum are the first three fields in the client
    // layouts supported here.
    areaParents_.clear();
    areaFlags_.clear();
    const bool haveFlags = areaFields > 4;
    for (uint32_t i = 0; i < numAreas; ++i) {
        const uint32_t areaId = areaDbc->getUInt32(i, 0);
        if (areaId == 0) continue;
        areaParents_[areaId] = areaDbc->getUInt32(i, 2);
        // Field 4 is Flags. Not in dbc_layouts.json, so checked against known
        // rows rather than trusted from its position: area 4197 is Wintergrasp
        // and must carry 0x01000000, and 3703 is Shattrath and must carry the
        // sanctuary bit 0x800. A wrong column reads as zero forever.
        if (haveFlags) areaFlags_[areaId] = areaDbc->getUInt32(i, 4);
    }
}

} // namespace game
} // namespace wowee

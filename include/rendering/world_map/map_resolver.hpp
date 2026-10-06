// map_resolver.hpp - Centralized map navigation resolution for the world map.
// Determines the correct action when clicking a region or zone at any view level.
// All functions are stateless free functions - trivially testable.
// Map folder names are resolved from a built-in table matching
// Data/interface/worldmap/ rather than WorldLoader::mapIdToName.
#pragma once

#include "rendering/world_map/world_map_types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace wowee {
namespace rendering {
namespace world_map {

// ── Map folder lookup (replaces WorldLoader::mapIdToName for world map) ──

/// Map ID → worldmap folder name (e.g. 0 → "Azeroth", 571 → "Northrend").
/// Returns empty string if unknown.
const char* mapIdToFolder(uint32_t mapId);

/// Worldmap folder name → map ID (e.g. "Azeroth" → 0, "Northrend" → 571).
/// Case-insensitive comparison. Returns -1 if unknown.
///
/// "World" and "Cosmic" are not maps and never were: they are views the
/// interface assembles out of the other maps, and they answer -1 here like
/// anything else with no map behind it. Ask isUiOnlyMapFolder to tell the two
/// apart - they used to be indistinguishable, because the sentinel standing
/// for them is UINT32_MAX and casting that to int gives exactly -1.
int folderToMapId(const std::string& folder);

/// Whether this folder names a view the interface builds rather than a map the
/// game has. Nothing can be loaded for one, and nothing is wrong when nothing
/// is.
bool isUiOnlyMapFolder(const std::string& folder);

/// Map ID → display name for UI (e.g. 0 → "Eastern Kingdoms", 571 → "Northrend").
/// Returns nullptr if unknown.
const char* mapDisplayName(uint32_t mapId);

// ── Result types ─────────────────────────────────────────────

enum class MapResolveAction {
    NONE,                ///< No valid navigation target
    NAVIGATE_CONTINENT,  ///< Switch to continent view within current map data
    LOAD_MAP,            ///< Load a different map entirely (switchToMap)
    ENTER_ZONE,          ///< Enter zone view within current continent
};

struct MapResolveResult {
    MapResolveAction action = MapResolveAction::NONE;
    int targetZoneIdx = -1;      ///< Zone index for NAVIGATE_CONTINENT or ENTER_ZONE
    std::string targetMapName;   ///< Map folder name for LOAD_MAP
};

// ── Resolve functions ────────────────────────────────────────

/// Resolve WORLD view region click. Determines whether to navigate within
/// the current map data (e.g. clicking EK when already on Azeroth) or load
/// a new map (e.g. clicking Kalimdor or Northrend from Azeroth world view).
MapResolveResult resolveWorldRegionClick(uint32_t regionMapId,
                                          const std::vector<Zone>& zones,
                                          int currentMapId,
                                          int cosmicIdx);

/// Resolve CONTINENT view zone click. Determines whether the clicked zone
/// can be entered directly (same map) or requires loading a different map
/// (zone's displayMapID differs from current).
MapResolveResult resolveZoneClick(int zoneIdx,
                                   const std::vector<Zone>& zones,
                                   int currentMapId);

/// Resolve COSMIC view map click. Always returns LOAD_MAP for the target.
MapResolveResult resolveCosmicClick(uint32_t targetMapId);

/// Find the best continent zone index to display for a given mapId within
/// the currently loaded zones. Prefers leaf continents over root continents.
/// Returns -1 if no suitable continent is found.
int findContinentForMapId(const std::vector<Zone>& zones,
                           uint32_t mapId,
                           int cosmicIdx);

// ── Reading WorldMapArea the way the client does ─────────────

/// The map a WorldMapArea row is drawn on.
///
/// DisplayMapID is -1 on every ordinary row and means "the row's own map"; a
/// value of zero or more is the continent the row is shown on instead. The
/// client reads it exactly that way (SetMapByID takes MapID when it is below
/// zero), and zero is a real answer: Eversong Woods, Ghostlands and Silvermoon
/// are on map 530 and are drawn on the Eastern Kingdoms, map 0.
///
/// A file with no -1 anywhere in the column is one that does not carry it at
/// all - vanilla's has no such field and reads as zero - and there zero means
/// nothing either. hasMinusOneRows says which of the two the file is.
uint32_t worldMapDisplayMap(uint32_t ownMapId, uint32_t rawDisplayMapId,
                            bool hasMinusOneRows);

/// The art for one tile of a WorldMapArea map. A map with dungeon floors names
/// its tiles by floor as well - Dalaran1_1 to Dalaran1_12 - which is what the
/// interface builds from GetCurrentMapDungeonLevel; one without is Name1 to
/// Name12. tileIndex is one-based, as the files are.
std::string worldMapTilePath(const std::string& folder, uint32_t dungeonFloor,
                             int tileIndex);

/// The zone a position on a map falls in, by WorldMapArea's rectangles alone:
/// the AreaTable id of the zone row of mapId that the point sits deepest
/// inside, or 0 when none holds it. wowX and wowY are canonical coordinates as
/// the client holds them - coords::serverToCanonical of what the server sent.
///
/// For a caller with only a destination - the loading screen, before any of
/// the map is there to ask.
uint32_t zoneAreaAtPosition(const std::vector<Zone>& zones, uint32_t mapId,
                            float wowX, float wowY);

/// The column of Map.dbc holding the map's name, MapName_lang, for a file of
/// fieldCount columns. 3.3.5's file has 66 and a Flags column at 3 that the
/// older ones lack, which puts the name at 5 rather than 4; read at 4 it was
/// the PvP flag, an offset of zero, and every name came out empty.
///
/// The enUS column of the block - on a localized client the loader has already
/// moved that client's own column there.
uint32_t mapDbcNameField(uint32_t fieldCount);

} // namespace world_map
} // namespace rendering
} // namespace wowee

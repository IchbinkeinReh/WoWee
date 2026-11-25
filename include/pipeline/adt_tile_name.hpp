#pragma once

/// The map and tile an ADT's path names.
///
/// A tile is stored as World\Maps\<Map>\<Map>_<X>_<Y>.adt, and the map's own
/// name may hold underscores: Azjol_LowerCity, Azjol_Uppercity, the
/// Transport_* maps. So the tile numbers are the LAST two fields of the name,
/// and the map is everything before them. Cutting at the first "_" read
/// "LowerCity" as X, failed, and fell back to tile [32,49], which those maps
/// do not have: the first tile asked for did not exist.

#include <cctype>
#include <string>

namespace wowee::pipeline {

struct AdtTileName {
    std::string mapName;
    int tileX = 0;
    int tileY = 0;
};

namespace adt_tile_name_detail {
inline bool parseTileNumber(const std::string& text, int& out) {
    if (text.empty() || text.size() > 3) return false;
    int value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
    }
    if (value > 63) return false;
    out = value;
    return true;
}
}  // namespace adt_tile_name_detail

/// Reads `path` (any directory part, "\\" or "/", extension ".adt" in any
/// case). False, and `out` untouched, when the name is not <Map>_<X>_<Y>.adt.
inline bool parseAdtTileName(const std::string& path, AdtTileName& out) {
    const size_t sep = path.find_last_of("\\/");
    std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".adt") return false;
    name.resize(name.size() - 4);

    const size_t ySep = name.rfind('_');
    if (ySep == std::string::npos || ySep == 0) return false;
    const size_t xSep = name.rfind('_', ySep - 1);
    if (xSep == std::string::npos || xSep == 0) return false;

    int x = 0;
    int y = 0;
    if (!adt_tile_name_detail::parseTileNumber(name.substr(xSep + 1, ySep - xSep - 1), x)) return false;
    if (!adt_tile_name_detail::parseTileNumber(name.substr(ySep + 1), y)) return false;
    out.mapName = name.substr(0, xSep);
    out.tileX = x;
    out.tileY = y;
    return true;
}

}  // namespace wowee::pipeline

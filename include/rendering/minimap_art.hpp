#pragma once

// The textures the minimap draws with, which the interface may replace:
// Minimap:SetMaskTexture, SetIconTexture, SetBlipTexture, SetClassBlipTexture,
// SetStaticPOIArrowTexture, SetPOIArrowTexture, SetCorpsePOIArrowTexture and
// SetPlayerTexture (0x00583860 .. 0x00583f60, 0x0057e100). The defaults are
// what the client loads at start (0x005832f0); a replacement takes effect the
// next time the minimap draws.

#include <array>
#include <cstdint>
#include <string>

namespace wowee::rendering::minimap_art {

enum Slot : int {
    Mask,            ///< DAT_00beba30
    Icons,           ///< DAT_00beba3c, the POI icons
    Blips,           ///< DAT_00beba38, the object blips
    ClassBlips,      ///< DAT_00beba34, the party and raid blips
    StaticPOIArrow,  ///< DAT_00beba24, an area POI's rim arrow
    POIArrow,        ///< DAT_00beba2c, the guide arrow
    CorpseArrow,     ///< DAT_00beba28
    Player,          ///< the player's arrow
    kCount
};

inline constexpr std::array<const char*, kCount> kDefaults = {
    "Textures\\MinimapMask.blp",
    "Interface\\Minimap\\POIIcons.blp",
    "Interface\\Minimap\\ObjectIcons.blp",
    "Interface\\Minimap\\PartyRaidBlips.blp",
    "Interface\\Minimap\\Rotating-MinimapArrow.blp",
    "Interface\\Minimap\\Rotating-MinimapGuideArrow.blp",
    "Interface\\Minimap\\Rotating-MinimapCorpseArrow.blp",
    "Interface\\Minimap\\MinimapArrow.blp",
};

struct State {
    std::array<std::string, kCount> path;
    uint32_t generation = 0;
};

inline State& state() {
    static State s;
    return s;
}

/// The file a slot draws now: the interface's replacement, else the default.
inline std::string pathOf(Slot slot) {
    const std::string& p = state().path[static_cast<size_t>(slot)];
    return p.empty() ? std::string(kDefaults[static_cast<size_t>(slot)]) : p;
}

/// A file name as the client takes it - without an extension it is a .blp.
inline std::string normalized(std::string file) {
    const size_t slash = file.find_last_of("\\/");
    const size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) file += ".blp";
    return file;
}

/// Replace a slot's file; an empty name changes nothing, as the setters
/// ignore one.
inline void set(Slot slot, const std::string& file) {
    if (file.empty()) return;
    std::string n = normalized(file);
    std::string& cur = state().path[static_cast<size_t>(slot)];
    if (cur == n) return;
    cur = std::move(n);
    ++state().generation;
}

}  // namespace wowee::rendering::minimap_art

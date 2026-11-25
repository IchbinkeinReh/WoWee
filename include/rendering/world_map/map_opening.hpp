#pragma once

namespace wowee::rendering::world_map {

/// What the map is as it is about to be drawn.
struct MapOpening {
    bool nothingLoaded = false;   ///< no map yet, or the player's is another now
    bool asked = false;           ///< to go back to the player, with the next picture
    bool firstTime = false;
    bool afterGap = false;        ///< not drawn in the picture before
    bool inInterface = false;     ///< in FrameXML's frame
    bool placed = false;          ///< SetMapToCurrentZone, SetMapZoom and the like have set it
};

/// Whether it is put on where the player is. In the interface's frame SetMapToCurrentZone does
/// that, at once (0x00547170, worldmapframe.lua's OnShow), and what is asked for after it counts:
/// the zone of a quest shown on the map was undone by the first picture.
[[nodiscard]] inline bool opensOnPlayer(const MapOpening& m) {
    if (m.nothingLoaded || m.asked) return true;
    return m.inInterface ? !m.placed : m.firstTime || m.afterGap;
}

}  // namespace wowee::rendering::world_map

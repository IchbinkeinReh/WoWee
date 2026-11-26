#pragma once

/// The liquids a placed building brings with it: every group's MLIQ, with the
/// MOMT colour an interior liquid is drawn in.
///
/// The client draws a group's liquid with the group (0x00793d20), whatever
/// placed the building: a tile's MODF or the map's WDT for a map that is one
/// building (0x007bfce0 reads the WDT; its MODF is a map object like any
/// other). Molten Core, a map of the second kind, had its lava never handed to
/// the water renderer: only the tile path placed liquids (U17).

#include "pipeline/wmo_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {
namespace wmo_liquid_load {

/// The MOMT entry MLIQ names gives an interior liquid its colour (0x00793d20
/// reads its diffuse colour, +0x1c); white when it names none.
inline uint32_t momtColor(const pipeline::WMOModel& model, const pipeline::WMOGroup& group) {
    return group.liquid.momtIndex < model.materials.size()
        ? model.materials[group.liquid.momtIndex].color2 : 0xffffffffu;
}

/// Which of an MLIQ's (xTiles + 1) x (yTiles + 1) vertices a liquid tile
/// uses: the corners of every tile whose low nibble is not 0xF. The group's
/// liquid search reads the four corners of the tile under the point, and
/// only where that tile has liquid (0x0079b760); the height of a vertex no
/// such tile uses means nothing. T376: the heights were kept only within 8
/// yards of the MLIQ's base, and Molten Core's lava stands 71 yards over its
/// base (Blackrock_lower_guild_003: base -228.9, the river -157.3), so every
/// query took the base and no one swam in it.
inline std::vector<uint8_t> usedVertices(uint32_t xTiles, uint32_t yTiles, const std::vector<uint8_t>& tileFlags) {
    const size_t xVerts = static_cast<size_t>(xTiles) + 1;
    std::vector<uint8_t> used(xVerts * (static_cast<size_t>(yTiles) + 1), 0);
    for (uint32_t ty = 0; ty < yTiles; ++ty) {
        for (uint32_t tx = 0; tx < xTiles; ++tx) {
            const size_t t = static_cast<size_t>(ty) * xTiles + tx;
            if (t < tileFlags.size() && (tileFlags[t] & 0x0F) == 0x0F) continue;
            const size_t v = static_cast<size_t>(ty) * xVerts + tx;
            used[v] = used[v + 1] = used[v + xVerts] = used[v + xVerts + 1] = 1;
        }
    }
    return used;
}

/// Hands the groups from `next` on that have a liquid to
/// `place(liquid, momtColor, groupIndex)`, at most `budget` of them, and
/// returns the next group to look at (the group count when all are done).
template <class Place>
size_t placeGroupLiquids(const pipeline::WMOModel& model, size_t next, size_t budget, Place&& place) {
    size_t placed = 0;
    while (next < model.groups.size() && placed < budget) {
        const auto& group = model.groups[next];
        const auto groupIndex = static_cast<uint32_t>(next);
        ++next;
        if (!group.liquid.hasLiquid()) continue;
        place(group.liquid, momtColor(model, group), groupIndex);
        ++placed;
    }
    return next;
}

}  // namespace wmo_liquid_load
}  // namespace rendering
}  // namespace wowee

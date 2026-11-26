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

namespace wowee {
namespace rendering {
namespace wmo_liquid_load {

/// The MOMT entry MLIQ names gives an interior liquid its colour (0x00793d20
/// reads its diffuse colour, +0x1c); white when it names none.
inline uint32_t momtColor(const pipeline::WMOModel& model, const pipeline::WMOGroup& group) {
    return group.liquid.momtIndex < model.materials.size()
        ? model.materials[group.liquid.momtIndex].color2 : 0xffffffffu;
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

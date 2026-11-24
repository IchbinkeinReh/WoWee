#pragma once

/// Which liquid a point is in, by the place it is in: the client asks one
/// place only, never whatever liquid there is over or under the point.
///
/// The camera (0x00790920 into 0x00cd8794, which 0x00780620 answers and the
/// light, the fog, the glow's wave, the liquid's sound and reverb are read
/// from): in a building's group (0x00cd87a4, 0x00795d40's) that group's liquid
/// (0x007c8360); in none, 0x007a0b00.
///
/// A unit (0x007a1bc0, 0x007c23f0 into the world object's +0x80, which
/// 0x0077f1e0 answers: its footfalls 0x0071a030, its splash, its thud, its
/// swimming): in a building's group (the floor search's) that group's
/// (0x007a1a30 -> 0x007c8360); in none, the same two.
///
/// 0x007a0b00: the exterior groups of the buildings whose box holds the point
/// first (0x007a09d0 -> 0x007aeb90), then the ground's (0x007a0820).
///
/// So a room under a lake is dry: the Coilfang rooms' groups have no liquid,
/// and the lake ninety yards over them is the ground's.

#include <cstdint>
#include <limits>
#include <optional>

namespace wowee::rendering::liquid_place {

/// The level of a group that is liquid throughout (0x007c8360 answers
/// FLT_MAX): no surface anywhere.
inline constexpr float kFilledLevel = std::numeric_limits<float>::max();

struct Liquid {
    float level = 0.0f;       ///< the surface over the point, world z
    uint32_t liquidType = 0;  ///< the LiquidType row
    bool building = false;    ///< a building group's, not the ground's
    /// Throughout the group: there is no surface to draw on.
    [[nodiscard]] bool filled() const { return level == kFilledLevel; }
};

/// `in`: the group the point is in, if any. `ofGroup(ref)` a group's liquid
/// over the point, `exteriorGroups()` those 0x007aeb90 asks, `ofGround()` the
/// ground's.
template <typename Ref, typename OfGroup, typename ExteriorGroups, typename OfGround>
std::optional<Liquid> liquidOf(const std::optional<Ref>& in, OfGroup&& ofGroup, ExteriorGroups&& exteriorGroups,
                               OfGround&& ofGround) {
    if (in) return ofGroup(*in);
    for (const Ref& ref : exteriorGroups()) {
        if (auto l = ofGroup(ref)) return l;
    }
    return ofGround();
}

/// Whether a group is liquid throughout (0x007c8360): it has a LiquidType
/// (+0x144) and no MLIQ tiles (+0x11c, +0x120) - the underwater caves and the
/// water outside the Deeprun Tram's glass.
inline bool groupFilled(uint32_t liquidType, uint32_t xTiles, uint32_t yTiles) {
    return liquidType != 0 && (xTiles == 0 || yTiles == 0);
}

/// Whether a point can be in any liquid at all, before its place is looked
/// for: it is under the highest surface there, or a group liquid throughout is
/// about.
inline bool mayBeInLiquid(const std::optional<float>& highestSurface, float pointZ, bool filledGroupsLoaded) {
    constexpr float kSlack = 0.01f;  // 0x007a0820's
    return filledGroupsLoaded || (highestSurface && pointZ < *highestSurface + kSlack);
}

}  // namespace wowee::rendering::liquid_place

#pragma once

/// Whether the player stands outdoors, as 0x0077fbf0 answers it for
/// 0x00739650's ONLY_INDOORS, ONLY_OUTDOORS and NO_MOUNTS_ALLOWED
/// (0x0071b7f0). Arithmetic only: the renderer finds the place.

#include <cstdint>

namespace wowee::game::outdoors {

/// What 0x0077fbf0 reads of where the player is.
struct Place {
    bool known = false;      ///< worked out this session (the player's +0xb8 set)
    /// 0x007a13e0: the first building group the player is linked to
    /// (0x007c2a70), with its MOGP flags (+0x30) and its WMOAreaTable row's
    /// Flags (0x00990560, +0x24) where it has one.
    bool inGroup = false;
    uint32_t groupFlags = 0;
    bool hasRow = false;
    uint32_t rowFlags = 0;
    uint32_t groundArea = 0;  ///< 0x007a0490: the terrain chunk's area under the player
};

/// 0x0077fbf0: in a building group, its WMOAreaTable row's 0x4 outdoors and
/// 0x2 indoors, else the group's exterior flag (0x8); out of one, the
/// ground's AreaTable Flags - 0x4000000 outdoors, 0x2000000 indoors - and
/// outdoors where they say neither. `areaFlags` are the ground area's.
[[nodiscard]] constexpr bool outdoors(const Place& p, uint32_t areaFlags) {
    if (p.inGroup) {
        if (p.hasRow && (p.rowFlags & 0x4u) != 0) return true;
        if (p.hasRow && (p.rowFlags & 0x2u) != 0) return false;
        return (p.groupFlags & 0x8u) != 0;
    }
    if ((areaFlags & 0x4000000u) != 0) return true;
    return (areaFlags & 0x2000000u) == 0;
}

}  // namespace wowee::game::outdoors

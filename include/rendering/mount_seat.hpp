#pragma once

// A unit on its mount (CGUnit_C, 0x0073d5d0): how big the mount is drawn,
// where the rider sits on it, and which pose the rider holds there (+0xb7c).

#include <cstdint>

namespace wowee::rendering::mount_seat {

/// AnimationData Mount: the rider's pose unless a mount aura's kit names
/// another (+0xb7c, set to 0x5b by the unit's constructor and 0x0071e930).
inline constexpr uint32_t kRiderPoseMount = 91;

/// The mount's size. 0x0073d5d0 keeps the mount display's
/// CreatureDisplayInfo scale (+0x10) at +0x990, and the unit's model scale
/// (0x0071c0e0) is multiplied by it while it rides: the mount is the unit's
/// own size times its display's. The rider hangs on the seat under the
/// inverse (0x004e26f0(1 / +0x990)), so it keeps the unit's own size. Not
/// positive reads 1.
inline float mountModelScale(float unitScale, float mountDisplayScale) {
    if (!(unitScale > 0.0f)) unitScale = 1.0f;
    if (!(mountDisplayScale > 0.0f)) mountDisplayScale = 1.0f;
    return unitScale * mountDisplayScale;
}

/// The seat's height over the mount's feet: the seat attachment's height
/// in the model, by the mount's size.
inline float seatHeight(float modelSeatZ, float mountScale) {
    return modelSeatZ * (mountScale > 0.0f ? mountScale : 1.0f);
}

/// 0x00724820: an aura with a Mounted (78) effect coming onto the unit sets
/// its rider pose to the AnimID (SpellVisualKit +8) of its first visual's
/// StateKit (SpellVisual +0x10), where that is not negative.
inline uint32_t riderPoseOnMountAura(uint32_t current, int32_t stateKitAnim) {
    return stateKitAnim >= 0 ? static_cast<uint32_t>(stateKitAnim) : current;
}

/// 0x0071e930: the aura going from the active player puts the pose back to
/// Mount where it was that kit's. Other units keep theirs.
inline uint32_t riderPoseOnMountAuraGone(uint32_t current, int32_t stateKitAnim, bool activePlayer) {
    if (activePlayer && stateKitAnim >= 0 && current == static_cast<uint32_t>(stateKitAnim))
        return kRiderPoseMount;
    return current;
}

}  // namespace wowee::rendering::mount_seat

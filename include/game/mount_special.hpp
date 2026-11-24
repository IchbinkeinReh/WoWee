#pragma once

#include <cstdint>

// What the jump key does on a mount.
namespace wowee::game {

enum class JumpKey : uint8_t { Jump, MountSpecial, Nothing };

struct JumpKeyInput {
    bool mounted = false;        ///< +0x9c0
    uint32_t mountFlags = 0;     ///< the mount display's CreatureModelData Flags
    uint32_t moveFlags = 0;      ///< +0x7cc
    bool floating = false;       ///< no gravity (0x006e9ad0)
    bool deep = false;           ///< deepInLiquid
};

/// 0x00718a20: the liquid stands over the feet by more than half the body.
inline bool deepInLiquid(float liquidOverFeet, float collisionHeight) {
    return liquidOverFeet > collisionHeight * 0.5f;
}

/// 0x0072eb80: a mount that stands rears (animation 94, CMSG_MOUNTSPECIAL_ANIM)
/// instead of jumping - one that can fly or whose model is flagged 0x400 jumps,
/// as one that goes anywhere does; turning on the spot, or with the water up to
/// its middle, it does nothing.
inline JumpKey jumpKey(const JumpKeyInput& in) {
    constexpr uint32_t kJumpingMount = 0x400, kCanFly = 0x1000000, kGoing = 0xf, kTurning = 0x30;
    if (!in.mounted || (in.mountFlags & kJumpingMount) != 0 || (in.moveFlags & kCanFly) != 0) return JumpKey::Jump;
    if ((in.moveFlags & kGoing) != 0 && !in.floating) return JumpKey::Jump;
    if ((in.moveFlags & kTurning) != 0 || in.deep) return JumpKey::Nothing;
    return JumpKey::MountSpecial;
}

}  // namespace wowee::game

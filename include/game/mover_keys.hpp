#pragma once

#include <cstdint>

namespace wowee::game {

/// Whether a mover's keys may do anything (0x005fa060): alive, and not knocked
/// down - AnimationData's only rows with BodyFlags 0x80 (0x0071b000). Its spline
/// and its seat (0x0074ba40) are asked apart.
[[nodiscard]] constexpr bool keysTakeEffect(int32_t health, uint32_t animation) {
    constexpr uint32_t kKnockdown = 121, kFlyKnockdown = 350;
    return health > 0 && animation != kKnockdown && animation != kFlyKnockdown;
}

/// Whether they may move it (0x005fac90): nor stood dead - nor does a dead one jump
/// (0x005fbf80). Its root (0x800) is asked apart.
[[nodiscard]] constexpr bool keysMayMove(int32_t health, uint8_t standState, uint32_t animation = 0) {
    constexpr uint8_t kStandDead = 7;
    return keysTakeEffect(health, animation) && standState != kStandDead;
}

/// Whether they may turn it (0x005fa0d0): not stunned; and whether the mouse may
/// (0x005fa110): nor sitting, kneeling or stood dead.
[[nodiscard]] constexpr bool keysMayTurn(int32_t health, uint32_t unitFlags, uint32_t animation = 0) {
    constexpr uint32_t kUnitFlagStunned = 0x40000;
    return keysTakeEffect(health, animation) && (unitFlags & kUnitFlagStunned) == 0;
}
[[nodiscard]] constexpr bool mouseMayTurn(int32_t health, uint32_t unitFlags, uint8_t standState,
                                          uint32_t animation = 0) {
    return keysMayTurn(health, unitFlags, animation) && standState == 0;
}

}  // namespace wowee::game

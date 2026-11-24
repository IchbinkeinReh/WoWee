#pragma once

#include <cstdint>

namespace wowee::game {

/// Whether a mover's keys may move it (0x005fac90): alive and not stood dead,
/// nor does a dead one jump (0x005fbf80). Its root (0x800) and its seat (0x0074b8b0) are asked apart.
[[nodiscard]] constexpr bool keysMayMove(int32_t health, uint8_t standState) {
    constexpr uint8_t kStandDead = 7;
    return health > 0 && standState != kStandDead;
}

}  // namespace wowee::game

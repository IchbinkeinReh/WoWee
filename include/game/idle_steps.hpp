#pragma once

#include <cstdint>

namespace wowee::game {

/// What 0x0052af40 asks once a frame about a player at whose client nothing has been
/// pressed or moved for `idleMs`.
struct IdlePlayer {
    uint32_t idleMs = 0;
    bool standing = false;    ///< UNIT_FIELD_BYTES_1's stand state is 0
    bool fighting = false;    ///< UNIT_FLAG_IN_COMBAT
    bool mounted = false;     ///< 0x0051a230
    bool walking = false;     ///< a click-to-move under way (0x00721f90)
    bool onTaxi = false;      ///< UNIT_FLAG_TAXI_FLIGHT
    bool away = false;        ///< 0x00bcefec
    bool loggingOut = false;  ///< 0x006b0930
};

/// After five minutes it sits down and says it is away, after thirty it logs out.
struct IdleSteps {
    bool sit = false, away = false, logout = false;
};

[[nodiscard]] constexpr IdleSteps idleSteps(const IdlePlayer& p) {
    constexpr uint32_t kAwayMs = 300000, kLogoutMs = 1800000;
    if (p.idleMs < kAwayMs) return {};
    if (p.idleMs >= kLogoutMs) return {.logout = !p.loggingOut};
    return {.sit = p.standing && !p.fighting && !p.mounted && !p.walking, .away = !p.onTaxi && !p.away};
}

}  // namespace wowee::game

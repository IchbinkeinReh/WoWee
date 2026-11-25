#pragma once

#include <cmath>

namespace wowee::rendering {

/// How far a drunk player's facing sways in an update (0x006cf3d0), in radians: `drunk` from 0
/// to 1, the time in milliseconds.
[[nodiscard]] inline float drunkSway(double milliseconds, float drunk) {
    if (!(drunk > 0.0f)) return 0.0f;
    const double x = milliseconds * 0.017453292 * static_cast<double>(drunk);
    const double waves = std::cos(0.154 * x) + std::cos(0.195 * x) + std::cos(0.093 * x);
    return static_cast<float>(static_cast<double>(drunk) * 0.01308997 * waves * 0.333333);
}

/// The client sways once an update; an update here is a sixtieth of a second, however many
/// frames that is.
inline constexpr float kDrunkUpdatesPerSecond = 60.0f;

}  // namespace wowee::rendering

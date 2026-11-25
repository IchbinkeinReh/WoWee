#pragma once

#include <cmath>
#include <cstdint>

// How much of a unit's model is seen: its display's share (CreatureDisplayInfo
// CreatureModelAlpha) and a spell kit's on top of it.

namespace wowee::rendering::unit_alpha {

/// The unit's own share (0x00715b50, vtable +0xe8).
[[nodiscard]] constexpr float ofDisplay(uint32_t creatureModelAlpha) {
    return static_cast<float>(creatureModelAlpha) * 0.003921569f;
}

/// What the unit's alpha goes to (0x0071abe0): its display's, times a kit's.
[[nodiscard]] constexpr float target(float display, float kit) { return display * kit; }

/// 0x00744030: kept as a byte, and at once where it is there already or no
/// time is given.
[[nodiscard]] inline bool atOnce(float now, float to, float seconds) {
    return std::lround(to * 255.0f) == std::lround(now * 255.0f) || !(seconds > 0.0f);
}

}  // namespace wowee::rendering::unit_alpha

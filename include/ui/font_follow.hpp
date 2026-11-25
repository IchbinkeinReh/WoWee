#pragma once

#include <cstdint>

/// What a text, or a font, takes from the font object it was given - and takes again as
/// that changes (0x004963f0) - but for what it has set itself (0x00483030: the mask at
/// +0x08; 0x004a3a50 and its neighbours take a bit out of it).
namespace wowee::ui::font_follow {

inline constexpr uint8_t kFont = 1;      ///< face, height and flags
inline constexpr uint8_t kStyle = 2;     ///< justification and how lines are broken
inline constexpr uint8_t kColor = 4;
inline constexpr uint8_t kShadow = 8;
inline constexpr uint8_t kSpacing = 16;
inline constexpr uint8_t kAll = 31;

/// What changes reach one that has set `own` itself.
[[nodiscard]] constexpr uint8_t taken(uint8_t changed, uint8_t own) {
    return static_cast<uint8_t>(changed & ~own & kAll);
}

}  // namespace wowee::ui::font_follow

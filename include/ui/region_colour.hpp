#pragma once

#include <algorithm>
#include <cmath>

namespace wowee::ui::region_colour {

/// A part of a colour as a script hands it over (0x00960420, 0x0048bd20): held to 0..1, what
/// is no number 0, and kept as a byte, half a one up.
inline float part(double v) {
    if (!(v > 0.0)) return 0.0f;
    if (v >= 1.0) return 1.0f;
    return static_cast<float>(static_cast<int>(static_cast<double>(static_cast<float>(v)) * 255.0 + 0.5)) / 255.0f;
}

/// The byte a part is.
inline int byteOf(float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

/// A part as the colours' getters answer it (the float at 0x009ead78, not quite a 255th).
inline double answered(float v) { return static_cast<double>(byteOf(v)) * static_cast<double>(0.00392156f); }

/// A shadow's alpha under words of this alpha: never more than theirs (0x006c7440,
/// 0x006c6c30). Both times the frame's.
inline float shadow(float own, float words) { return std::min(own, words); }

}  // namespace wowee::ui::region_colour

#pragma once

// The least the ray traced lighting may darken a surface to: the Ray Tracing
// page's "Minimum brightness (%)". The traced terms scale the picture drawn
// without them (rt_apply: dst * outMul + outAdd), and the scale multiplies
// the ambient by the traced occlusion, the sun's shade and the baked colour's
// shade together, so a corner all three find dark goes to black whatever the
// ambient. The setting is a floor on that scale - outMul held to at least
// this factor - so a lit surface is as it was and only what the trace takes
// near black is lifted. 0 is the tracing as it always was.

#include <algorithm>

namespace wowee::rendering {

inline constexpr int kRtMinBrightnessMaxPercent = 50;

/// The factor the traced scale is held to at least, for a percentage of the
/// picture without the tracing: 0 to 0.5.
[[nodiscard]] inline constexpr float rtMinBrightnessFactor(int percent) {
    return static_cast<float>(std::clamp(percent, 0, kRtMinBrightnessMaxPercent)) / 100.0f;
}

/// The traced scale of one channel under that floor (rt_apply.glsli).
[[nodiscard]] inline constexpr float rtFlooredScale(float scale, float factor) {
    return std::max(scale, factor);
}

}  // namespace wowee::rendering

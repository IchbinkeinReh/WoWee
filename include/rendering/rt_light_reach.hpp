#pragma once

// How far the ray traced point lights are shown: the Ray Tracing page's
// "RT light distance (%)" (U18, the user's wish; not in the original client).
// A share of the view distance the world is drawn to. The trace has always
// gathered its lights out to kRtLightReachCap yards (a light whose reach
// comes within it), and that stays the most: 100 % is the picture as it was,
// whatever the view distance. Below the cap a light is shown by where it
// stands: full out to kRtLightReachFadeFrom of the distance, fading to none at
// it, so it does not pop as the camera moves.

#include <algorithm>
#include <glm/glm.hpp>

namespace wowee::rendering {

inline constexpr int kRtLightReachMinPercent = 1;
inline constexpr int kRtLightReachMaxPercent = 100;
inline constexpr int kRtLightReachDefaultPercent = 100;
/// The yards the lights were always gathered to.
inline constexpr float kRtLightReachCap = 120.0f;
/// Where the fade to the cut starts, as a share of the cut.
inline constexpr float kRtLightReachFadeFrom = 0.85f;

/// The yards the lights are shown to: the percentage of the view distance,
/// held to the cap.
[[nodiscard]] inline float rtLightReach(int percent, float viewDistance) {
    const int p = std::clamp(percent, kRtLightReachMinPercent, kRtLightReachMaxPercent);
    return std::min(kRtLightReachCap, std::max(viewDistance, 0.0f) * static_cast<float>(p) / 100.0f);
}

/// How much of a light standing `distance` from the eye is shown under a
/// cut at `reach` yards: 1 when the cut is the cap (the lights as always),
/// else 1 out to the fade, falling smoothly to 0 at the cut and beyond.
[[nodiscard]] inline float rtLightReachShare(float distance, float reach) {
    if (reach >= kRtLightReachCap) return 1.0f;
    if (!(distance < reach)) return 0.0f;
    const float from = reach * kRtLightReachFadeFrom;
    if (distance <= from) return 1.0f;
    const float t = (distance - from) / (reach - from);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

}  // namespace wowee::rendering

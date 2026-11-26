#pragma once

// How far the ray traced point lights are shown: the Ray Tracing page's
// "RT light distance (%)" (U18, the user's wish "in % ... im Verhältnis zur
// eingestellten Sichtweite"; not in the original client). A share of the view
// distance the world is drawn to: 100 % is the whole view distance (the
// coordinator's reading of the wish; the old fixed 120 yards are gone). A
// light is shown by where it stands: full out to kRtLightReachFadeFrom of the
// distance, fading to none at it, so it does not pop as the camera moves.

#include <algorithm>
#include <glm/glm.hpp>

namespace wowee::rendering {

inline constexpr int kRtLightReachMinPercent = 1;
inline constexpr int kRtLightReachMaxPercent = 100;
inline constexpr int kRtLightReachDefaultPercent = 100;
/// Where the fade to the cut starts, as a share of the cut.
inline constexpr float kRtLightReachFadeFrom = 0.85f;

/// The yards the lights are shown to: the percentage of the view distance.
[[nodiscard]] inline float rtLightReach(int percent, float viewDistance) {
    const int p = std::clamp(percent, kRtLightReachMinPercent, kRtLightReachMaxPercent);
    return std::max(viewDistance, 0.0f) * static_cast<float>(p) / 100.0f;
}

/// How much of a light standing `distance` from the eye is shown under a
/// cut at `reach` yards: 1 out to the fade, falling smoothly to 0 at the cut
/// and beyond.
[[nodiscard]] inline float rtLightReachShare(float distance, float reach) {
    if (!(distance < reach)) return 0.0f;
    const float from = reach * kRtLightReachFadeFrom;
    if (distance <= from) return 1.0f;
    const float t = (distance - from) / (reach - from);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

}  // namespace wowee::rendering

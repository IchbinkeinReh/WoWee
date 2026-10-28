#pragma once

// Rays a pixel of each kind the ray traced lighting casts (the Ray Tracing
// settings, RtLighting::FrameInputs::rays): toward the sun, for the
// occlusion, toward a point light, and a probe's rays a direction for the
// bounced light. One of each is the trace as it always ran; more average
// that many samples a pixel, at that many times the kind's cost, for less
// noise before the temporal and spatial filters.

namespace wowee::rendering {

inline constexpr int kMaxRtRaysPerPixel = 5;

struct RtRaysPerPixel {
    int sun = 1;
    int occlusion = 1;
    int pointLights = 1;
    int bounce = 1;
};

/// Held to 1..kMaxRtRaysPerPixel, as the pass block takes it.
[[nodiscard]] inline constexpr int clampRtRays(int n) {
    return n < 1 ? 1 : (n > kMaxRtRaysPerPixel ? kMaxRtRaysPerPixel : n);
}

}  // namespace wowee::rendering

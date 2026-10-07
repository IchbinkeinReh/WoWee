#pragma once

#include <cstdint>

// Rendering-domain constants: distances, LOD thresholds, particle tuning.

namespace wowee {
namespace rendering {

// ---------------------------------------------------------------------------
// M2 culling geometry
// ---------------------------------------------------------------------------
constexpr float M2_PADDED_RADIUS_SCALE       = 1.5f;
constexpr float M2_PADDED_RADIUS_MIN_MARGIN  = 3.0f;

// ---------------------------------------------------------------------------
// M2 animation timing (milliseconds)
// ---------------------------------------------------------------------------
constexpr float M2_DEFAULT_PARTICLE_ANIM_MS    = 3333.0f;

// ---------------------------------------------------------------------------
// HiZ occlusion culling
// ---------------------------------------------------------------------------
// VP matrix diff threshold - below this HiZ is considered safe.
// Typical tracking camera (following a walking character) produces 0.05–0.25.
constexpr float HIZ_VP_DIFF_THRESHOLD = 0.5f;

// ---------------------------------------------------------------------------
// Character rendering
// ---------------------------------------------------------------------------
// Default frustum-cull radius when model bounds are unavailable (world units).
// 4.0 covers Tauren, mounted characters, and most creature models.

} // namespace rendering
} // namespace wowee

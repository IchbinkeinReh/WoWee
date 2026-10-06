#pragma once

#include <cstdint>

// Rendering-domain constants: distances, LOD thresholds, particle tuning.

namespace wowee {
namespace rendering {

// ---------------------------------------------------------------------------
// M2 instance-count → render-distance mapping
// ---------------------------------------------------------------------------
constexpr uint32_t M2_HIGH_DENSITY_INSTANCE_THRESHOLD = 2000;
constexpr float    M2_MAX_RENDER_DISTANCE_HIGH_DENSITY = 800.0f;
constexpr float    M2_MAX_RENDER_DISTANCE_LOW_DENSITY  = 2800.0f;

// ---------------------------------------------------------------------------
// M2 distance thresholds (world units)
// ---------------------------------------------------------------------------
// Flying ambient models are drawn out to this range: their flight path is
// baked into bone animation, and they read from much further away.
constexpr float M2_SKY_BIRD_MAX_RENDER_DISTANCE = 320.0f;

// ---------------------------------------------------------------------------
// M2 culling geometry
// ---------------------------------------------------------------------------
constexpr float M2_CULL_RADIUS_SCALE_DIVISOR = 12.0f;
constexpr float M2_PADDED_RADIUS_SCALE       = 1.5f;
constexpr float M2_PADDED_RADIUS_MIN_MARGIN  = 3.0f;

// Distance floor for server game objects (mailboxes, chests, nodes, doors).
// The adaptive doodad distance collapses to its densest-scene value in any
// populated area - a city holds ~100k M2 instances, so ambient doodads are cut
// at 300 * viewDistanceScale, i.e. 200 world units at a mid view-distance
// setting. Applying that to game objects hides objects the player interacts
// with. The server only sends a game object when it is inside its own
// visibility radius, so anything we have been told about is worth drawing:
// this floor sits comfortably beyond any server visibility setting, leaving
// frustum and occlusion culling to do the real work.
constexpr float M2_GAME_OBJECT_MIN_RENDER_DISTANCE = 600.0f;

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

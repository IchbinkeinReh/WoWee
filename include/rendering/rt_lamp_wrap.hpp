#pragma once

// How far past the terminator the ray traced lamps' light wraps, N.L taken
// as (N.L + w) / (1 + w) (rt_trace.comp). A lamp is a flame, a glass, a
// glow, not a point: a face turned just past one still takes a little of its
// light. The Ray Tracing page's "Softer lamp light" switch takes it further,
// for a flatter, softer lamp light that also reaches faces turned up to some
// thirty degrees away (the bevels and steps a low candle stands beside).

namespace wowee::rendering {

inline constexpr float kLampWrap = 0.25f;     // faces up to some 15 degrees past
inline constexpr float kLampWrapSoft = 0.5f;  // up to some 30

[[nodiscard]] inline constexpr float lampWrap(bool softer) { return softer ? kLampWrapSoft : kLampWrap; }

/// The wrapped N.L the trace lights a face by; at or under 0 the lamp gives
/// it nothing.
[[nodiscard]] inline constexpr float wrappedNdotL(float ndl, float wrap) { return (ndl + wrap) / (1.0f + wrap); }

}  // namespace wowee::rendering

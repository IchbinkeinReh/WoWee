#pragma once

// Which units the ray traced lighting takes as shadow casters
// (CharacterRenderer::syncRtScene).

namespace wowee::rendering::rt_unit_cast {

/// A unit drawn see-through as a whole - opacity times its spell kits'
/// alpha under 1, as character_renderer.cpp's instanceDrawnFaded has it
/// (the camera's own fade aside) - casts no traced shadow. It is in no
/// G-buffer and is lit by the traced light of what stands behind it; its
/// own shadow there would darken it.
inline constexpr float kOpaque = 0.999f;

inline bool castsTracedShadow(float opacity, float kitAlpha) { return opacity * kitAlpha >= kOpaque; }

}  // namespace wowee::rendering::rt_unit_cast
